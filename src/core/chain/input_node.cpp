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
	const std::string ext = PluginRegistry::extension_of(url);
	std::vector<DecoderPtr> candidates;
	auto source = reg.source_for_url(url);
	if(!source || !source->open(url)) return false;

	if(auto primary = reg.decoder_for_extension(ext)) {
		candidates.push_back(std::move(primary));
	} else if(auto primary = reg.decoder_for_mime(source->mime_type())) {
		candidates.push_back(std::move(primary));
	}
	for(auto &fallback : reg.fallback_decoders()) {
		candidates.push_back(std::move(fallback));
	}

	bool reuse_open_source = true;
	for(auto &candidate : candidates) {
		if(!reuse_open_source) {
			source = reg.source_for_url(url);
			if(!source || !source->open(url)) continue;
		}
		reuse_open_source = false;
		if(!candidate || !candidate->open(source.get())) continue;

		DecoderProperties props = candidate->properties();
		if(!props.format.valid()) {
			candidate->close();
			source->close();
			continue;
		}

		source_ = std::move(source);
		decoder_ = std::move(candidate);
		if(source_) source_->set_metadata_changed_callback([this] {
			if(metadata_changed_cb_) metadata_changed_cb_();
		});
		if(decoder_) decoder_->set_metadata_changed_callback([this] {
			if(metadata_changed_cb_) metadata_changed_cb_();
		});
		props_ = props;
		return true;
	}

	if(source) source->close();
	return false;
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
