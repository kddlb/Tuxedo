#pragma once

#include "core/chain/dsp_node.hpp"
#include "core/chain/faded_buffer.hpp"

#include <condition_variable>
#include <mutex>

namespace tuxedo {

class DSPFaderNode : public DSPNode {
public:
	explicit DSPFaderNode(Node *previous = nullptr, double latency_seconds = 0.10);
	~DSPFaderNode() override = default;

	void process() override;

	void reset_buffer();
	bool pause_and_wait();
	void resume_with_fade_in();
	void apply_output_fade(float *samples, size_t frames, StreamFormat format);

	bool paused() const;
	bool fading() const;
	double timestamp() const;

private:
	static constexpr double kFadeTimeMs = 200.0;
	static constexpr size_t kChunkFrames = 512;

	void begin_fade_locked(float target_level);

	mutable std::mutex state_mtx_;
	std::condition_variable state_cv_;
	FadedBuffer fader_;
	StreamFormat format_{};
	double timestamp_ = 0.0;
	bool paused_ = false;
	bool pause_requested_ = false;
	bool output_paused_ = false;
};

} // namespace tuxedo
