// FLAC decoder, ported from Cog's Plugins/Flac/FlacDecoder.m.
// MVP scope: raw FLAC streams, any bit depth (8/16/24/32), stereo/multichannel,
// float32 interleaved output. OggFLAC, metadata (Vorbis comments / pictures /
// embedded cuesheets) and HDCD detection are deferred.
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
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
};

} // namespace tuxedo
