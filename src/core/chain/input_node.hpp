// InputNode: drives a Decoder, pushes AudioChunks into its buffer.
// Mirrors Cog's InputNode.
#pragma once

#include "core/chain/node.hpp"
#include "plugin/decoder.hpp"
#include "plugin/source.hpp"

#include <atomic>
#include <memory>

namespace tuxedo {

class InputNode : public Node {
public:
	InputNode();
	~InputNode() override;

	bool open_url(const std::string &url);
	void close();

	const DecoderProperties &properties() const { return props_; }

	// Request a seek to `frame`; handled inside the worker between reads.
	void request_seek(int64_t frame);

	void process() override;

private:
	SourcePtr source_;
	DecoderPtr decoder_;
	DecoderProperties props_{};

	std::atomic<int64_t> pending_seek_{-1};
};

} // namespace tuxedo
