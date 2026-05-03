#include "core/cue_sheet.hpp"

#include "core/media_probe.hpp"
#include "plugin/registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace tuxedo {

namespace {

std::string trim(std::string s) {
	size_t start = 0;
	while(start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
	size_t end = s.size();
	while(end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
	return s.substr(start, end - start);
}

std::string lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

std::string strip_file_prefix(const std::string &url) {
	static const std::string file_prefix = "file://";
	if(url.compare(0, file_prefix.size(), file_prefix) == 0) return url.substr(file_prefix.size());
	return url;
}

bool has_scheme(const std::string &url) {
	return !PluginRegistry::scheme_of(url).empty();
}

bool is_all_digits(const std::string &value) {
	return !value.empty() &&
	       std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::optional<int> parse_track_number(const std::string &value) {
	if(!is_all_digits(value)) return std::nullopt;
	try {
		return std::stoi(value);
	} catch(...) {
		return std::nullopt;
	}
}

std::string quoted_or_remainder(std::string value) {
	value = trim(std::move(value));
	if(value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		return value.substr(1, value.size() - 2);
	}
	if(!value.empty() && value.front() == '"') {
		value.erase(value.begin());
		size_t quote = value.find('"');
		if(quote != std::string::npos) value.erase(quote);
	}
	return trim(std::move(value));
}

std::string normalized_track_fragment(int track_number, const std::string &raw) {
	if(track_number < 0) return raw;
	std::ostringstream oss;
	oss << std::setw(2) << std::setfill('0') << track_number;
	return oss.str();
}

std::string origin_of(const std::string &url) {
	size_t scheme = url.find("://");
	if(scheme == std::string::npos) return {};
	size_t slash = url.find('/', scheme + 3);
	return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string directory_of_url(const std::string &url) {
	size_t slash = url.find_last_of('/');
	return slash == std::string::npos ? url : url.substr(0, slash + 1);
}

std::string resolve_relative_url(const std::string &entry, const std::string &base_url) {
	if(entry.empty()) return {};
	if(has_scheme(entry)) return entry;

	std::string path = entry;
	std::replace(path.begin(), path.end(), '\\', '/');

	const std::string base_scheme = PluginRegistry::scheme_of(base_url);
	if(base_scheme == "http" || base_scheme == "https") {
		if(!path.empty() && path[0] == '/') return origin_of(base_url) + path;
		return directory_of_url(base_url) + path;
	}

	const std::string base_path = strip_file_prefix(base_url);
	std::filesystem::path resolved = std::filesystem::path(path).is_absolute()
	    ? std::filesystem::path(path)
	    : std::filesystem::path(base_path).parent_path() / path;
	return resolved.lexically_normal().string();
}

bool read_all_text(const std::string &url, std::string &out) {
	out.clear();
	auto source = PluginRegistry::instance().source_for_url(url);
	if(!source || !source->open(url)) return false;

	char buf[4096];
	for(;;) {
		int64_t n = source->read(buf, sizeof(buf));
		if(n < 0) {
			source->close();
			return false;
		}
		if(n == 0) break;
		out.append(buf, static_cast<size_t>(n));
	}
	source->close();
	return true;
}

std::vector<std::string> split_lines(std::string text) {
	for(char &c : text) if(c == '\r') c = '\n';
	std::vector<std::string> lines;
	std::istringstream iss(text);
	std::string line;
	while(std::getline(iss, line, '\n')) lines.push_back(line);
	return lines;
}

void set_metadata_field(nlohmann::json &metadata, const char *key, const std::string &value) {
	if(value.empty()) return;
	metadata[key] = nlohmann::json::array({value});
}

void seed_track_metadata(CueTrack &track, const std::string &artist, const std::string &album,
                         const std::string &genre, const std::string &date,
                         const std::string &album_gain, const std::string &album_peak) {
	set_metadata_field(track.metadata, "artist", artist);
	set_metadata_field(track.metadata, "album", album);
	set_metadata_field(track.metadata, "genre", genre);
	set_metadata_field(track.metadata, "date", date);
	set_metadata_field(track.metadata, "replaygain_album_gain", album_gain);
	set_metadata_field(track.metadata, "replaygain_album_peak", album_peak);
	if(track.track_number > 0) {
		set_metadata_field(track.metadata, "tracknumber", std::to_string(track.track_number));
	}
}

bool parse_index_position(const std::string &value, CueIndex &out) {
	out = {};
	if(is_all_digits(value)) {
		try {
			out.value = std::stoll(value);
			out.in_samples = true;
			return true;
		} catch(...) {
			return false;
		}
	}

	std::istringstream iss(value);
	std::string minute_token;
	std::string second_token;
	std::string frame_token;
	if(!std::getline(iss, minute_token, ':')) return false;
	if(!std::getline(iss, second_token, ':')) return false;
	if(!std::getline(iss, frame_token)) return false;
	if(!is_all_digits(minute_token) || !is_all_digits(second_token) || !is_all_digits(frame_token)) {
		return false;
	}

	const int minutes = std::stoi(minute_token);
	const int seconds = std::stoi(second_token);
	const int frames = std::stoi(frame_token);
	if(seconds >= 60 || frames >= 75) return false;

	out.value = static_cast<int64_t>(((minutes * 60) + seconds) * 75 + frames);
	out.in_samples = false;
	return true;
}

bool extract_cuesheet_text(const nlohmann::json &metadata, std::string &out) {
	out.clear();
	auto it = metadata.find("cuesheet");
	if(it == metadata.end()) return false;

	if(it->is_string()) {
		out = it->get<std::string>();
		return !out.empty();
	}
	if(it->is_array() && !it->empty() && (*it)[0].is_string()) {
		out = (*it)[0].get<std::string>();
		return !out.empty();
	}
	return false;
}

bool parse_cuesheet_text(const std::string &text, const std::string &source_url, bool embedded,
                         CueSheet &out) {
	out = {};
	out.source_url = cue_strip_fragment(source_url);
	out.embedded = embedded;

	std::string current_file;
	std::string album_artist;
	std::string album_title;
	std::string album_genre;
	std::string album_date;
	std::string album_gain;
	std::string album_peak;
	CueTrack pending;
	bool have_pending = false;

	for(const std::string &raw : split_lines(text)) {
		const std::string line = trim(raw);
		if(line.empty()) continue;

		const size_t split = line.find_first_of(" \t");
		const std::string command = lowercase(split == std::string::npos ? line : line.substr(0, split));
		const std::string rest = split == std::string::npos ? std::string{} : trim(line.substr(split + 1));

		if(command == "file") {
			current_file = resolve_relative_url(quoted_or_remainder(rest), out.source_url);
			have_pending = false;
			continue;
		}

		if(command == "track") {
			std::istringstream iss(rest);
			std::string track_token;
			std::string type_token;
			if(!(iss >> track_token >> type_token)) continue;
			if(lowercase(type_token) != "audio") {
				have_pending = false;
				continue;
			}

			pending = {};
			pending.media_url = current_file;
			pending.track_number = parse_track_number(track_token).value_or(0);
			pending.fragment = normalized_track_fragment(pending.track_number, track_token);
			seed_track_metadata(pending, album_artist, album_title, album_genre, album_date,
			                    album_gain, album_peak);
			have_pending = true;
			continue;
		}

		if(command == "index") {
			if(!have_pending || pending.media_url.empty()) continue;
			std::istringstream iss(rest);
			std::string index_token;
			std::string position_token;
			if(!(iss >> index_token >> position_token)) continue;
			if(index_token != "01") continue;
			if(!parse_index_position(position_token, pending.start)) continue;
			out.tracks.push_back(pending);
			have_pending = false;
			continue;
		}

		if(command == "performer") {
			const std::string value = quoted_or_remainder(rest);
			if(have_pending) {
				set_metadata_field(pending.metadata, "artist", value);
			} else {
				album_artist = value;
			}
			continue;
		}

		if(command == "title") {
			const std::string value = quoted_or_remainder(rest);
			if(have_pending) {
				set_metadata_field(pending.metadata, "title", value);
			} else {
				album_title = value;
			}
			continue;
		}

		if(command == "isrc") {
			if(!have_pending) continue;
			set_metadata_field(pending.metadata, "isrc", quoted_or_remainder(rest));
			continue;
		}

		if(command == "rem") {
			const size_t rem_split = rest.find_first_of(" \t");
			const std::string rem_type = lowercase(rem_split == std::string::npos ? rest : rest.substr(0, rem_split));
			const std::string rem_value =
			    quoted_or_remainder(rem_split == std::string::npos ? std::string{} : rest.substr(rem_split + 1));

			if(rem_type == "genre") {
				if(have_pending) set_metadata_field(pending.metadata, "genre", rem_value);
				else album_genre = rem_value;
			} else if(rem_type == "date") {
				if(have_pending) set_metadata_field(pending.metadata, "date", rem_value);
				else album_date = rem_value;
			} else if(rem_type == "replaygain_album_gain") {
				if(have_pending) set_metadata_field(pending.metadata, "replaygain_album_gain", rem_value);
				album_gain = rem_value;
			} else if(rem_type == "replaygain_album_peak") {
				if(have_pending) set_metadata_field(pending.metadata, "replaygain_album_peak", rem_value);
				album_peak = rem_value;
			} else if(rem_type == "replaygain_track_gain") {
				if(have_pending) set_metadata_field(pending.metadata, "replaygain_track_gain", rem_value);
			} else if(rem_type == "replaygain_track_peak") {
				if(have_pending) set_metadata_field(pending.metadata, "replaygain_track_peak", rem_value);
			}
		}
	}

	return !out.tracks.empty();
}

bool load_external_cuesheet(const std::string &url, CueSheet &out) {
	std::string text;
	if(!read_all_text(url, text)) return false;
	return parse_cuesheet_text(text, url, false, out);
}

bool load_embedded_cuesheet(const std::string &url, CueSheet &out) {
	OpenedMedia opened;
	if(!open_media_url(cue_strip_fragment(url), opened, true)) return false;

	std::string text;
	if(!opened.decoder || !extract_cuesheet_text(opened.decoder->metadata(), text)) return false;
	return parse_cuesheet_text(text, cue_strip_fragment(url), true, out);
}

} // namespace

std::string cue_strip_fragment(const std::string &url) {
	const size_t hash = url.rfind('#');
	return hash == std::string::npos ? url : url.substr(0, hash);
}

std::string cue_fragment(const std::string &url) {
	const size_t hash = url.rfind('#');
	return hash == std::string::npos ? std::string{} : url.substr(hash + 1);
}

bool cue_has_track_fragment(const std::string &url) {
	return is_all_digits(cue_fragment(url));
}

bool load_cuesheet_for_url(const std::string &url, CueSheet &out) {
	const std::string base_url = cue_strip_fragment(url);
	const std::string ext = lowercase(PluginRegistry::extension_of(base_url));
	if(ext == "cue") return load_external_cuesheet(base_url, out);
	if(ext == "flac") return load_embedded_cuesheet(base_url, out);
	return false;
}

bool resolve_cue_track(const std::string &url, CueTrackSelection &out) {
	out = {};
	if(!cue_has_track_fragment(url)) return false;
	if(!load_cuesheet_for_url(url, out.sheet)) return false;

	const std::string fragment = cue_fragment(url);
	const auto numeric_fragment = parse_track_number(fragment);
	for(size_t i = 0; i < out.sheet.tracks.size(); ++i) {
		const CueTrack &track = out.sheet.tracks[i];
		if(track.fragment == fragment) {
			out.track_index = i;
			return true;
		}
		if(numeric_fragment && track.track_number == *numeric_fragment) {
			out.track_index = i;
			return true;
		}
	}
	return false;
}

std::vector<std::string> cue_track_urls(const CueSheet &sheet, const std::string &base_url) {
	std::vector<std::string> out;
	out.reserve(sheet.tracks.size());
	const std::string base = cue_strip_fragment(base_url);
	for(const CueTrack &track : sheet.tracks) {
		out.push_back(base + "#" + track.fragment);
	}
	return out;
}

int64_t cue_index_to_frame(const CueIndex &index, int sample_rate) {
	if(index.in_samples) return index.value;
	if(sample_rate <= 0) return -1;
	return (index.value * sample_rate) / 75;
}

std::optional<int64_t> cue_track_end_frame(const CueTrackSelection &selection, int sample_rate,
                                           int64_t total_frames) {
	if(selection.track_index >= selection.sheet.tracks.size()) return std::nullopt;
	const std::string current_media = selection.sheet.tracks[selection.track_index].media_url;
	for(size_t i = selection.track_index + 1; i < selection.sheet.tracks.size(); ++i) {
		if(selection.sheet.tracks[i].media_url == current_media) {
			return cue_index_to_frame(selection.sheet.tracks[i].start, sample_rate);
		}
		return total_frames >= 0 ? std::optional<int64_t>(total_frames) : std::nullopt;
	}
	return total_frames >= 0 ? std::optional<int64_t>(total_frames) : std::nullopt;
}

} // namespace tuxedo
