// Mirrors Cog's Audio/Status.h.
#pragma once

namespace tuxedo {

enum class PlaybackStatus {
	Stopped = 0,
	Paused,
	Playing,
	Stopping,
};

inline const char *status_name(PlaybackStatus s) {
	switch(s) {
		case PlaybackStatus::Stopped: return "stopped";
		case PlaybackStatus::Paused: return "paused";
		case PlaybackStatus::Playing: return "playing";
		case PlaybackStatus::Stopping: return "stopping";
	}
	return "unknown";
}

} // namespace tuxedo
