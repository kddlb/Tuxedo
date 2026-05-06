#pragma once

#include <cstdint>
#include <cstddef>

namespace tuxedo {

// Effect: base interface for synchronous, in-place audio processing.
// Called from the audio output thread. Implementation must be thread-safe
// and non-blocking.
class Effect {
public:
	virtual ~Effect() = default;

	// Process interleaved float32 samples in-place.
	// frames: number of audio frames (time slices).
	// channels: number of interleaved channels.
	// sample_rate: the current hardware sample rate.
	virtual void process(float *samples, size_t frames, uint32_t channels, uint32_t sample_rate) = 0;
};

} // namespace tuxedo
