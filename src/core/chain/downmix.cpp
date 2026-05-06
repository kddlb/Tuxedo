#include "core/chain/downmix.hpp"

#include "core/channel_layout.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tuxedo {

namespace {

using namespace channel_layout;

bool has(uint64_t layout, uint64_t flag) {
	return (layout & flag) != 0;
}

} // namespace

DownmixProcessor::DownmixProcessor(StreamFormat input_format, uint64_t input_layout, StreamFormat output_format)
: input_format_(input_format),
  input_layout_(input_layout ? input_layout : channel_layout::default_for_channels(input_format.channels)),
  output_format_(output_format) {}

void DownmixProcessor::process(const float *in_buffer, size_t frames, float *out_buffer) const {
	if(!in_buffer || !out_buffer || frames == 0) return;

	if(output_format_.channels == input_format_.channels) {
		std::memcpy(out_buffer, in_buffer, frames * output_format_.channels * sizeof(float));
		return;
	}
	if(output_format_.channels == 2) {
		downmix_to_stereo(in_buffer, frames, out_buffer);
		return;
	}
	if(output_format_.channels == 1) {
		downmix_to_mono(in_buffer, frames, out_buffer);
		return;
	}

	std::fill(out_buffer, out_buffer + frames * output_format_.channels, 0.0f);
}

void DownmixProcessor::downmix_to_stereo(const float *in_buffer, size_t frames, float *out_buffer) const {
	std::fill(out_buffer, out_buffer + frames * 2, 0.0f);

	float front_lr = 0.0f;
	float front_center = 0.0f;
	float lfe = 0.0f;
	float back_lr = 0.0f;
	float back_cross = 0.0f;
	float back_center = 0.0f;
	float side_lr = 0.0f;
	float side_cross = 0.0f;

	if(has(input_layout_, kFrontLeft) || has(input_layout_, kFrontRight)) front_lr = 1.0f;
	if(has(input_layout_, kFrontCenter)) {
		front_lr = 0.5858f;
		front_center = 0.4142f;
	}
	if(has(input_layout_, kBackLeft) || has(input_layout_, kBackRight)) {
		if(has(input_layout_, kFrontCenter)) {
			front_lr = 0.651f;
			front_center = 0.46f;
			back_lr = 0.5636f;
			back_cross = 0.3254f;
		} else {
			front_lr = 0.4226f;
			back_lr = 0.366f;
			back_cross = 0.2114f;
		}
	}
	if(has(input_layout_, kLfe)) {
		front_lr *= 0.8f;
		front_center *= 0.8f;
		lfe = front_center;
		back_lr *= 0.8f;
		back_cross *= 0.8f;
	}
	if(has(input_layout_, kBackCenter)) {
		front_lr *= 0.86f;
		front_center *= 0.86f;
		lfe *= 0.86f;
		back_lr *= 0.86f;
		back_cross *= 0.86f;
		back_center = front_center * 0.86f;
	}
	if(has(input_layout_, kSideLeft) || has(input_layout_, kSideRight)) {
		float ratio = has(input_layout_, kBackCenter) ? 0.85f : 0.73f;
		front_lr *= ratio;
		front_center *= ratio;
		lfe *= ratio;
		back_lr *= ratio;
		back_cross *= ratio;
		back_center *= ratio;
		side_lr = 0.463882352941176f * ratio;
		side_cross = 0.267882352941176f * ratio;
	}

	const auto ordered = channel_layout::ordered_channels(input_layout_, input_format_.channels);
	for(uint32_t channel = 0; channel < input_format_.channels; ++channel) {
		float left_ratio = 0.0f;
		float right_ratio = 0.0f;
		const uint64_t role = channel < ordered.size() ? ordered[channel] : 0;
		switch(role) {
			case kFrontLeft: left_ratio = front_lr; break;
			case kFrontRight: right_ratio = front_lr; break;
			case kFrontCenter: left_ratio = front_center; right_ratio = front_center; break;
			case kLfe: left_ratio = lfe; right_ratio = lfe; break;
			case kBackLeft: left_ratio = back_lr; right_ratio = back_cross; break;
			case kBackRight: left_ratio = back_cross; right_ratio = back_lr; break;
			case kBackCenter: left_ratio = back_center; right_ratio = back_center; break;
			case kSideLeft: left_ratio = side_lr; right_ratio = side_cross; break;
			case kSideRight: left_ratio = side_cross; right_ratio = side_lr; break;
			default:
				if(channel == 0) left_ratio = 1.0f;
				else if(channel == 1) right_ratio = 1.0f;
				else { left_ratio = 0.25f; right_ratio = 0.25f; }
				break;
		}

		for(size_t frame = 0; frame < frames; ++frame) {
			const float sample = in_buffer[frame * input_format_.channels + channel];
			out_buffer[frame * 2] += sample * left_ratio;
			out_buffer[frame * 2 + 1] += sample * right_ratio;
		}
	}
}

void DownmixProcessor::downmix_to_mono(const float *in_buffer, size_t frames, float *out_buffer) const {
	if(input_format_.channels == 1) {
		std::memcpy(out_buffer, in_buffer, frames * sizeof(float));
		return;
	}

	std::vector<float> stereo(frames * 2);
	downmix_to_stereo(in_buffer, frames, stereo.data());
	for(size_t frame = 0; frame < frames; ++frame) {
		out_buffer[frame] = 0.5f * (stereo[frame * 2] + stereo[frame * 2 + 1]);
	}
}

} // namespace tuxedo
