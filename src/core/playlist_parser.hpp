#pragma once

#include <string>
#include <vector>

namespace tuxedo {

struct PlaylistParseResult {
	bool recognized = false;
	bool passthrough_original = false;
	std::vector<std::string> urls;
};

PlaylistParseResult parse_playlist_url(const std::string &url);

} // namespace tuxedo
