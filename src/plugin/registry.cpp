#include "plugin/registry.hpp"

#include "plugin/input/file_source.hpp"
#include "plugin/input/ffmpeg_decoder.hpp"
#include "plugin/input/flac_decoder.hpp"
#include "plugin/input/http_source.hpp"
#include "plugin/input/miniaudio_decoder.hpp"
#include "plugin/input/mp3_decoder.hpp"
#include "plugin/input/opus_decoder.hpp"
#include "plugin/input/vorbis_decoder.hpp"

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

void PluginRegistry::register_fallback_decoder(DecoderFactory f) {
	fallback_decoders_.push_back(std::move(f));
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

std::vector<DecoderPtr> PluginRegistry::fallback_decoders() {
	std::vector<DecoderPtr> out;
	out.reserve(fallback_decoders_.size());
	for(const auto &factory : fallback_decoders_) {
		out.push_back(factory());
	}
	return out;
}

std::string PluginRegistry::extension_of(const std::string &path) {
	size_t end = path.find_first_of("?#");
	if(end == std::string::npos) end = path.size();
	auto dot = path.rfind('.', end);
	auto slash = path.find_last_of("/\\", end == 0 ? 0 : end - 1);
	if(dot == std::string::npos) return {};
	if(slash != std::string::npos && dot < slash) return {};
	return path.substr(dot + 1, end - dot - 1);
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
	r.register_source("http", [] { return SourcePtr(new HttpSource()); });
	r.register_source("https", [] { return SourcePtr(new HttpSource()); });

	// miniaudio handles the WAV fallback. WAV has no standardised
	// tag container worth reading, so an empty metadata() is fine there.
	auto ma_factory = [] { return DecoderPtr(new MiniaudioDecoder()); };
	for(const char *ext : {"wav", "wave"}) {
		r.register_decoder(ext, ma_factory);
	}

	// libFLAC takes over .flac — lossless path, Vorbis-comment metadata.
	r.register_decoder("flac", [] { return DecoderPtr(new FlacDecoder()); });

	// libopusfile for .opus — native Vorbis-comment metadata + R128 gains.
	r.register_decoder("opus", [] { return DecoderPtr(new OpusDecoder()); });

	// libvorbisfile for Ogg Vorbis — Vorbis-comment metadata +
	// METADATA_BLOCK_PICTURE album art. .oga routes here too.
	auto vorbis_factory = [] { return DecoderPtr(new VorbisDecoder()); };
	for(const char *ext : {"ogg", "oga"}) {
		r.register_decoder(ext, vorbis_factory);
	}

	// miniaudio/dr_mp3 for MP3 audio + libid3tag for ID3v1/v2 tags,
	// including APIC album art and TXXX ReplayGain entries.
	r.register_decoder("mp3", [] { return DecoderPtr(new Mp3Decoder()); });

	// FFmpeg is the broad fallback path for streams, extensionless URLs,
	// and formats without a dedicated native decoder.
	r.register_fallback_decoder([] { return DecoderPtr(new FfmpegDecoder()); });
}

} // namespace tuxedo
