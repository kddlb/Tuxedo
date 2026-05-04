#include "plugin/input/silence_decoder.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace tuxedo {

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kDefaultSeconds = 10;

int64_t parse_duration_seconds(const std::string &url) {
	const std::string scheme = "silence:";
	if(url.compare(0, scheme.size(), scheme) != 0) return kDefaultSeconds;

	size_t start = scheme.size();
	while(start < url.size() && url[start] == '/') ++start;
	size_t end = url.find_first_of("?#", start);
	if(end == std::string::npos) end = url.size();
	if(start >= end) return kDefaultSeconds;

	char *tail = nullptr;
	double seconds = std::strtod(url.c_str() + start, &tail);
	if(!tail || tail != url.c_str() + end || seconds <= 0.0) return kDefaultSeconds;

	double frames = seconds * static_cast<double>(kSampleRate);
	if(frames < 1.0) return 1;
	return static_cast<int64_t>(frames);
}

} // namespace

bool SilenceDecoder::open(Source *source) {
	close();
	if(!source) return false;

	props_.format.sample_rate = kSampleRate;
	props_.format.channels = kChannels;
	props_.total_frames = parse_duration_seconds(source->url());
	props_.codec = "synthesized";
	current_frame_ = 0;
	return true;
}

void SilenceDecoder::close() {
	props_ = {};
	current_frame_ = 0;
}

bool SilenceDecoder::read(AudioChunk &out, size_t max_frames) {
	out = {};
	if(!props_.format.valid() || max_frames == 0) return false;
	if(props_.total_frames >= 0 && current_frame_ >= props_.total_frames) return false;

	size_t frames = max_frames;
	if(props_.total_frames >= 0) {
		const int64_t remaining = props_.total_frames - current_frame_;
		if(remaining <= 0) return false;
		frames = std::min<size_t>(frames, static_cast<size_t>(remaining));
	}
	if(frames == 0) return false;

	std::vector<float> samples(frames * props_.format.channels, 0.0f);
	const double timestamp = static_cast<double>(current_frame_) / props_.format.sample_rate;
	out = AudioChunk(props_.format, std::move(samples), timestamp);
	current_frame_ += static_cast<int64_t>(frames);
	return true;
}

int64_t SilenceDecoder::seek(int64_t frame) {
	if(!props_.format.valid() || frame < 0) return -1;
	if(props_.total_frames >= 0) frame = std::min(frame, props_.total_frames);
	current_frame_ = frame;
	return current_frame_;
}

} // namespace tuxedo
