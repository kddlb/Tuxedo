#include "core/chain/converter_node.hpp"

#include "miniaudio.h"

#include <cstdlib>
#include <utility>
#include <vector>

namespace tuxedo {

ConverterNode::ConverterNode() = default;

ConverterNode::~ConverterNode() {
	request_stop();
	join();
	if(converter_) {
		ma_data_converter_uninit(static_cast<ma_data_converter *>(converter_), nullptr);
		std::free(converter_);
		converter_ = nullptr;
	}
}

void ConverterNode::set_target_format(std::optional<StreamFormat> target) {
	target_ = target;
}

void ConverterNode::request_flush() {
	flush_requested_.store(true);
}

void ConverterNode::set_gain(float gain) {
	gain_.store(gain);
}

void ConverterNode::process() {
	if(!previous_) return;

	StreamFormat in_fmt{};
	if(!previous_->peek_format(in_fmt)) return;

	const StreamFormat out_fmt = target_ ? *target_ : in_fmt;
	const bool identity = (!target_) || (*target_ == in_fmt);

	// Identity passthrough: forward chunks unchanged. Covers the
	// common same-format case with zero allocation, zero extra copies.
	if(identity) {
		while(should_continue()) {
			if(flush_requested_.exchange(false)) flush_buffer();
			auto chunk = previous_->read_chunk(4096);
			if(chunk.empty()) break;
			const float gain = gain_.load();
			if(gain != 1.0f) {
				for(float &sample : chunk.samples()) sample *= gain;
			}
			write_chunk(std::move(chunk));
		}
		return;
	}

	// Real conversion.
	ma_data_converter_config cfg = ma_data_converter_config_init(
		ma_format_f32, ma_format_f32,
		in_fmt.channels, out_fmt.channels,
		in_fmt.sample_rate, out_fmt.sample_rate);
	auto *conv = static_cast<ma_data_converter *>(std::malloc(sizeof(ma_data_converter)));
	if(!conv) return;
	if(ma_data_converter_init(&cfg, nullptr, conv) != MA_SUCCESS) {
		std::free(conv);
		return;
	}
	converter_ = conv;

	std::vector<float> leftover_in;  // unconsumed input samples

	while(should_continue()) {
		if(flush_requested_.exchange(false)) {
			ma_data_converter_reset(conv);
			leftover_in.clear();
			flush_buffer();
		}

		AudioChunk in_chunk = previous_->read_chunk(4096);
		if(in_chunk.empty()) break;

		// Concatenate leftover with new input; process the combined buffer.
		std::vector<float> in_samples = std::move(in_chunk.samples());
		if(!leftover_in.empty()) {
			in_samples.insert(in_samples.begin(), leftover_in.begin(), leftover_in.end());
			leftover_in.clear();
		}

		ma_uint64 total_in = in_samples.size() / in_fmt.channels;
		ma_uint64 cursor = 0;

		while(cursor < total_in && should_continue()) {
			ma_uint64 remaining = total_in - cursor;
			ma_uint64 expected_out = 0;
			ma_data_converter_get_expected_output_frame_count(conv, remaining, &expected_out);
			if(expected_out == 0) {
				// Not enough input to emit any output yet — stash and wait
				// for more. `remaining` is small enough (< one resampler
				// window) that we're safe to just hold it.
				leftover_in.assign(
					in_samples.begin() + cursor * in_fmt.channels,
					in_samples.end());
				cursor = total_in;
				break;
			}
			expected_out += 64; // slack for rounding

			std::vector<float> out_samples(expected_out * out_fmt.channels);
			ma_uint64 in_consumed = remaining;
			ma_uint64 out_produced = expected_out;
			ma_result r = ma_data_converter_process_pcm_frames(
				conv,
				in_samples.data() + cursor * in_fmt.channels, &in_consumed,
				out_samples.data(), &out_produced);
			if(r != MA_SUCCESS) { cursor = total_in; break; }

			if(out_produced > 0) {
				out_samples.resize(out_produced * out_fmt.channels);
				const float gain = gain_.load();
				if(gain != 1.0f) {
					for(float &sample : out_samples) sample *= gain;
				}
				write_chunk(AudioChunk(out_fmt, std::move(out_samples), in_chunk.stream_timestamp()));
			}

			if(in_consumed == 0 && out_produced == 0) {
				// No progress: stash remainder for next chunk.
				leftover_in.assign(
					in_samples.begin() + cursor * in_fmt.channels,
					in_samples.end());
				cursor = total_in;
				break;
			}
			cursor += in_consumed;
		}
	}

	// Drain: flush the resampler's internal tail so the final samples
	// reach the output.
	while(should_continue()) {
		ma_uint64 in_consumed = 0;
		ma_uint64 out_produced = 4096;
		std::vector<float> out_samples(out_produced * out_fmt.channels);
		ma_result r = ma_data_converter_process_pcm_frames(
			conv, nullptr, &in_consumed,
			out_samples.data(), &out_produced);
		if(r != MA_SUCCESS || out_produced == 0) break;
		out_samples.resize(out_produced * out_fmt.channels);
		const float gain = gain_.load();
		if(gain != 1.0f) {
			for(float &sample : out_samples) sample *= gain;
		}
		write_chunk(AudioChunk(out_fmt, std::move(out_samples), 0.0));
	}
}

} // namespace tuxedo
