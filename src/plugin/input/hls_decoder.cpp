#include "plugin/input/hls_decoder.hpp"

#include "plugin/input/ffmpeg_decoder.hpp"
#include "plugin/input/hls_memory_source.hpp"
#include "plugin/input/hls_playlist.hpp"
#include "plugin/input/hls_segment_manager.hpp"
#include "plugin/input/http_source.hpp"
#include "plugin/registry.hpp"

#include <cstring>
#include <utility>

namespace tuxedo {

namespace {

bool read_all(Source &source, std::string &out) {
	out.clear();
	char buf[4096];
	for(;;) {
		int64_t n = source.read(buf, sizeof(buf));
		if(n < 0) return false;
		if(n == 0) break;
		out.append(buf, static_cast<size_t>(n));
	}
	return !out.empty();
}

bool fetch_url_text(const std::string &url, std::string &out) {
	HttpSource src;
	if(!src.open(url)) return false;
	out.clear();
	char buf[4096];
	for(;;) {
		int64_t n = src.read(buf, sizeof(buf));
		if(n < 0) {
			src.close();
			return false;
		}
		if(n == 0) break;
		out.append(buf, static_cast<size_t>(n));
	}
	src.close();
	return !out.empty();
}

// Strip a UTF-8 BOM if present. RFC 8216 requires UTF-8 but some
// publishers leave a BOM at the head of the playlist body.
void strip_utf8_bom(std::string &s) {
	if(s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xEF &&
	   static_cast<uint8_t>(s[1]) == 0xBB && static_cast<uint8_t>(s[2]) == 0xBF) {
		s.erase(0, 3);
	}
}

std::string fake_filename_for_mime(const std::string &mime) {
	std::string m;
	m.reserve(mime.size());
	for(char c : mime) m.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

	auto starts = [&m](const char *p) { return m.compare(0, std::strlen(p), p) == 0; };
	if(starts("audio/aac") || starts("audio/aacp")) return "stream.aac";
	if(starts("audio/mpeg") || starts("audio/mp3")) return "stream.mp3";
	if(starts("audio/mp4") || starts("audio/m4a")) return "stream.m4a";
	if(starts("video/mp2t") || starts("audio/mp2t")) return "stream.ts";
	if(starts("video/mp4")) return "stream.mp4";
	return {};
}

std::string strip_fragment(const std::string &url) {
	auto hash = url.find('#');
	return hash == std::string::npos ? url : url.substr(0, hash);
}

std::string directory_of(const std::string &url) {
	std::string head = strip_fragment(url);
	auto q = head.find('?');
	if(q != std::string::npos) head.erase(q);
	auto slash = head.find_last_of('/');
	if(slash == std::string::npos) return head;
	return head.substr(0, slash + 1);
}

} // namespace

HlsDecoder::HlsDecoder() = default;
HlsDecoder::~HlsDecoder() { close(); }

bool HlsDecoder::open(Source *source) {
	close();
	if(!source) return false;

	source_url_ = source->url();
	const std::string scheme = PluginRegistry::scheme_of(source_url_);
	if(scheme != "http" && scheme != "https") return false;

	std::string text;
	if(!read_all(*source, text)) return false;
	strip_utf8_bom(text);

	HlsPlaylist playlist;
	std::string parse_err;
	if(!parse_hls_playlist(text, source_url_, playlist, parse_err)) return false;

	// Master playlist? Pick the highest-bandwidth variant and refetch.
	if(playlist.is_master) {
		const HlsVariant *best = nullptr;
		for(const HlsVariant &v : playlist.variants) {
			if(v.url.empty()) continue;
			if(!best || v.bandwidth > best->bandwidth) best = &v;
		}
		if(!best) return false;

		std::string variant_url = best->url;
		std::string variant_text;
		if(!fetch_url_text(variant_url, variant_text)) return false;
		strip_utf8_bom(variant_text);

		HlsPlaylist media;
		std::string media_err;
		if(!parse_hls_playlist(variant_text, variant_url, media, media_err)) return false;
		if(media.is_master) return false; // nested master — refuse
		playlist = std::move(media);
	}

	if(playlist.segments.empty()) return false;
	if(playlist.segments[0].encrypted) return false; // not supported in this build

	is_live_ = playlist.is_live;

	// Build a clean URL for the memory source. Strip any fragment so
	// FFmpeg doesn't interpret it as a subsong index.
	std::string clean_url = strip_fragment(source_url_);

	memory_source_ = std::make_unique<HlsMemorySource>(clean_url, "application/octet-stream");

	segment_manager_ = std::make_unique<HlsSegmentManager>(std::move(playlist));
	segment_manager_->set_memory_source(memory_source_.get());

	std::vector<uint8_t> first_data;
	std::string fetch_err;
	if(!segment_manager_->download_segment_at_index(0, first_data, fetch_err)) {
		close();
		return false;
	}

	std::string seg_mime = segment_manager_->last_observed_mime_type();
	if(seg_mime.empty()) seg_mime = "audio/mpeg";
	memory_source_->set_mime_type(seg_mime);

	std::string fake_name = fake_filename_for_mime(seg_mime);
	if(!fake_name.empty()) {
		memory_source_->set_url(directory_of(clean_url) + fake_name);
	}

	memory_source_->append_data(first_data.data(), first_data.size());
	segment_manager_->start_fetching_from(1);

	decoder_ = std::make_unique<FfmpegDecoder>();
	if(!decoder_->open(memory_source_.get())) {
		close();
		return false;
	}

	if(!is_live_) {
		total_duration_ = segment_manager_->total_duration();
		uint32_t sr = decoder_->properties().format.sample_rate;
		if(sr > 0 && total_duration_ > 0.0) {
			total_frames_ = static_cast<int64_t>(total_duration_ * sr);
		} else {
			total_frames_ = -1;
		}
	} else {
		total_frames_ = -1;
	}

	return true;
}

void HlsDecoder::close() {
	if(segment_manager_) segment_manager_->stop();

	if(decoder_) {
		decoder_->close();
		decoder_.reset();
	}
	if(memory_source_) {
		memory_source_->close();
		memory_source_.reset();
	}
	segment_manager_.reset();

	is_live_ = false;
	total_duration_ = 0.0;
	total_frames_ = -1;
	pending_skip_frames_ = 0;
	source_url_.clear();
}

DecoderProperties HlsDecoder::properties() const {
	DecoderProperties props = decoder_ ? decoder_->properties() : DecoderProperties{};
	props.total_frames = total_frames_;
	return props;
}

bool HlsDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!decoder_) return false;

	while(decoder_->read(out, max_frames)) {
		if(pending_skip_frames_ <= 0) return true;

		size_t frames = out.frame_count();
		if(static_cast<int64_t>(frames) <= pending_skip_frames_) {
			pending_skip_frames_ -= static_cast<int64_t>(frames);
			continue;
		}
		// remove_frames(N) returns the first N frames and leaves the
		// remainder in `out` — exactly the discard-leading-skip we want.
		(void)out.remove_frames(static_cast<size_t>(pending_skip_frames_));
		pending_skip_frames_ = 0;
		return true;
	}
	return false;
}

