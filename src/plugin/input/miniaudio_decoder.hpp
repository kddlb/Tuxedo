// MVP decoder backed by miniaudio's built-in MP3/FLAC/WAV/Vorbis
// decoders. Reads through our Source interface via user-supplied read
// and seek callbacks so the same plumbing works for future non-file
// sources.
#pragma once

#include "plugin/decoder.hpp"

namespace tuxedo {

class MiniaudioDecoder : public Decoder {
public:
	MiniaudioDecoder();
	~MiniaudioDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

private:
	struct Impl;
	Impl *impl_ = nullptr;
	DecoderProperties props_{};
};

} // namespace tuxedo
