#include "plugin/input/hls_playlist.hpp"

#include <cctype>
#include <cstring>
#include <unordered_map>

namespace tuxedo {

namespace {

std::string trim(const std::string &s) {
	size_t start = 0;
	while(start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
	size_t end = s.size();
	while(end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
	return s.substr(start, end - start);
}

bool case_insensitive_equal(const std::string &a, const std::string &b) {
	if(a.size() != b.size()) return false;
	for(size_t i = 0; i < a.size(); ++i) {
		if(std::tolower(static_cast<unsigned char>(a[i])) !=
		   std::tolower(static_cast<unsigned char>(b[i]))) return false;
	}
	return true;
}

bool starts_with(const std::string &s, const char *prefix) {
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}

std::vector<std::string> split_lines(const std::string &text) {
	std::vector<std::string> out;
	std::string buf;
	for(char c : text) {
		if(c == '\r' || c == '\n') {
			if(!buf.empty() || !out.empty()) out.push_back(std::move(buf));
			buf.clear();
		} else {
			buf.push_back(c);
		}
	}
	if(!buf.empty()) out.push_back(std::move(buf));
	return out;
}

// Parses RFC 8216 §4.2 attribute lists: name=value, comma-separated, with
// quoted strings allowed for values. Keys are returned uppercased to
// match HLS conventions. Values are unquoted but otherwise unescaped.
std::unordered_map<std::string, std::string>
parse_attribute_list(const std::string &attrs) {
	std::unordered_map<std::string, std::string> out;
	const size_t len = attrs.size();
	size_t i = 0;

	while(i < len) {
		while(i < len && (attrs[i] == ' ' || attrs[i] == '\t')) ++i;
		if(i >= len) break;

		size_t key_start = i;
		while(i < len && attrs[i] != '=' && attrs[i] != ',') ++i;
		std::string key = trim(attrs.substr(key_start, i - key_start));

		std::string value;
		if(i < len && attrs[i] == '=') {
			++i;
			if(i < len && attrs[i] == '"') {
				++i;
				size_t v_start = i;
				while(i < len && attrs[i] != '"') ++i;
				value = attrs.substr(v_start, i - v_start);
				if(i < len) ++i; // closing quote
			} else {
				size_t v_start = i;
				while(i < len && attrs[i] != ',') ++i;
				value = trim(attrs.substr(v_start, i - v_start));
			}
		}

		if(!key.empty()) out[key] = value;

		if(i < len && attrs[i] == ',') ++i;
	}

	return out;
}

std::vector<uint8_t> data_from_hex_string(const std::string &hex) {
	std::string s = hex;
	if(starts_with(s, "0x") || starts_with(s, "0X")) s.erase(0, 2);
	if(s.size() % 2 == 1) s.insert(s.begin(), '0');

	std::vector<uint8_t> out;
	out.reserve(s.size() / 2);
	for(size_t i = 0; i + 2 <= s.size(); i += 2) {
		auto nibble = [](char c) -> int {
			if(c >= '0' && c <= '9') return c - '0';
			if(c >= 'a' && c <= 'f') return c - 'a' + 10;
			if(c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		int hi = nibble(s[i]);
		int lo = nibble(s[i + 1]);
		if(hi < 0 || lo < 0) return {};
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return out;
}

bool has_scheme(const std::string &uri) {
	auto colon = uri.find(':');
	if(colon == std::string::npos) return false;
	for(size_t i = 0; i < colon; ++i) {
		char c = uri[i];
		if(!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

std::string origin_of(const std::string &url) {
	size_t scheme = url.find("://");
	if(scheme == std::string::npos) return {};
	size_t slash = url.find('/', scheme + 3);
	return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string directory_of_url(const std::string &url) {
	size_t end = url.find_first_of("?#");
	std::string head = end == std::string::npos ? url : url.substr(0, end);
	size_t slash = head.find_last_of('/');
	if(slash == std::string::npos) return head;
	return head.substr(0, slash + 1);
}

// Resolve a URI from a playlist line against a base playlist URL.
// HLS playlists almost always live at http(s), so this only handles the
// http/https cases that matter in practice.
std::string resolve_uri(const std::string &uri, const std::string &base_url) {
	std::string trimmed = trim(uri);
	if(trimmed.empty()) return {};
	if(has_scheme(trimmed)) return trimmed;
	if(trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') {
		// scheme-relative ("//host/path") — inherit base scheme
		size_t scheme = base_url.find("://");
		if(scheme == std::string::npos) return trimmed;
		return base_url.substr(0, scheme + 1) + trimmed;
	}
	if(!trimmed.empty() && trimmed[0] == '/') return origin_of(base_url) + trimmed;
	return directory_of_url(base_url) + trimmed;
}

} // namespace

bool parse_hls_playlist(const std::string &text,
                        const std::string &base_url,
                        HlsPlaylist &out,
                        std::string &error) {
	out = {};
	out.url = base_url;

	HlsSegment pending_segment;
	bool have_pending_segment = false;

	HlsVariant pending_variant;
	bool have_pending_variant = false;

	std::string current_map_url;
	bool key_encrypted = false;
	std::string key_method;
	std::string key_url;
	std::vector<uint8_t> key_iv;
	bool pending_discontinuity = false;

	bool saw_header = false;
	bool is_master = false;

	for(const std::string &raw : split_lines(text)) {
		std::string line = trim(raw);
		if(line.empty()) continue;

		if(!saw_header) {
			if(line == "#EXTM3U") {
				saw_header = true;
				continue;
			}
			error = "Missing #EXTM3U header";
			return false;
		}

		if(line[0] != '#') {
			// URI line — terminates a pending segment or variant.
			std::string url = resolve_uri(line, base_url);
			if(url.empty()) continue;

			if(have_pending_variant) {
				pending_variant.url = url;
				out.variants.push_back(std::move(pending_variant));
				pending_variant = {};
				have_pending_variant = false;
				is_master = true;
			} else if(have_pending_segment) {
				pending_segment.url = url;
				pending_segment.sequence_number =
				    out.media_sequence + static_cast<int64_t>(out.segments.size());
				pending_segment.discontinuity_sequence = out.discontinuity_sequence;
				if(!current_map_url.empty()) pending_segment.map_section_url = current_map_url;
				if(key_encrypted) {
					pending_segment.encrypted = true;
					pending_segment.encryption_method = key_method;
					pending_segment.encryption_key_url = key_url;
					pending_segment.iv = key_iv;
				}
				if(pending_discontinuity) {
					pending_segment.discontinuity = true;
					out.discontinuity_sequence++;
					pending_discontinuity = false;
				}
				out.segments.push_back(std::move(pending_segment));
				pending_segment = {};
				have_pending_segment = false;
			}
			continue;
		}

		// Tag line — split at first ':'.
		auto colon = line.find(':');
		std::string tag = colon == std::string::npos ? line : line.substr(0, colon);
		std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 1);

		if(tag == "#EXT-X-VERSION") {
			try { out.version = std::stoi(value); } catch(...) {}
		} else if(tag == "#EXT-X-TARGETDURATION") {
			try { out.target_duration = std::stoi(value); } catch(...) {}
		} else if(tag == "#EXT-X-MEDIA-SEQUENCE") {
			try { out.media_sequence = std::stoll(value); } catch(...) {}
		} else if(tag == "#EXT-X-DISCONTINUITY-SEQUENCE") {
			try { out.discontinuity_sequence = std::stoll(value); } catch(...) {}
		} else if(tag == "#EXT-X-PLAYLIST-TYPE") {
			std::string t = trim(value);
			if(case_insensitive_equal(t, "VOD")) {
				out.type = HlsPlaylistType::VOD;
				out.is_live = false;
			} else if(case_insensitive_equal(t, "EVENT")) {
				out.type = HlsPlaylistType::Event;
			}
		} else if(tag == "#EXT-X-ENDLIST") {
			out.has_endlist = true;
			out.is_live = false;
		} else if(tag == "#EXTINF") {
			pending_segment = {};
			auto comma = value.find(',');
			std::string dur_str = comma == std::string::npos ? value : value.substr(0, comma);
			try { pending_segment.duration = std::stod(dur_str); } catch(...) {}
			if(comma != std::string::npos && comma + 1 <= value.size()) {
				pending_segment.title = value.substr(comma + 1);
			}
			have_pending_segment = true;
		} else if(tag == "#EXT-X-DISCONTINUITY") {
			pending_discontinuity = true;
		} else if(tag == "#EXT-X-KEY") {
			auto attrs = parse_attribute_list(value);
			auto method_it = attrs.find("METHOD");
			std::string method = method_it == attrs.end() ? std::string() : method_it->second;
			if(method.empty() || case_insensitive_equal(method, "NONE")) {
				key_encrypted = false;
				key_method.clear();
				key_url.clear();
				key_iv.clear();
			} else {
				key_encrypted = true;
				key_method = method;
				auto uri_it = attrs.find("URI");
				key_url = uri_it == attrs.end() ? std::string()
				                                : resolve_uri(uri_it->second, base_url);
				auto iv_it = attrs.find("IV");
				key_iv = iv_it == attrs.end() ? std::vector<uint8_t>()
				                              : data_from_hex_string(iv_it->second);
			}
		} else if(tag == "#EXT-X-MAP") {
			auto attrs = parse_attribute_list(value);
			auto uri_it = attrs.find("URI");
			current_map_url = uri_it == attrs.end() ? std::string()
			                                        : resolve_uri(uri_it->second, base_url);
		} else if(tag == "#EXT-X-STREAM-INF") {
			pending_variant = {};
			pending_variant.playlist_url = base_url;
			auto attrs = parse_attribute_list(value);
			try {
				auto bw = attrs.find("BANDWIDTH");
				if(bw != attrs.end()) pending_variant.bandwidth = std::stoll(bw->second);
			} catch(...) {}
			try {
				auto bw = attrs.find("AVERAGE-BANDWIDTH");
				if(bw != attrs.end()) pending_variant.average_bandwidth = std::stoll(bw->second);
			} catch(...) {}
			auto codecs = attrs.find("CODECS");
			if(codecs != attrs.end()) pending_variant.codecs = codecs->second;
			auto res = attrs.find("RESOLUTION");
			if(res != attrs.end()) pending_variant.resolution = res->second;
			have_pending_variant = true;
		}
		// Other #EXT* tags are recognised silently or ignored — we don't
		// need to act on EXT-X-MEDIA / EXT-X-I-FRAME-STREAM-INF /
		// EXT-X-INDEPENDENT-SEGMENTS / EXT-X-START / EXT-X-PROGRAM-DATE-TIME
		// / EXT-X-BYTERANGE / EXT-X-ALLOW-CACHE / EXT-X-GAP /
		// EXT-X-BITRATE for audio playback.
	}

	if(!saw_header) {
		error = "Missing #EXTM3U header";
		return false;
	}
	if(out.segments.empty() && out.variants.empty()) {
		error = "Playlist contains no segments or variants";
		return false;
	}

	out.is_master = is_master;
	return true;
}

} // namespace tuxedo
