#pragma once

#include "core/format.hpp"

#include <cstddef>
#include <cstdint>

namespace tuxedo {

class DownmixProcessor {
public:
	DownmixProcessor(StreamFormat input_format, uint64_t input_layout, StreamFormat output_format);

	void process(const float *in_buffer, size_t frames, float *out_buffer) const;

private:
	void downmix_to_stereo(const float *in_buffer, size_t frames, float *out_buffer) const;
	void downmix_to_mono(const float *in_buffer, size_t frames, float *out_buffer) const;

	StreamFormat input_format_{};
	uint64_t input_layout_ = 0;
	StreamFormat output_format_{};
};

} // namespace tuxedo
