// Stream format. Simplified from Cog's AudioStreamBasicDescription usage:
// this MVP deals only in interleaved float32, so sample rate and channel
// count are the only negotiable parameters.
#pragma once

#include <cstdint>

namespace tuxedo {

struct StreamFormat {
	uint32_t sample_rate = 0;
	uint32_t channels = 0;

	bool valid() const { return sample_rate > 0 && channels > 0; }
	bool operator==(const StreamFormat &o) const {
		return sample_rate == o.sample_rate && channels == o.channels;
	}
	bool operator!=(const StreamFormat &o) const { return !(*this == o); }
};

} // namespace tuxedo
