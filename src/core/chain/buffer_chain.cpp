#include "core/chain/buffer_chain.hpp"

namespace tuxedo {

BufferChain::BufferChain() = default;

BufferChain::~BufferChain() { close(); }

bool BufferChain::open(const std::string &url) {
	close();
	input_ = std::make_unique<InputNode>();
	if(!input_->open_url(url)) {
		input_.reset();
		return false;
	}
	format_ = input_->properties().format;
	return true;
}

void BufferChain::close() {
	if(input_) {
		input_->request_stop();
		input_->join();
		input_.reset();
	}
	format_ = {};
}

void BufferChain::launch() {
	if(input_) input_->launch();
}

void BufferChain::request_stop() {
	if(input_) input_->request_stop();
}

} // namespace tuxedo
