#include "plugin/registry.hpp"

#include "core/archive_url.hpp"
#include "plugin/input/cue_decoder.hpp"
#include "plugin/input/archive_source.hpp"
#include "plugin/input/file_source.hpp"
#include "plugin/input/ffmpeg_decoder.hpp"
#include "plugin/input/flac_decoder.hpp"
#include "plugin/input/http_source.hpp"
#include "plugin/input/miniaudio_decoder.hpp"
#include "plugin/input/musepack_decoder.hpp"
#include "plugin/input/mp3_decoder.hpp"
#include "plugin/input/opus_decoder.hpp"
#include "plugin/input/silence_decoder.hpp"
#include "plugin/input/silence_source.hpp"
#include "plugin/input/vorbis_decoder.hpp"

#include <algorithm>
#include <cctype>

namespace tuxedo {

namespace {

std::string lowercase_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

} // namespace

PluginRegistry &PluginRegistry::instance() {
	static PluginRegistry r;
	return r;
}

void PluginRegistry::register_source(const std::string &scheme, SourceFactory f) {
	sources_[scheme] = std::move(f);
}

void PluginRegistry::register_decoder(const std::string &ext, DecoderFactory f) {
	decoders_[lowercase_copy(ext)] = std::move(f);
}

void PluginRegistry::register_decoder_mime(const std::string &mime, DecoderFactory f) {
	std::string key = normalize_mime_type(mime);
	if(key.empty()) return;
	mime_decoders_[key] = std::move(f);
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
	auto it = decoders_.find(lowercase_copy(ext));
	return it == decoders_.end() ? nullptr : it->second();
}

DecoderPtr PluginRegistry::decoder_for_mime(const std::string &mime) {
	std::string key = normalize_mime_type(mime);
	if(key.empty()) return nullptr;
	auto it = mime_decoders_.find(key);
	return it == mime_decoders_.end() ? nullptr : it->second();
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
	ArchiveUrlParts archive_parts;
	if(parse_archive_url(path, archive_parts)) {
		return extension_of(archive_parts.entry_path);
	}
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

std::string PluginRegistry::normalize_mime_type(const std::string &mime) {
	size_t start = 0;
	while(start < mime.size() && std::isspace(static_cast<unsigned char>(mime[start]))) ++start;
	size_t end = mime.find(';', start);
	if(end == std::string::npos) end = mime.size();
	while(end > start && std::isspace(static_cast<unsigned char>(mime[end - 1]))) --end;
	if(start >= end) return {};
	return lowercase_copy(mime.substr(start, end - start));
}

void register_builtin_plugins() {
	auto &r = PluginRegistry::instance();
	r.register_source("file", [] { return SourcePtr(new FileSource()); });
	r.register_source("http", [] { return SourcePtr(new HttpSource()); });
	r.register_source("https", [] { return SourcePtr(new HttpSource()); });
	r.register_source("silence", [] { return SourcePtr(new SilenceSource()); });
	r.register_source("unpack", [] { return SourcePtr(new ArchiveSource()); });

	auto cue_factory = [] { return DecoderPtr(new CueDecoder()); };
	r.register_decoder("cue", cue_factory);
	r.register_decoder_mime("application/x-cue", cue_factory);

	auto silence_factory = [] { return DecoderPtr(new SilenceDecoder()); };
	r.register_decoder_mime("audio/x-silence", silence_factory);

	// miniaudio handles the WAV fallback. WAV has no standardised
	// tag container worth reading, so an empty metadata() is fine there.
	auto ma_factory = [] { return DecoderPtr(new MiniaudioDecoder()); };
	for(const char *ext : {"wav", "wave"}) {
		r.register_decoder(ext, ma_factory);
	}
	for(const char *mime : {"audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave"}) {
		r.register_decoder_mime(mime, ma_factory);
	}

	// libFLAC takes over .flac — lossless path, Vorbis-comment metadata.
	auto flac_factory = [] { return DecoderPtr(new FlacDecoder()); };
	r.register_decoder("flac", flac_factory);
	for(const char *mime : {"audio/flac", "audio/x-flac", "application/flac", "application/x-flac"}) {
		r.register_decoder_mime(mime, flac_factory);
	}

	// libopusfile for .opus — native Vorbis-comment metadata + R128 gains.
	auto opus_factory = [] { return DecoderPtr(new OpusDecoder()); };
	r.register_decoder("opus", opus_factory);
	r.register_decoder_mime("audio/opus", opus_factory);

	// libvorbisfile for Ogg Vorbis — Vorbis-comment metadata +
	// METADATA_BLOCK_PICTURE album art. .oga routes here too.
	auto vorbis_factory = [] { return DecoderPtr(new VorbisDecoder()); };
	for(const char *ext : {"ogg", "oga"}) {
		r.register_decoder(ext, vorbis_factory);
	}
	r.register_decoder_mime("audio/vorbis", vorbis_factory);

	// miniaudio/dr_mp3 for MP3 audio + libid3tag for ID3v1/v2 tags,
	// including APIC album art and TXXX ReplayGain entries.
	auto mp3_factory = [] { return DecoderPtr(new Mp3Decoder()); };
	r.register_decoder("mp3", mp3_factory);
	for(const char *mime : {"audio/mpeg", "audio/mp3"}) {
		r.register_decoder_mime(mime, mp3_factory);
	}

	// libmpcdec handles Musepack streams natively, matching Cog's
	// reader/demux path instead of routing .mpc through FFmpeg fallback.
	auto musepack_factory = [] { return DecoderPtr(new MusepackDecoder()); };
	r.register_decoder("mpc", musepack_factory);
	for(const char *mime : {"audio/x-musepack", "audio/musepack", "audio/x-mpc"}) {
		r.register_decoder_mime(mime, musepack_factory);
	}

	// FFmpeg is the broad fallback path for streams, extensionless URLs,
	// and formats without a dedicated native decoder.
	auto ffmpeg_factory = [] { return DecoderPtr(new FfmpegDecoder()); };
	for(const char *mime : {"application/ogg", "audio/ogg",
	                        "application/vnd.apple.mpegurl", "application/x-mpegurl",
	                        "audio/mpegurl", "audio/x-mpegurl"}) {
		r.register_decoder_mime(mime, ffmpeg_factory);
	}
	r.register_fallback_decoder(ffmpeg_factory);
}

} // namespace tuxedo
