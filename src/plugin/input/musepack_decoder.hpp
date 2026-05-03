// Musepack decoder via libmpcdec. Mirrors Cog's MusepackDecoder with
// tuxedo's Decoder/Source interfaces and exposes stream ReplayGain in
// the existing metadata shape.
#pragma once

#include "plugin/decoder.hpp"

#include <cstdint>
#include <vector>

namespace tuxedo {

class MusepackDecoder : public Decoder {
public:
	MusepackDecoder();
	~MusepackDecoder() override;

	bool open(Source *source) override;
	void close() override;

	DecoderProperties properties() const override { return props_; }

	bool read(AudioChunk &out, size_t max_frames) override;
	int64_t seek(int64_t frame) override;

	nlohmann::json metadata() const override;

private:
	struct Impl;
	Impl *impl_ = nullptr;
	DecoderProperties props_{};

	std::vector<float> block_;
	size_t block_frames_ = 0;
	size_t block_frames_consumed_ = 0;
	int64_t current_frame_ = 0;

	nlohmann::json metadata_ = nlohmann::json::object();
};

} // namespace tuxedo
