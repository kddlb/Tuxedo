#include "core/chain/dsp_fader_node.hpp"

namespace tuxedo {

DSPFaderNode::DSPFaderNode(Node *previous, double latency_seconds)
: DSPNode(previous, latency_seconds), fader_(1.0f, 1.0f, 0.0) {}

void DSPFaderNode::process() {
	while(should_continue()) {
		{
			std::unique_lock<std::mutex> lk(state_mtx_);
			state_cv_.wait(lk, [this] { return !should_continue() || !paused_; });
			if(!should_continue()) break;
		}

		if(!previous_) continue;

		AudioChunk chunk = previous_->read_chunk(kChunkFrames);
		if(chunk.empty()) {
			if(previous_->end_of_stream()) break;
			continue;
		}

		{
			std::lock_guard<std::mutex> g(state_mtx_);
			format_ = chunk.format();
			timestamp_ = chunk.stream_timestamp();
		}

		write_chunk(std::move(chunk));

		{
			std::lock_guard<std::mutex> g(state_mtx_);
			timestamp_ += chunk.duration();
		}
	}
}

void DSPFaderNode::reset_buffer() {
	std::lock_guard<std::mutex> g(state_mtx_);
	flush_buffer();
	fader_.reset(1.0f, 1.0f, 0.0);
	format_ = {};
	timestamp_ = 0.0;
	paused_ = false;
	pause_requested_ = false;
	output_paused_ = false;
	state_cv_.notify_all();
}

bool DSPFaderNode::pause_and_wait() {
	{
		std::lock_guard<std::mutex> g(state_mtx_);
		if(paused_) return true;
		pause_requested_ = true;
		output_paused_ = false;
		begin_fade_locked(0.0f);
		state_cv_.notify_all();
	}

	std::unique_lock<std::mutex> lk(state_mtx_);
	state_cv_.wait(lk, [this] { return !should_continue() || output_paused_; });
	if(output_paused_) {
		paused_ = true;
		flush_buffer();
	}
	lk.unlock();

	wait_until_buffered_frames_at_most(0);
	return should_continue();
}

void DSPFaderNode::resume_with_fade_in() {
	{
		std::lock_guard<std::mutex> g(state_mtx_);
		if(!paused_) return;
		paused_ = false;
		output_paused_ = false;
		pause_requested_ = false;
		begin_fade_locked(1.0f);
		state_cv_.notify_all();
	}

	wait_until_buffered_frames_at_least(1);
}

void DSPFaderNode::apply_output_fade(float *samples, size_t frames, StreamFormat format) {
	std::lock_guard<std::mutex> g(state_mtx_);
	if(!samples || !frames) return;
	const bool fade_finished = fader_.apply(samples, frames, format);
	if(pause_requested_ && fade_finished && fader_.fade_target() == 0.0f) {
		pause_requested_ = false;
		output_paused_ = true;
		state_cv_.notify_all();
	}
}

bool DSPFaderNode::paused() const {
	std::lock_guard<std::mutex> g(state_mtx_);
	return paused_;
}

bool DSPFaderNode::fading() const {
	std::lock_guard<std::mutex> g(state_mtx_);
	return fader_.active();
}

double DSPFaderNode::timestamp() const {
	std::lock_guard<std::mutex> g(state_mtx_);
	return timestamp_;
}

void DSPFaderNode::begin_fade_locked(float target_level) {
	fader_.reset(fader_.current_level(), target_level, kFadeTimeMs);
}

} // namespace tuxedo
