// HLS (HTTP Live Streaming) decoder. Parses an .m3u8 playlist, picks
// the highest-bandwidth audio variant from a master playlist, then
// downloads MPEG-TS / fMP4 / AAC segments in a background thread and
// feeds the concatenated bytes to an internal FFmpeg decoder via an
// HlsMemorySource. Ported from Cog's HLSDecoder.
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
#include <memory>

namespace tuxedo {

class HlsMemorySource;
class HlsSegmentManager;
class FfmpegDecoder;

class HlsDecoder : public Decoder {
public:
	HlsDecoder();
	~HlsDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override;
	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;
	nlohmann::json metadata() const override;

private:
	std::unique_ptr<HlsMemorySource> memory_source_;
	std::unique_ptr<HlsSegmentManager> segment_manager_;
	std::unique_ptr<FfmpegDecoder> decoder_;

	bool is_live_ = false;
	double total_duration_ = 0.0;
	int64_t total_frames_ = -1;
	int64_t pending_skip_frames_ = 0;

	std::string source_url_;

	bool reopen_decoder_after_reset();
};

} // namespace tuxedo
