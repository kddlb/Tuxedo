#include "core/media_probe.hpp"

#include "core/cue_sheet.hpp"
#include "plugin/registry.hpp"
#include "plugin/input/cue_decoder.hpp"

namespace tuxedo {

bool open_media_url(const std::string &url, OpenedMedia &out, bool skip_cue) {
	out = {};

	auto &reg = PluginRegistry::instance();
	if(!skip_cue && cue_has_track_fragment(url)) {
		auto source = reg.source_for_url(url);
		if(!source || !source->open(url)) return false;

		auto cue_decoder = DecoderPtr(new CueDecoder());
		if(!cue_decoder->open(source.get())) {
			source->close();
			return false;
		}

		out.source = std::move(source);
		out.properties = cue_decoder->properties();
		out.decoder = std::move(cue_decoder);
		return true;
	}

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

		out.source = std::move(source);
		out.decoder = std::move(candidate);
		out.properties = props;
		return true;
	}

	if(source) source->close();
	return false;
}

} // namespace tuxedo
