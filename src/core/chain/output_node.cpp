#include "core/chain/output_node.hpp"

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

double OutputNode::seconds_played() const {
	if(!format_.valid()) return 0.0;
	return double(frames_played_.load()) / format_.sample_rate;
}

void OutputNode::render(float *dst, size_t frames) {
	const uint32_t ch = format_.channels;
	const size_t total_samples = frames * ch;

	if(paused_.load() || !previous_) {
		std::memset(dst, 0, total_samples * sizeof(float));
		return;
	}

	size_t filled = 0;
	const float vol = static_cast<float>(volume_.load());

	{
		std::lock_guard<std::mutex> g(leftover_mtx_);
		if(!leftover_.empty()) {
			// consume() trims leftover_ in place via remove_frames, so re-issue
			// a simpler inline consumption here that leaves the remainder intact.
			const size_t src_frames = leftover_.frame_count();
			const size_t want = std::min(src_frames, frames - filled);
			const float *sp = leftover_.samples().data();
			float *dp = dst + filled * ch;
			for(size_t i = 0; i < want * ch; ++i) dp[i] = sp[i] * vol;
			filled += want;
			if(want == src_frames) {
				leftover_ = {};
			} else {
				AudioChunk head = leftover_.remove_frames(want);
				(void)head;
			}
		}
	}

	while(filled < frames) {
		AudioChunk chunk = previous_->read_chunk(frames - filled);
		if(chunk.empty()) break;

		const size_t src_frames = chunk.frame_count();
		const size_t want = std::min(src_frames, frames - filled);
		const float *sp = chunk.samples().data();
		float *dp = dst + filled * ch;
		for(size_t i = 0; i < want * ch; ++i) dp[i] = sp[i] * vol;
		filled += want;

		if(want < src_frames) {
			AudioChunk head = chunk.remove_frames(want);
			(void)head;
			std::lock_guard<std::mutex> g(leftover_mtx_);
			leftover_ = std::move(chunk);
		}
	}

	if(filled < frames) {
		std::memset(dst + filled * ch, 0, (frames - filled) * ch * sizeof(float));
	}

	frames_played_.fetch_add(static_cast<int64_t>(filled));
}

} // namespace tuxedo
