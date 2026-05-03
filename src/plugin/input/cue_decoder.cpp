#include "plugin/input/cue_decoder.hpp"

#include "core/cue_sheet.hpp"
#include "core/media_probe.hpp"

#include <algorithm>
#include <utility>

namespace tuxedo {

namespace {

void overlay_metadata(nlohmann::json &target, const nlohmann::json &overlay) {
	for(auto it = overlay.begin(); it != overlay.end(); ++it) {
		target[it.key()] = it.value();
	}
}

} // namespace

CueDecoder::CueDecoder() = default;

CueDecoder::~CueDecoder() { close(); }

bool CueDecoder::open(Source *source) {
	close();
	if(!source) return false;

	CueTrackSelection selection;
	if(!resolve_cue_track(source->url(), selection)) return false;

	const CueTrack &track = selection.sheet.tracks[selection.track_index];
	OpenedMedia opened;
	if(!open_media_url(track.media_url, opened, true)) return false;
	if(!opened.decoder || !opened.properties.format.valid()) return false;

	track_start_ = cue_index_to_frame(track.start, opened.properties.format.sample_rate);
	if(track_start_ < 0) return false;

	track_end_ = cue_track_end_frame(selection, opened.properties.format.sample_rate,
	                                 opened.properties.total_frames)
	                 .value_or(opened.properties.total_frames);
	if(track_end_ >= 0 && track_end_ < track_start_) return false;

	if(track_start_ > 0 && opened.decoder->seek(track_start_) < 0) return false;

	wrapped_source_ = std::move(opened.source);
	wrapped_decoder_ = std::move(opened.decoder);
	props_ = opened.properties;
	if(track_end_ >= 0) {
		props_.total_frames = track_end_ - track_start_;
	} else if(props_.total_frames >= 0) {
		props_.total_frames -= track_start_;
	}

	metadata_ = wrapped_decoder_->metadata();
	metadata_.erase("cuesheet");
	overlay_metadata(metadata_, track.metadata);
	current_frame_ = 0;
	return true;
}

void CueDecoder::close() {
	if(wrapped_decoder_) {
		wrapped_decoder_->close();
		wrapped_decoder_.reset();
	}
	if(wrapped_source_) {
		wrapped_source_->close();
		wrapped_source_.reset();
	}
	props_ = {};
	metadata_ = nlohmann::json::object();
	track_start_ = 0;
	track_end_ = -1;
	current_frame_ = 0;
}

bool CueDecoder::read(AudioChunk &out, size_t max_frames) {
	out = {};
	if(!wrapped_decoder_) return false;
	if(props_.total_frames >= 0 && current_frame_ >= props_.total_frames) return false;

	AudioChunk chunk;
	if(!wrapped_decoder_->read(chunk, max_frames) || chunk.empty()) return false;

	size_t take = chunk.frame_count();
	if(props_.total_frames >= 0) {
		const int64_t remaining = props_.total_frames - current_frame_;
		if(remaining <= 0) return false;
		take = std::min<size_t>(take, static_cast<size_t>(remaining));
	}
	if(take == 0) return false;

	if(take < chunk.frame_count()) chunk = chunk.remove_frames(take);
	chunk.set_stream_timestamp(props_.format.sample_rate
	                               ? static_cast<double>(current_frame_) / props_.format.sample_rate
	                               : 0.0);
	current_frame_ += static_cast<int64_t>(take);
	out = std::move(chunk);
	return true;
}

int64_t CueDecoder::seek(int64_t frame) {
	if(!wrapped_decoder_ || frame < 0) return -1;
	if(props_.total_frames >= 0 && frame > props_.total_frames) return -1;

	const int64_t absolute = track_start_ + frame;
	const int64_t seeked = wrapped_decoder_->seek(absolute);
	if(seeked < 0) return -1;
	current_frame_ = seeked - track_start_;
	return current_frame_;
}

} // namespace tuxedo
