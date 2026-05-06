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
	target_ = std::nullopt;
	input_ = std::make_unique<InputNode>();
	if(!input_->open_url(url) && !input_->open_url(kSilenceFallbackUrl)) {
		input_.reset();
		url_.clear();
		return false;
	}
	format_ = input_->properties().format;

	converter_ = std::make_unique<ConverterNode>();
	converter_->set_previous(input_.get());
	downmix_ = std::make_unique<DSPDownmixNode>(
	    converter_.get(), input_->properties().format, input_->properties().channel_layout);
	format_ = compute_output_format(std::nullopt);
	downmix_->set_output_format(format_);
	fader_ = std::make_unique<DSPFaderNode>(downmix_.get());
	return true;
}

void BufferChain::close() {
	if(fader_) {
		fader_->request_stop();
		fader_->join();
		fader_.reset();
	}
	if(downmix_) {
		downmix_->request_stop();
		downmix_->join();
		downmix_.reset();
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
	target_ = std::nullopt;
}

void BufferChain::set_downmix_enabled(bool enabled) {
	downmix_enabled_ = enabled;
	if(!input_ || !converter_ || !downmix_) return;
	converter_->set_target_format(compute_converter_target(target_));
	format_ = compute_output_format(target_);
	downmix_->set_output_format(format_);
}

void BufferChain::retarget(std::optional<StreamFormat> target) {
	if(!converter_ || !downmix_ || !input_) return;
	target_ = target;
	converter_->set_target_format(compute_converter_target(target));
	format_ = compute_output_format(target);
	downmix_->set_output_format(format_);
}

void BufferChain::seek(int64_t frame) {
	if(!input_) return;
	input_->request_seek(frame);
	if(converter_) {
		converter_->request_flush();
		converter_->flush_buffer();
	}
	if(downmix_) downmix_->reset_buffer();
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
	if(downmix_) downmix_->launch();
	if(fader_) fader_->launch();
	launched_ = true;
}

void BufferChain::request_stop() {
	if(fader_) fader_->request_stop();
	if(downmix_) downmix_->request_stop();
	if(converter_) converter_->request_stop();
	if(input_) input_->request_stop();
}

StreamFormat BufferChain::compute_output_format(std::optional<StreamFormat> target) const {
	if(!input_) return {};
	StreamFormat out = target ? *target : input_->properties().format;
	if(downmix_enabled_ && input_->properties().format.channels > 2 && out.channels > 2) out.channels = 2;
	return out;
}

std::optional<StreamFormat> BufferChain::compute_converter_target(std::optional<StreamFormat> target) const {
	if(!input_) return target;
	if(!target) return std::nullopt;
	StreamFormat out = *target;
	if(downmix_enabled_ && input_->properties().format.channels > 2 && out.channels <= 2) {
		out.channels = input_->properties().format.channels;
	}
	return out;
}

} // namespace tuxedo
