#include "core/chain/faded_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace tuxedo {

namespace {

constexpr float kLevelEpsilon = 1e-6f;

float clamp_level(float level) {
	return std::max(0.0f, std::min(1.0f, level));
}

} // namespace

FadedBuffer::FadedBuffer(float fade_start, float fade_target, double duration_ms) {
	reset(fade_start, fade_target, duration_ms);
}

void FadedBuffer::reset(float fade_start, float fade_target, double duration_ms) {
	fade_level_ = clamp_level(fade_start);
	fade_target_ = clamp_level(fade_target);
	duration_ms_ = std::max(0.0, duration_ms);
	remaining_frames_ = 0;
	active_ = std::abs(fade_level_ - fade_target_) >= kLevelEpsilon;
	if(!active_) fade_level_ = fade_target_;
}

bool FadedBuffer::apply(AudioChunk &chunk) {
	if(chunk.empty()) return !active_;

	const auto fmt = chunk.format();
	if(!fmt.valid() || !fmt.channels) return !active_;

	auto &samples = chunk.samples();
	if(active_ && remaining_frames_ <= 0) {
		remaining_frames_ = std::max<int64_t>(
		    1, static_cast<int64_t>(std::llround(duration_ms_ * fmt.sample_rate / 1000.0)));
	}

	for(size_t frame = 0; frame < chunk.frame_count(); ++frame) {
		for(uint32_t channel = 0; channel < fmt.channels; ++channel) {
			samples[frame * fmt.channels + channel] *= fade_level_;
		}

		if(active_ && remaining_frames_ > 0) {
			fade_level_ += (fade_target_ - fade_level_) / static_cast<float>(remaining_frames_);
			--remaining_frames_;
			if(remaining_frames_ <= 0 || std::abs(fade_level_ - fade_target_) < kLevelEpsilon) {
				fade_level_ = fade_target_;
				remaining_frames_ = 0;
				active_ = false;
			}
		}
	}

	return !active_;
}

} // namespace tuxedo
