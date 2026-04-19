// Decoder: turns a Source's bytes into AudioChunks. Mirrors Cog's
// CogDecoder protocol.
#pragma once

#include "core/audio_chunk.hpp"
#include "plugin/source.hpp"

#include <memory>
#include <optional>
#include <string>

namespace tuxedo {

struct DecoderProperties {
	StreamFormat format{};
	int64_t total_frames = -1; // -1 = unknown / infinite
	std::string codec;
};

class Decoder {
public:
	virtual ~Decoder() = default;

	virtual bool open(Source *source) = 0;
	virtual void close() = 0;

	virtual DecoderProperties properties() const = 0;

	// Read up to max_frames frames into `out`. Returns false at EOF.
	virtual bool read(AudioChunk &out, size_t max_frames) = 0;

	// Returns the frame actually seeked to, or -1 if unsupported/failed.
	virtual int64_t seek(int64_t frame) = 0;
};

using DecoderPtr = std::unique_ptr<Decoder>;

} // namespace tuxedo
