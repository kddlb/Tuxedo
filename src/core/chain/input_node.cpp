#include "core/chain/input_node.hpp"

#include "core/media_probe.hpp"

namespace tuxedo {

InputNode::InputNode() = default;

InputNode::~InputNode() {
	request_stop();
	join();
	close();
}

bool InputNode::open_url(const std::string &url) {
	close();
	OpenedMedia opened;
	if(!open_media_url(url, opened)) return false;
	source_ = std::move(opened.source);
	decoder_ = std::move(opened.decoder);
	props_ = opened.properties;
	if(source_) source_->set_metadata_changed_callback([this] {
		if(metadata_changed_cb_) metadata_changed_cb_();
	});
	if(decoder_) decoder_->set_metadata_changed_callback([this] {
		if(metadata_changed_cb_) metadata_changed_cb_();
	});
	return true;
}

void InputNode::close() {
	if(decoder_) {
		decoder_->set_metadata_changed_callback({});
		decoder_->close();
		decoder_.reset();
	}
	if(source_) {
		source_->set_metadata_changed_callback({});
		source_->close();
		source_.reset();
	}
	props_ = {};
}

void InputNode::set_metadata_changed_callback(Source::MetadataChangedCallback cb) {
	metadata_changed_cb_ = std::move(cb);
	if(source_) source_->set_metadata_changed_callback([this] {
		if(metadata_changed_cb_) metadata_changed_cb_();
	});
	if(decoder_) decoder_->set_metadata_changed_callback([this] {
		if(metadata_changed_cb_) metadata_changed_cb_();
	});
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
