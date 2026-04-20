#include "core/chain/input_node.hpp"

#include "plugin/registry.hpp"

namespace tuxedo {

InputNode::InputNode() = default;

InputNode::~InputNode() {
	request_stop();
	join();
	close();
}

bool InputNode::open_url(const std::string &url) {
	close();

	auto &reg = PluginRegistry::instance();
	source_ = reg.source_for_url(url);
	if(!source_ || !source_->open(url)) return false;

	const std::string ext = PluginRegistry::extension_of(url);
	decoder_ = reg.decoder_for_extension(ext);
	if(!decoder_ || !decoder_->open(source_.get())) return false;

	props_ = decoder_->properties();
	return props_.format.valid();
}

void InputNode::close() {
	if(decoder_) {
		decoder_->close();
		decoder_.reset();
	}
	if(source_) {
		source_->close();
		source_.reset();
	}
	props_ = {};
}

void InputNode::request_seek(int64_t frame) {
	pending_seek_.store(frame);
}

void InputNode::process() {
	static constexpr size_t kReadFrames = 4096;

	while(should_continue()) {
		int64_t seek_to = pending_seek_.exchange(-1);
		if(seek_to >= 0) {
			// Any chunks already queued are pre-seek — discard them so the
			// OutputNode starts consuming post-seek audio on its next pull.
			flush_buffer();
			decoder_->seek(seek_to);
		}

		AudioChunk chunk;
		if(!decoder_->read(chunk, kReadFrames)) break;
		if(chunk.empty()) break;
		write_chunk(std::move(chunk));
	}
}

} // namespace tuxedo
