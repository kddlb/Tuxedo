// Mirrors Cog's AudioChunk: a bundle of (format, timestamp, samples).
// MVP stores interleaved float32 only; the richer configurations in
// Cog's AudioChunk (channel layouts, HDCD, lossless flag) are deferred.
#pragma once

#include "core/format.hpp"

#include <cstddef>
#include <vector>

namespace tuxedo {

class AudioChunk {
public:
	AudioChunk() = default;
	AudioChunk(StreamFormat f, std::vector<float> samples, double timestamp = 0.0)
	: format_(f), samples_(std::move(samples)), stream_timestamp_(timestamp) {}

	const StreamFormat &format() const { return format_; }
	void set_format(StreamFormat f) { format_ = f; }

	const std::vector<float> &samples() const { return samples_; }
	std::vector<float> &samples() { return samples_; }

	size_t frame_count() const {
		return format_.channels ? samples_.size() / format_.channels : 0;
	}

	double duration() const {
		return format_.sample_rate ? double(frame_count()) / format_.sample_rate : 0.0;
	}

	bool empty() const { return samples_.empty(); }

	double stream_timestamp() const { return stream_timestamp_; }
	void set_stream_timestamp(double t) { stream_timestamp_ = t; }

	// Remove and return the first `frames` frames as a new chunk.
	AudioChunk remove_frames(size_t frames);

private:
	StreamFormat format_{};
	std::vector<float> samples_{};
	double stream_timestamp_ = 0.0;
};

} // namespace tuxedo
