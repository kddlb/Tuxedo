#pragma once

#include "plugin/decoder.hpp"

namespace tuxedo {

class CueDecoder : public Decoder {
public:
	CueDecoder();
	~CueDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }
	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;
	nlohmann::json metadata() const override { return metadata_; }

private:
	SourcePtr wrapped_source_;
	DecoderPtr wrapped_decoder_;
	DecoderProperties props_{};
	nlohmann::json metadata_ = nlohmann::json::object();
	int64_t track_start_ = 0;
	int64_t track_end_ = -1;
	int64_t current_frame_ = 0;
};

} // namespace tuxedo
