#include "core/chain/output_node.hpp"

#include "core/chain/dsp_fader_node.hpp"
#include "plugin/output/miniaudio_backend.hpp"

#include <algorithm>
#include <cstring>

namespace tuxedo {

OutputNode::OutputNode() = default;

OutputNode::~OutputNode() { close(); }

bool OutputNode::open(StreamFormat format) {
	close();
	format_ = format;
	backend_ = std::make_unique<MiniaudioBackend>();
	auto render_cb = [this](float *dst, size_t frames) { render(dst, frames); };
	if(!backend_->open(format_, render_cb)) {
		backend_.reset();
		return false;
	}
	return true;
}

void OutputNode::close() {
	if(backend_) {
		backend_->close();
		backend_.reset();
	}
	frames_played_.store(0);
	paused_.store(false);
	previous_.store(nullptr);
	next_source_.store(nullptr);
	std::lock_guard<std::mutex> g(leftover_mtx_);
	leftover_ = {};
}

void OutputNode::start() {
	if(backend_) backend_->start();
}

void OutputNode::pause() {
	paused_.store(true);
}

void OutputNode::resume() {
	paused_.store(false);
}

void OutputNode::flush_leftover() {
	std::lock_guard<std::mutex> g(leftover_mtx_);
	leftover_ = {};
}

void OutputNode::set_on_stream_consumed(std::function<void()> cb) {
	std::lock_guard<std::mutex> g(callbacks_mtx_);
	on_stream_consumed_ = std::move(cb);
}

void OutputNode::set_on_stream_advanced(std::function<void()> cb) {
	std::lock_guard<std::mutex> g(callbacks_mtx_);
	on_stream_advanced_ = std::move(cb);
}

double OutputNode::seconds_played() const {
	if(!format_.valid()) return 0.0;
	return double(frames_played_.load()) / format_.sample_rate;
}

namespace {
void copy_scaled(float *dst, const float *src, size_t samples, float vol) {
	for(size_t i = 0; i < samples; ++i) dst[i] = src[i] * vol;
}
} // namespace

void OutputNode::render(float *dst, size_t frames) {
	const uint32_t ch = format_.channels;
	const size_t total_samples = frames * ch;

	Node *cur = previous_.load();
	if(paused_.load() || !cur) {
		std::memset(dst, 0, total_samples * sizeof(float));
		return;
	}

	size_t filled = 0;
	const float vol = static_cast<float>(volume_.load());

	// Drain the stashed leftover fragment first.
	{
		std::lock_guard<std::mutex> g(leftover_mtx_);
		if(!leftover_.empty()) {
			const size_t src_frames = leftover_.frame_count();
			const size_t want = std::min(src_frames, frames - filled);
			copy_scaled(dst + filled * ch, leftover_.samples().data(), want * ch, vol);
			filled += want;
			if(want == src_frames) leftover_ = {};
			else leftover_.remove_frames(want);
		}
	}

	bool advanced_to_next = false;
	bool hit_natural_end = false;

	while(filled < frames) {
		AudioChunk chunk = cur->read_chunk(frames - filled);
		if(chunk.empty()) {
			// Current source drained. Two possibilities:
			//   (a) genuine underrun — leave silence.
			//   (b) end-of-stream — try to hot-swap to the queued source.
			if(!cur->end_of_stream()) break;

			Node *nxt = next_source_.exchange(nullptr);
			if(!nxt) { hit_natural_end = true; break; }

			previous_.store(nxt);
			cur = nxt;
			advanced_to_next = true;
			continue;
		}

		const size_t src_frames = chunk.frame_count();
		const size_t want = std::min(src_frames, frames - filled);
		copy_scaled(dst + filled * ch, chunk.samples().data(), want * ch, vol);
		filled += want;

		if(want < src_frames) {
			chunk.remove_frames(want);
			std::lock_guard<std::mutex> g(leftover_mtx_);
			leftover_ = std::move(chunk);
		}
	}

	if(filled < frames) {
		std::memset(dst + filled * ch, 0, (frames - filled) * ch * sizeof(float));
	}

	if(auto *fader = dynamic_cast<DSPFaderNode *>(cur)) {
		fader->apply_output_fade(dst, frames, format_);
	}

	frames_played_.fetch_add(static_cast<int64_t>(filled));

	// Fire notifications *outside* the audio hot path loop, but still on
	// the audio thread. Callbacks are expected to be non-blocking
	// (condvar notify_one is all they need to do).
	if(advanced_to_next || hit_natural_end) {
		std::function<void()> cb1, cb2;
		{
			std::lock_guard<std::mutex> g(callbacks_mtx_);
			if(advanced_to_next) cb1 = on_stream_advanced_;
			if(hit_natural_end) cb2 = on_stream_consumed_;
		}
		if(cb1) cb1();
		if(cb2) cb2();
	}
}

} // namespace tuxedo
