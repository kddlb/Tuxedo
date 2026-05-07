// MP3 decoder. Audio via vendored minimp3 (single-frame and seek-aware
// `mp3dec_ex_*` paths), tags via libid3tag so we get ID3v2/v1
// title/artist/album/... plus embedded APIC album art and TXXX
// ReplayGain entries. Surfaces metadata in the same canonical shape
// as the Vorbis-comment decoders (FLAC, Opus, Vorbis). The streaming
// path supports unseekable sources with no definite length.
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
	void set_metadata_changed_callback(MetadataChangedCallback cb) override;

private:
	bool open_seekable();
	bool open_streaming();
	bool refill_input();
	bool decode_streaming_frame();

	struct Impl;
	Impl *impl_ = nullptr;
	DecoderProperties props_{};

	bool seekable_ = false;
	int64_t total_frames_explicit_ = 0;
	uint32_t start_padding_ = 0;
	uint32_t end_padding_ = 0;
	bool found_itun_smpb_ = false;

	nlohmann::json id3_tags_ = nlohmann::json::object();
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;

	MetadataChangedCallback metadata_changed_cb_;
};

} // namespace tuxedo
