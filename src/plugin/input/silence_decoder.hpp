#pragma once

#include "plugin/decoder.hpp"

namespace tuxedo {

class SilenceDecoder : public Decoder {
public:
	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }
	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

private:
	DecoderProperties props_{};
	int64_t current_frame_ = 0;
};

} // namespace tuxedo