int64_t HlsDecoder::seek(int64_t frame) {
	if(!decoder_ || is_live_) return -1;
	if(frame < 0) frame = 0;

	const uint32_t sr = decoder_->properties().format.sample_rate;
	if(sr == 0) return -1;

	const double target_time = static_cast<double>(frame) / sr;

	const auto &segs = segment_manager_->playlist().segments;
	if(segs.empty()) return -1;

	double accum = 0.0;
	size_t target_index = 0;
	double segment_start_time = 0.0;
	for(size_t i = 0; i < segs.size(); ++i) {
		if(target_time < accum + segs[i].duration) {
			target_index = i;
			segment_start_time = accum;
			break;
		}
		accum += segs[i].duration;
		// Clamp to last segment if we run off the end.
		target_index = i;
		segment_start_time = accum - segs[i].duration;
	}

	const int64_t segment_start_frame = static_cast<int64_t>(segment_start_time * sr);
	int64_t offset_frames = frame - segment_start_frame;
	if(offset_frames < 0) offset_frames = 0;

	// Try to fetch the new starting segment BEFORE tearing anything
	// down — if the network fetch fails we'd rather leave the existing
	// decoder running than drop into a broken state.
	std::vector<uint8_t> seg_data;
	std::string fetch_err;
	if(!segment_manager_->download_segment_at_index(target_index, seg_data, fetch_err)) {
		return -1;
	}

	segment_manager_->stop();
	memory_source_->reset();

	decoder_->close();
	decoder_.reset();

	memory_source_->append_data(seg_data.data(), seg_data.size());
	segment_manager_->start_fetching_from(target_index + 1);

	decoder_ = std::make_unique<FfmpegDecoder>();
	if(!decoder_->open(memory_source_.get())) {
		decoder_.reset();
		return -1;
	}

	pending_skip_frames_ = offset_frames;
	return frame;
}

nlohmann::json HlsDecoder::metadata() const {
	if(decoder_) return decoder_->metadata();
	return nlohmann::json::object();
}

} // namespace tuxedo
