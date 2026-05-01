#pragma once

#include "plugin/decoder.hpp"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;
struct SwrContext;

namespace tuxedo {

class FfmpegDecoder : public Decoder {
public:
	FfmpegDecoder();
	~FfmpegDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }
	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;
	nlohmann::json metadata() const override;

private:
	bool decode_more();
	bool append_decoded_frame();
	bool drain_resampler();
	void rebuild_metadata();

	Source *source_ = nullptr;
	DecoderProperties props_{};

	AVFormatContext *format_ctx_ = nullptr;
	AVCodecContext *codec_ctx_ = nullptr;
	AVIOContext *io_ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	SwrContext *swr_ = nullptr;

	int stream_index_ = -1;
	int attached_picture_index_ = -1;
	bool packet_eof_ = false;
	bool audio_eof_ = false;

	std::vector<float> pending_samples_;
	size_t pending_offset_ = 0;
	double pending_timestamp_ = 0.0;
	int64_t current_frame_ = 0;

	nlohmann::json base_metadata_ = nlohmann::json::object();
	std::string picture_mime_;
	std::vector<uint8_t> picture_bytes_;
};

} // namespace tuxedo
