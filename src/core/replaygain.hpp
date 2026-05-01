#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace tuxedo {

enum class ReplayGainMode {
	Off,
	Track,
	TrackPeak,
	Album,
	AlbumPeak,
	SoundCheck,
};

const char *replaygain_mode_name(ReplayGainMode mode);
std::optional<ReplayGainMode> replaygain_mode_from_string(const std::string &mode);
float replaygain_scale_for_metadata(const nlohmann::json &metadata, ReplayGainMode mode);

} // namespace tuxedo
