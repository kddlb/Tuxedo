#include "plugin/dsp/fader_effect.hpp"

#include <algorithm>
#include <cmath>

namespace tuxedo {

namespace {

constexpr float kLevelEpsilon = 1e-6f;

float clamp_level(float level) {
	return std::max(0.0f, std::min(1.0f, level));
}

} // namespace

FaderEffect::FaderEffect() = default;
FaderEffect::~FaderEffect() = default;

void FaderEffect::fade_to(float target, double ms) {
	target = clamp_level(target);
	target_level_.store(target);
	if(ms <= 0.0) {
		current_level_.store(target);
		duration_ms_.store(0.0);
		remaining_frames_.store(0);
	} else {
		duration_ms_.store(ms);
		remaining_frames_.store(0);
	}
}

void FaderEffect::set_level(float level) {
	level = clamp_level(level);
	current_level_.store(level);
	target_level_.store(level);
	duration_ms_.store(0.0);
	remaining_frames_.store(0);
}

void FaderEffect::process(float *samples, size_t frames, uint32_t channels, uint32_t sample_rate) {
	float current = current_level_.load();
	float target = target_level_.load();
	int64_t remaining = remaining_frames_.load();

	if(std::abs(current - target) < kLevelEpsilon) {
		current = target;
		if(current < 0.9999f) {
			for(size_t i = 0; i < frames * channels; ++i) {
				samples[i] *= current;
			}
		}
		return;
	}

	if(remaining <= 0) {
		double ms = duration_ms_.load();
		remaining = std::max<int64_t>(1, static_cast<int64_t>(std::llround(ms * sample_rate / 1000.0)));
	}

	for(size_t f = 0; f < frames; ++f) {
		for(uint32_t c = 0; c < channels; ++c) {
			samples[f * channels + c] *= current;
		}

		if(remaining > 0) {
			current += (target - current) / static_cast<float>(remaining);
			--remaining;
		} else {
			current = target;
		}
	}

	if(remaining <= 0 || std::abs(current - target) < kLevelEpsilon) {
		current = target;
		remaining = 0;
	}

	current_level_.store(current);
	remaining_frames_.store(remaining);
}

} // namespace tuxedo
