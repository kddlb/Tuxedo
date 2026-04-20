// BufferChain: owns an InputNode and (eventually) any intermediate
// nodes. Mirrors Cog's BufferChain. MVP has no converter node because
// MiniaudioDecoder already emits float32 at the output format.
#pragma once

#include "core/chain/input_node.hpp"

#include <memory>
#include <string>

namespace tuxedo {

class BufferChain {
public:
	BufferChain();
	~BufferChain();

	bool open(const std::string &url);
	void close();

	void launch();
	void request_stop();

	InputNode *input() { return input_.get(); }
	const InputNode *input() const { return input_.get(); }
	Node *final_node() { return input_.get(); }

	StreamFormat format() const { return format_; }
	const std::string &url() const { return url_; }

private:
	std::unique_ptr<InputNode> input_;
	StreamFormat format_{};
	std::string url_;
};

} // namespace tuxedo
