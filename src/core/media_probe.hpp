// Shared URL -> Source + Decoder opener used by both playback and
// offline metadata/properties queries so decoder selection stays
// identical across both paths.
#pragma once

#include "plugin/decoder.hpp"
#include "plugin/source.hpp"

#include <string>

namespace tuxedo {

struct OpenedMedia {
	SourcePtr source;
	DecoderPtr decoder;
	DecoderProperties properties{};
};

bool open_media_url(const std::string &url, OpenedMedia &out, bool skip_cue = false);

} // namespace tuxedo
