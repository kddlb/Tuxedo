#pragma once

#include "core/audio_chunk.hpp"

#include <cstdint>

namespace tuxedo {

class FadedBuffer {
public:
	FadedBuffer() = default;
	FadedBuffer(float fade_start, float fade_target, double duration_ms);

	void reset(float fade_start, float fade_target, double duration_ms);
	bool apply(AudioChunk &chunk);

	float current_level() const { return fade_level_; }
	float fade_target() const { return fade_target_; }
	bool active() const { return active_; }

private:
	float fade_level_ = 1.0f;
	float fade_target_ = 1.0f;
	double duration_ms_ = 0.0;
	int64_t remaining_frames_ = 0;
	bool active_ = false;
};

} // namespace tuxedo
