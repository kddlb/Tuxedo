// HLS playlist data structures and RFC 8216 parser. Ported from
// Cog's HLSPlaylist / HLSSegment / HLSVariant / HLSPlaylistParser.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tuxedo {

struct HlsSegment {
	std::string url;
	std::string mime_type; // populated post-fetch
	double duration = 0.0;
	int64_t sequence_number = 0;
	int64_t discontinuity_sequence = 0;
	bool discontinuity = false;

	bool encrypted = false;
	std::string encryption_method;
	std::string encryption_key_url;
	std::vector<uint8_t> iv;

	std::string map_section_url;
	std::string title;
};

struct HlsVariant {
	int64_t bandwidth = 0;
	int64_t average_bandwidth = 0;
	std::string codecs;
	std::string resolution;
	std::string url;
	std::string playlist_url;
};

enum class HlsPlaylistType {
	Unspecified = 0,
	Event = 1,
	VOD = 2,
};

struct HlsPlaylist {
	std::string url; // URL the playlist was fetched from
	bool is_master = false;
	bool is_live = true;
	bool has_endlist = false;
	int version = 1;
	int target_duration = 10;
	int64_t media_sequence = 0;
	int64_t discontinuity_sequence = 0;
	HlsPlaylistType type = HlsPlaylistType::Unspecified;

	std::vector<HlsSegment> segments;
	std::vector<HlsVariant> variants;
};

// Parses `text` as an HLS playlist, with `base_url` used to resolve
// relative URIs. Returns true on success and fills `out`. On failure,
// returns false and fills `error` with a human-readable reason.
bool parse_hls_playlist(const std::string &text,
                        const std::string &base_url,
                        HlsPlaylist &out,
                        std::string &error);

} // namespace tuxedo
