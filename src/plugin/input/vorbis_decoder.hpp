// Ogg Vorbis decoder via libvorbisfile. Structurally mirrors
// OpusDecoder — Vorbis comments land through the shared
// vorbis_common helpers (so the rename rules stay aligned across
// FLAC/Opus/Vorbis), embedded METADATA_BLOCK_PICTURE is expanded into
// `album_art`, and ReplayGain tags surface unchanged.
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tuxedo {

class VorbisDecoder : public Decoder {
public:
	VorbisDecoder();
	~VorbisDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

	nlohmann::json metadata() const override;

private:
	void parse_info();
	void parse_tags();

	// OggVorbis_File * (opaque — libvorbisfile headers stay in the .cpp).
	void *vf_ = nullptr;
	bool inited_ = false;
	Source *source_ = nullptr;
	DecoderProperties props_{};

	int64_t current_frame_ = 0;

	nlohmann::json vorbis_tags_ = nlohmann::json::object();
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;
};

} // namespace tuxedo
