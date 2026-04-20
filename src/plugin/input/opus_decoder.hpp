// Opus decoder via libopusfile. Ported from Cog's
// Plugins/Opus/OpusDecoder.{h,m}. Outputs interleaved float32 at the
// Opus canonical 48 kHz rate; surfaces Vorbis-comment metadata and
// embedded METADATA_BLOCK_PICTURE artwork using the same JSON shape as
// FlacDecoder so clients can consume both uniformly.
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tuxedo {

class OpusDecoder : public Decoder {
public:
	OpusDecoder();
	~OpusDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

	nlohmann::json metadata() const override;

	// Accessor used by the C callback shims.
	Source *source() { return source_; }

private:
	void parse_head();
	void parse_tags();
	void accept_vorbis_entry(const char *name, size_t name_len,
	                         const char *value, size_t value_len);

	// OggOpusFile * (opaque; libopusfile headers in the .cpp only).
	void *of_ = nullptr;
	Source *source_ = nullptr;
	DecoderProperties props_{};

	// Channel remap: for multi-channel Opus, Vorbis order → standard
	// tuxedo order. Indexed by [channels][out_channel] → in_channel.
	int chmap_[8] = {0, 1, 2, 3, 4, 5, 6, 7};

	// Accumulator for decoded interleaved float32 samples. Opus reads
	// tend to come in ~120/240/960-frame chunks; we let the caller of
	// read() take however much it asked for.
	std::vector<float> block_;
	size_t block_frames_ = 0;
	size_t block_frames_consumed_ = 0;

	int64_t current_frame_ = 0;

	// Accumulated metadata.
	nlohmann::json vorbis_tags_ = nlohmann::json::object();
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;

	// R128 gain values in q7.8 centibels (raw from Opus header / tags).
	int32_t r128_header_gain_q8_ = 0;
	int32_t r128_track_gain_q8_ = 0;
	int32_t r128_album_gain_q8_ = 0;
	bool has_track_gain_ = false;
	bool has_album_gain_ = false;
};

} // namespace tuxedo
