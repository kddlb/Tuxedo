#include "plugin/registry.hpp"

#include "plugin/input/file_source.hpp"
#include "plugin/input/flac_decoder.hpp"
#include "plugin/input/miniaudio_decoder.hpp"

#include <algorithm>
#include <cctype>

namespace tuxedo {

PluginRegistry &PluginRegistry::instance() {
	static PluginRegistry r;
	return r;
}

void PluginRegistry::register_source(const std::string &scheme, SourceFactory f) {
	sources_[scheme] = std::move(f);
}

void PluginRegistry::register_decoder(const std::string &ext, DecoderFactory f) {
	std::string k = ext;
	std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
	decoders_[k] = std::move(f);
}

SourcePtr PluginRegistry::source_for_url(const std::string &url) {
	auto it = sources_.find(scheme_of(url));
	if(it == sources_.end()) {
		// Bare paths fall back to "file".
		it = sources_.find("file");
	}
	return it == sources_.end() ? nullptr : it->second();
}

DecoderPtr PluginRegistry::decoder_for_extension(const std::string &ext) {
	std::string k = ext;
	std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
	auto it = decoders_.find(k);
	return it == decoders_.end() ? nullptr : it->second();
}

std::string PluginRegistry::extension_of(const std::string &path) {
	auto dot = path.find_last_of('.');
	auto slash = path.find_last_of("/\\");
	if(dot == std::string::npos) return {};
	if(slash != std::string::npos && dot < slash) return {};
	return path.substr(dot + 1);
}

std::string PluginRegistry::scheme_of(const std::string &url) {
	auto colon = url.find(':');
	if(colon == std::string::npos) return {};
	// Crude but sufficient: "abc://..." or "abc:..."
	for(size_t i = 0; i < colon; ++i) {
		char c = url[i];
		if(!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) return {};
	}
	return url.substr(0, colon);
}

void register_builtin_plugins() {
	auto &r = PluginRegistry::instance();
	r.register_source("file", [] { return SourcePtr(new FileSource()); });

	// miniaudio covers mp3/wav/vorbis for the MVP.
	auto ma_factory = [] { return DecoderPtr(new MiniaudioDecoder()); };
	for(const char *ext : {"mp3", "wav", "wave", "ogg", "oga"}) {
		r.register_decoder(ext, ma_factory);
	}

	// libFLAC takes over .flac — lossless path, more accurate metadata later.
	r.register_decoder("flac", [] { return DecoderPtr(new FlacDecoder()); });
}

} // namespace tuxedo
