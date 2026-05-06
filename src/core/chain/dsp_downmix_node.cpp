#include "core/chain/dsp_downmix_node.hpp"

#include <vector>

namespace tuxedo {

DSPDownmixNode::DSPDownmixNode(Node *previous, StreamFormat input_format, uint64_t input_layout, double latency_seconds)
: DSPNode(previous, latency_seconds),
  input_format_(input_format),
  input_layout_(input_layout),
  output_format_(input_format) {}

void DSPDownmixNode::set_output_format(StreamFormat output_format) {
	std::lock_guard<std::mutex> g(mtx_);
	output_format_ = output_format;
}

void DSPDownmixNode::reset_buffer() {
	std::lock_guard<std::mutex> g(mtx_);
	flush_buffer();
}

StreamFormat DSPDownmixNode::output_format() const {
	std::lock_guard<std::mutex> g(mtx_);
	return output_format_;
}

std::unique_ptr<DownmixProcessor> DSPDownmixNode::rebuild_processor_locked() const {
	if(!input_format_.valid() || !output_format_.valid()) return nullptr;
	return std::make_unique<DownmixProcessor>(input_format_, input_layout_, output_format_);
}

void DSPDownmixNode::process() {
	while(should_continue()) {
		if(!previous_) return;

		AudioChunk chunk = previous_->read_chunk(4096);
		if(chunk.empty()) {
			if(previous_->end_of_stream()) break;
			continue;
		}

		std::unique_ptr<DownmixProcessor> processor;
		StreamFormat out_fmt{};
		{
			std::lock_guard<std::mutex> g(mtx_);
			if(!input_format_.valid()) input_format_ = chunk.format();
			out_fmt = output_format_;
			processor = rebuild_processor_locked();
		}

		if(!processor || out_fmt == chunk.format()) {
			write_chunk(std::move(chunk));
			continue;
		}

		std::vector<float> out_samples(chunk.frame_count() * out_fmt.channels);
		processor->process(chunk.samples().data(), chunk.frame_count(), out_samples.data());
		write_chunk(AudioChunk(out_fmt, std::move(out_samples), chunk.stream_timestamp()));
	}
}

} // namespace tuxedo
