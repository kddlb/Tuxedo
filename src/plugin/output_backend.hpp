// OutputBackend: abstract audio-device sink. Replaces Cog's
// OutputCoreAudio / OutputAVFoundation pair with a portable interface.
#pragma once

#include "core/format.hpp"

#include <cstddef>
#include <functional>

namespace tuxedo {

// Called on the audio thread. Must fill `frames * format.channels` float32
// samples, interleaved, into `dst`.
using RenderCallback = std::function<void(float *dst, size_t frames)>;

class OutputBackend {
public:
	virtual ~OutputBackend() = default;

	virtual bool open(StreamFormat format, RenderCallback cb) = 0;
	virtual void close() = 0;

	virtual void start() = 0;
	virtual void stop() = 0;
};

} // namespace tuxedo
