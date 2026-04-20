// MP3 decoder. Audio via miniaudio's built-in dr_mp3 (same as the
// generic MiniaudioDecoder), tags via libid3tag so we get ID3v2/v1
// title/artist/album/... plus embedded APIC album art and TXXX
// ReplayGain entries. Surfaces metadata in the same canonical shape
// as the Vorbis-comment decoders (FLAC, Opus, Vorbis).
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tuxedo {

class Mp3Decoder : public Decoder {
public:
	Mp3Decoder();
	~Mp3Decoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

	nlohmann::json metadata() const override;

private:
	void read_id3_tags(const std::string &path);

	struct Impl;
	Impl *impl_ = nullptr;
	DecoderProperties props_{};

	nlohmann::json id3_tags_ = nlohmann::json::object();
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;
};

} // namespace tuxedo
