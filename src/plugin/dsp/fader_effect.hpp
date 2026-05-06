#pragma once

#include "plugin/dsp/effect.hpp"

#include <atomic>

namespace tuxedo {

// FaderEffect: scales audio samples to perform smooth volume ramps.
// Useful for smooth pause/resume and crossfades.
class FaderEffect : public Effect {
public:
	FaderEffect();
	~FaderEffect() override;

	// Start a ramp to target_level over duration_ms.
	// target_level: 0.0 (silent) to 1.0 (full).
	void fade_to(float target_level, double duration_ms);

	// Immediately set the volume level without ramping.
	void set_level(float level);

	float current_level() const { return current_level_.load(); }

	void process(float *samples, size_t frames, uint32_t channels, uint32_t sample_rate) override;

private:
	std::atomic<float> current_level_{1.0f};
	std::atomic<float> target_level_{1.0f};
	std::atomic<double> duration_ms_{0.0};
	std::atomic<int64_t> remaining_frames_{0};
};

} // namespace tuxedo
