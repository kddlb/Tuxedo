// Decoder: turns a Source's bytes into AudioChunks. Mirrors Cog's
// CogDecoder protocol.
#pragma once

#include "core/audio_chunk.hpp"
#include "plugin/source.hpp"

#include <nlohmann/json.hpp>

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
	using MetadataChangedCallback = std::function<void()>;

	virtual ~Decoder() = default;

	virtual bool open(Source *source) = 0;
	virtual void close() = 0;

	virtual DecoderProperties properties() const = 0;

	// Read up to max_frames frames into `out`. Returns false at EOF.
	virtual bool read(AudioChunk &out, size_t max_frames) = 0;

	// Returns the frame actually seeked to, or -1 if unsupported/failed.
	virtual int64_t seek(int64_t frame) = 0;

	// Tag data (Vorbis comments / ID3 / etc) in the canonical shape
	// documented at doc/metadata.md: lowercased keys, multi-value fields
	// as JSON arrays, optional "album_art": {mime, data_b64}. Default is
	// empty; decoders that don't surface tags (miniaudio) just inherit.
	virtual nlohmann::json metadata() const { return nlohmann::json::object(); }
	virtual void set_metadata_changed_callback(MetadataChangedCallback cb) { (void)cb; }
};

using DecoderPtr = std::unique_ptr<Decoder>;

} // namespace tuxedo
