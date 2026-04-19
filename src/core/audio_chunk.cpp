#include "core/audio_chunk.hpp"

#include <algorithm>

namespace tuxedo {

AudioChunk AudioChunk::remove_frames(size_t frames) {
	const size_t avail = frame_count();
	const size_t take = std::min(frames, avail);
	const size_t take_samples = take * format_.channels;

	std::vector<float> head(samples_.begin(), samples_.begin() + take_samples);
	samples_.erase(samples_.begin(), samples_.begin() + take_samples);

	AudioChunk out(format_, std::move(head), stream_timestamp_);
	if(format_.sample_rate) stream_timestamp_ += double(take) / format_.sample_rate;
	return out;
}

} // namespace tuxedo
