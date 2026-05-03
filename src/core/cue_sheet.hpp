#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tuxedo {

struct CueIndex {
	int64_t value = 0;
	bool in_samples = false;
};

struct CueTrack {
	std::string fragment;
	int track_number = 0;
	std::string media_url;
	CueIndex start{};
	nlohmann::json metadata = nlohmann::json::object();
};

struct CueSheet {
	std::string source_url;
	bool embedded = false;
	std::vector<CueTrack> tracks;
};

struct CueTrackSelection {
	CueSheet sheet;
	size_t track_index = 0;
};

std::string cue_strip_fragment(const std::string &url);
std::string cue_fragment(const std::string &url);
bool cue_has_track_fragment(const std::string &url);

bool load_cuesheet_for_url(const std::string &url, CueSheet &out);
bool resolve_cue_track(const std::string &url, CueTrackSelection &out);
std::vector<std::string> cue_track_urls(const CueSheet &sheet, const std::string &base_url);

int64_t cue_index_to_frame(const CueIndex &index, int sample_rate);
std::optional<int64_t> cue_track_end_frame(const CueTrackSelection &selection, int sample_rate,
                                           int64_t total_frames);

} // namespace tuxedo
