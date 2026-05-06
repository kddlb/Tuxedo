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

		bool pause_now = false;
		{
			std::lock_guard<std::mutex> g(state_mtx_);
			format_ = chunk.format();
			timestamp_ = chunk.stream_timestamp();
			if(pause_requested_ && !fader_.active() && fader_.fade_target() > 0.0f) {
				begin_fade_locked(0.0f);
			}
		}

		const bool fade_finished = fader_.apply(chunk);
		write_chunk(std::move(chunk));

		{
			std::lock_guard<std::mutex> g(state_mtx_);
			timestamp_ += chunk.duration();
			if(pause_requested_ && fade_finished && fader_.fade_target() == 0.0f) {
				pause_requested_ = false;
				paused_ = true;
				pause_now = true;
			}
			if(fade_finished) state_cv_.notify_all();
		}

		if(pause_now) continue;
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
	state_cv_.notify_all();
}

bool DSPFaderNode::pause_and_wait() {
	{
		std::lock_guard<std::mutex> g(state_mtx_);
		if(paused_) return true;
		flush_buffer();
		pause_requested_ = true;
		begin_fade_locked(0.0f);
		state_cv_.notify_all();
	}

	std::unique_lock<std::mutex> lk(state_mtx_);
	state_cv_.wait(lk, [this] { return !should_continue() || paused_; });
	lk.unlock();

	wait_until_buffered_frames_at_most(0);
	return should_continue();
}

void DSPFaderNode::resume_with_fade_in() {
	{
		std::lock_guard<std::mutex> g(state_mtx_);
		if(!paused_) return;
		paused_ = false;
		pause_requested_ = false;
		begin_fade_locked(1.0f);
		state_cv_.notify_all();
	}

	wait_until_buffered_frames_at_least(1);
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
