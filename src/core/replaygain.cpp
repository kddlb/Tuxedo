#include "core/replaygain.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace tuxedo {

namespace {

std::optional<std::string> first_string(const nlohmann::json &metadata, const char *key) {
	auto it = metadata.find(key);
	if(it == metadata.end()) return std::nullopt;
	if(it->is_string()) return it->get<std::string>();
	if(it->is_array() && !it->empty() && (*it)[0].is_string()) return (*it)[0].get<std::string>();
	return std::nullopt;
}

std::optional<double> parse_double(const std::string &text) {
	char *end = nullptr;
	double value = std::strtod(text.c_str(), &end);
	if(end == text.c_str()) return std::nullopt;
	return value;
}

std::optional<double> parse_hex(const std::string &text) {
	char *end = nullptr;
	unsigned long value = std::strtoul(text.c_str(), &end, 16);
	if(end == text.c_str() || *end != '\0') return std::nullopt;
	return static_cast<double>(value);
}

std::optional<double> parse_json_number(const nlohmann::json &metadata, const char *key) {
	auto it = metadata.find(key);
	if(it == metadata.end()) return std::nullopt;
	if(it->is_number()) return it->get<double>();
	return std::nullopt;
}

std::optional<double> parse_db_value(const nlohmann::json &metadata, const char *key) {
	if(auto numeric = parse_json_number(metadata, key)) return numeric;
	auto text = first_string(metadata, key);
	if(!text) return std::nullopt;
	return parse_double(*text);
}

std::optional<double> parse_q8_gain(const nlohmann::json &metadata, const char *key) {
	auto it = metadata.find(key);
	if(it == metadata.end() || !it->is_number_integer()) return std::nullopt;
	return static_cast<double>(it->get<int32_t>()) / 256.0;
}

std::optional<double> soundcheck_gain_db(const nlohmann::json &metadata) {
	auto text = first_string(metadata, "soundcheck");
	if(!text) return std::nullopt;

	std::vector<std::string> fields;
	size_t start = 0;
	while(start < text->size()) {
		size_t end = text->find(' ', start);
		if(end == std::string::npos) end = text->size();
		if(end > start && end - start == 8) fields.push_back(text->substr(start, end - start));
		start = end + 1;
	}
	if(fields.size() < 2) return std::nullopt;

	auto a = parse_hex(fields[0]);
	auto b = parse_hex(fields[1]);
	if(!a || !b || *a <= 0.0 || *b <= 0.0) return std::nullopt;

	double volume1 = -std::log10(*a / 1000.0) * 10.0;
	double volume2 = -std::log10(*b / 1000.0) * 10.0;
	return std::min(volume1, volume2);
}

struct GainInfo {
	std::optional<double> gain_db;
	std::optional<double> peak;
};

GainInfo gain_info_for_mode(const nlohmann::json &metadata, ReplayGainMode mode) {
	GainInfo info;
	switch(mode) {
		case ReplayGainMode::Track:
		case ReplayGainMode::TrackPeak:
			info.gain_db = parse_db_value(metadata, "replaygain_track_gain");
			if(!info.gain_db) info.gain_db = parse_q8_gain(metadata, "r128_track_gain_q8");
			info.peak = parse_db_value(metadata, "replaygain_track_peak");
			break;
		case ReplayGainMode::Album:
		case ReplayGainMode::AlbumPeak:
			info.gain_db = parse_db_value(metadata, "replaygain_album_gain");
			if(!info.gain_db) info.gain_db = parse_q8_gain(metadata, "r128_album_gain_q8");
			info.peak = parse_db_value(metadata, "replaygain_album_peak");
			break;
		case ReplayGainMode::SoundCheck:
			info.gain_db = soundcheck_gain_db(metadata);
			break;
		case ReplayGainMode::Off:
			break;
	}
	return info;
}

bool use_peak(ReplayGainMode mode) {
	return mode == ReplayGainMode::TrackPeak || mode == ReplayGainMode::AlbumPeak;
}

} // namespace

const char *replaygain_mode_name(ReplayGainMode mode) {
	switch(mode) {
		case ReplayGainMode::Off: return "off";
		case ReplayGainMode::Track: return "track";
		case ReplayGainMode::TrackPeak: return "track_peak";
		case ReplayGainMode::Album: return "album";
		case ReplayGainMode::AlbumPeak: return "album_peak";
		case ReplayGainMode::SoundCheck: return "soundcheck";
	}
	return "off";
}

std::optional<ReplayGainMode> replaygain_mode_from_string(const std::string &mode) {
	if(mode == "off") return ReplayGainMode::Off;
	if(mode == "track") return ReplayGainMode::Track;
	if(mode == "track_peak") return ReplayGainMode::TrackPeak;
	if(mode == "album") return ReplayGainMode::Album;
	if(mode == "album_peak") return ReplayGainMode::AlbumPeak;
	if(mode == "soundcheck") return ReplayGainMode::SoundCheck;
	return std::nullopt;
}

float replaygain_scale_for_metadata(const nlohmann::json &metadata, ReplayGainMode mode) {
	if(mode == ReplayGainMode::Off) return 1.0f;

	GainInfo info = gain_info_for_mode(metadata, mode);
	double scale = info.gain_db ? std::pow(10.0, *info.gain_db / 20.0) : 1.0;
	if(use_peak(mode) && info.peak && *info.peak > 0.0 && scale * *info.peak > 1.0) {
		scale = 1.0 / *info.peak;
	}
	return static_cast<float>(scale);
}

} // namespace tuxedo
