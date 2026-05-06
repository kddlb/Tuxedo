#pragma once

#include "core/chain/downmix.hpp"
#include "core/chain/dsp_node.hpp"

#include <memory>
#include <mutex>

namespace tuxedo {

class DSPDownmixNode : public DSPNode {
public:
	DSPDownmixNode(Node *previous, StreamFormat input_format, uint64_t input_layout, double latency_seconds = 0.10);
	~DSPDownmixNode() override = default;

	void set_output_format(StreamFormat output_format);
	void reset_buffer();
	StreamFormat output_format() const;
	void process() override;

private:
	std::unique_ptr<DownmixProcessor> rebuild_processor_locked() const;

	mutable std::mutex mtx_;
	StreamFormat input_format_{};
	uint64_t input_layout_ = 0;
	StreamFormat output_format_{};
};

} // namespace tuxedo
