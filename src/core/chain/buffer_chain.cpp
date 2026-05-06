#include "core/chain/buffer_chain.hpp"

namespace tuxedo {

namespace {

constexpr const char *kSilenceFallbackUrl = "silence://10";

}

BufferChain::BufferChain() = default;

BufferChain::~BufferChain() { close(); }

bool BufferChain::open(const std::string &url) {
	close();
	url_ = url;
	input_ = std::make_unique<InputNode>();
	if(!input_->open_url(url) && !input_->open_url(kSilenceFallbackUrl)) {
		input_.reset();
		url_.clear();
		return false;
	}
	format_ = input_->properties().format;

	converter_ = std::make_unique<ConverterNode>();
	converter_->set_previous(input_.get());
	fader_ = std::make_unique<DSPFaderNode>(converter_.get());
	return true;
}

void BufferChain::close() {
	if(fader_) {
		fader_->request_stop();
		fader_->join();
		fader_.reset();
	}
	if(converter_) {
		converter_->request_stop();
		converter_->join();
		converter_.reset();
	}
	if(input_) {
		input_->request_stop();
		input_->join();
		input_.reset();
	}
	format_ = {};
	url_.clear();
	launched_ = false;
}

void BufferChain::retarget(std::optional<StreamFormat> target) {
	if(!converter_) return;
	converter_->set_target_format(target);
}

void BufferChain::seek(int64_t frame) {
	if(!input_) return;
	input_->request_seek(frame);
	if(converter_) {
		converter_->request_flush();
		converter_->flush_buffer();
	}
	if(fader_) fader_->reset_buffer();
	input_->flush_buffer();
}

void BufferChain::set_gain(float gain) {
	if(converter_) converter_->set_gain(gain);
}

void BufferChain::launch() {
	if(launched_) return;
	if(input_) input_->launch();
	if(converter_) converter_->launch();
	if(fader_) fader_->launch();
	launched_ = true;
}

void BufferChain::request_stop() {
	if(fader_) fader_->request_stop();
	if(converter_) converter_->request_stop();
	if(input_) input_->request_stop();
}

} // namespace tuxedo
