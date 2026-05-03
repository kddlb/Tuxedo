// FLAC decoder, ported from Cog's Plugins/Flac/FlacDecoder.m.
// Scope: raw FLAC streams, any bit depth (8/16/24/32), stereo/multichannel,
// float32 interleaved output, Vorbis-comment + embedded PICTURE metadata.
// OggFLAC, HDCD detection, and live metadata
// (willChangeValueForKey style KVO for ICY streams) are deferred.
#pragma once

#include "plugin/decoder.hpp"

#include <FLAC/format.h>

#include <cstdint>
#include <string>
#include <vector>

// libFLAC's handle type is a typedef of an anonymous struct, so it
// can't be forward-declared — we hold it as void* and cast inside the
// implementation.
namespace tuxedo {

class FlacDecoder : public Decoder {
public:
	FlacDecoder();
	~FlacDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

	nlohmann::json metadata() const override;
	void set_metadata_changed_callback(MetadataChangedCallback cb) override;

	// --- Accessors used by the C callback shims ---
	Source *source() { return source_; }
	bool end_of_stream() const { return end_of_stream_; }
	void set_end_of_stream(bool b) { end_of_stream_ = b; }
	void set_abort() { abort_ = true; }
	int64_t file_size() const { return file_size_; }

	// Called from the libFLAC write callback (on the call stack of
	// process_single). Stashes the decoded block as interleaved float32.
	void accept_block(const int32_t *const *planes, uint32_t blocksize,
	                  uint32_t channels, uint32_t bits_per_sample,
	                  uint32_t sample_rate);

	// Called from the metadata callback when STREAMINFO arrives.
	void accept_streaminfo(uint32_t channels, uint32_t sample_rate,
	                       uint32_t bits_per_sample, uint64_t total_samples);

	// One Vorbis comment entry, with name/value already split and
	// encoded as UTF-8 (libFLAC gives us raw bytes).
	void accept_vorbis_entry(const char *entry, uint32_t length);

	// An embedded PICTURE block. `data` is the raw image bytes.
	void accept_picture(const char *mime, const uint8_t *data, size_t length);
	void accept_cuesheet(const FLAC__StreamMetadata_CueSheet &cue_sheet);

	// Metadata updated on interval, fire callback if registered
	void new_metadata(void);

private:
	void *dec_ = nullptr; // FLAC__StreamDecoder *
	Source *source_ = nullptr;
	DecoderProperties props_{};

	bool has_stream_info_ = false;
	bool end_of_stream_ = false;
	bool abort_ = false;

	uint32_t bits_per_sample_ = 0;
	int64_t file_size_ = 0;

	// One block of decoded samples, interleaved float32, waiting to be
	// copied into the next read() call's AudioChunk.
	std::vector<float> block_;
	size_t block_frames_ = 0;
	size_t block_frames_consumed_ = 0;

	int64_t current_frame_ = 0;

	// Accumulated Vorbis comments. Each key maps to an array of values
	// (single-valued tags still land in a one-element array, for
	// uniformity with Cog's convention).
	nlohmann::json vorbis_tags_ = nlohmann::json::object();

	// Embedded album art, if any. Base64-encoded lazily in metadata().
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;
	std::string cuesheet_text_;

	// Interval metadata updated
	MetadataChangedCallback metadata_changed_cb_;
};

} // namespace tuxedo
