#include "plugin/input/ffmpeg_decoder.hpp"

#include "plugin/input/vorbis_common.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace tuxedo {

namespace {

constexpr int kAvioBufferSize = 32 * 1024;

int read_packet(void *opaque, uint8_t *buf, int buf_size) {
	auto *source = static_cast<Source *>(opaque);
	int64_t read = source->read(buf, static_cast<size_t>(buf_size));
	if(read < 0) return AVERROR(EIO);
	if(read == 0) return AVERROR_EOF;
	return static_cast<int>(read);
}

int64_t seek_packet(void *opaque, int64_t offset, int whence) {
	auto *source = static_cast<Source *>(opaque);
	if(whence == AVSEEK_SIZE) {
		if(!source->seekable()) return -1;
		int64_t here = source->tell();
		if(here < 0) return -1;
		if(!source->seek(0, SEEK_END)) return -1;
		int64_t size = source->tell();
		source->seek(here, SEEK_SET);
		return size;
	}

	int origin = SEEK_SET;
	switch(whence & ~AVSEEK_FORCE) {
		case SEEK_SET: origin = SEEK_SET; break;
		case SEEK_CUR: origin = SEEK_CUR; break;
		case SEEK_END: origin = SEEK_END; break;
		default: return -1;
	}

	if(!source->seek(offset, origin)) return -1;
	return source->tell();
}

void push_tag(nlohmann::json &tags, const std::string &key, const std::string &value) {
	if(value.empty()) return;
	auto it = tags.find(key);
	if(it == tags.end()) {
		tags[key] = nlohmann::json::array({value});
	} else {
		it->push_back(value);
	}
}

std::string canonical_tag_name(const std::string &raw) {
	std::string lower = vorbis_common::lowercase(raw);
	if(lower == "unsynced lyrics" || lower == "lyrics") return "unsyncedlyrics";
	if(lower == "icy-url") return "album";
	if(lower == "icy-genre") return "genre";
	if(lower == "album_artist") return "albumartist";
	if(lower == "track") return "tracknumber";
	if(lower == "disc") return "discnumber";
	if(lower == "date_recorded") return "date";
	if(lower == "itunnorm") return "soundcheck";
	if(lower == "replaygain_gain") return "replaygain_album_gain";
	if(lower == "replaygain_peak") return "replaygain_album_peak";
	return vorbis_common::canonicalise_tag(lower);
}

std::string codec_label(const AVCodecParameters *codecpar) {
	const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
	if(desc && desc->name) {
		std::string name = desc->name;
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return name;
	}
	return "FFmpeg";
}

std::string picture_mime_for_codec(AVCodecID codec_id) {
	switch(codec_id) {
		case AV_CODEC_ID_MJPEG:
		case AV_CODEC_ID_JPEG2000:
			return "image/jpeg";
		case AV_CODEC_ID_PNG:
			return "image/png";
		case AV_CODEC_ID_BMP:
			return "image/bmp";
		case AV_CODEC_ID_GIF:
			return "image/gif";
		case AV_CODEC_ID_TIFF:
			return "image/tiff";
		default:
			return "application/octet-stream";
	}
}

} // namespace

FfmpegDecoder::FfmpegDecoder() = default;

FfmpegDecoder::~FfmpegDecoder() { close(); }

bool FfmpegDecoder::open(Source *source) {
	close();
	source_ = source;

	unsigned char *avio_buffer = static_cast<unsigned char *>(av_malloc(kAvioBufferSize));
	if(!avio_buffer) return false;

	io_ctx_ = avio_alloc_context(
	    avio_buffer, kAvioBufferSize, 0, source_,
	    &read_packet, nullptr, source_->seekable() ? &seek_packet : nullptr);
	if(!io_ctx_) {
		av_free(avio_buffer);
		return false;
	}

	format_ctx_ = avformat_alloc_context();
	if(!format_ctx_) return false;
	format_ctx_->pb = io_ctx_;
	format_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;

	if(avformat_open_input(&format_ctx_, nullptr, nullptr, nullptr) < 0) return false;
	if(avformat_find_stream_info(format_ctx_, nullptr) < 0) return false;

	stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if(stream_index_ < 0) return false;

	AVStream *stream = format_ctx_->streams[stream_index_];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if(!codec) return false;

	codec_ctx_ = avcodec_alloc_context3(codec);
	if(!codec_ctx_) return false;
	if(avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) return false;
	if(avcodec_open2(codec_ctx_, codec, nullptr) < 0) return false;

	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if(!frame_ || !packet_) return false;

	AVChannelLayout in_layout{};
	if(codec_ctx_->ch_layout.nb_channels > 0) {
		if(av_channel_layout_copy(&in_layout, &codec_ctx_->ch_layout) < 0) return false;
	} else {
		int channels = stream->codecpar->ch_layout.nb_channels;
		if(channels <= 0) return false;
		av_channel_layout_default(&in_layout, channels);
	}

	AVChannelLayout out_layout{};
	if(av_channel_layout_copy(&out_layout, &in_layout) < 0) {
		av_channel_layout_uninit(&in_layout);
		return false;
	}

	if(swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, codec_ctx_->sample_rate,
	                       &in_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate,
	                       0, nullptr) < 0) {
		av_channel_layout_uninit(&out_layout);
		av_channel_layout_uninit(&in_layout);
		return false;
	}
	av_channel_layout_uninit(&out_layout);
	av_channel_layout_uninit(&in_layout);
	if(swr_init(swr_) < 0) return false;

	props_.format.sample_rate = codec_ctx_->sample_rate;
	props_.format.channels = codec_ctx_->ch_layout.nb_channels > 0
	    ? codec_ctx_->ch_layout.nb_channels
	    : stream->codecpar->ch_layout.nb_channels;
	if(!props_.format.channels) return false;
	props_.codec = codec_label(stream->codecpar);

	if(stream->duration > 0) {
		double seconds = av_q2d(stream->time_base) * static_cast<double>(stream->duration);
		props_.total_frames = static_cast<int64_t>(seconds * props_.format.sample_rate);
	} else if(format_ctx_->duration > 0) {
		double seconds = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
		props_.total_frames = static_cast<int64_t>(seconds * props_.format.sample_rate);
	} else {
		props_.total_frames = -1;
	}

	for(unsigned int i = 0; i < format_ctx_->nb_streams; ++i) {
		AVStream *candidate = format_ctx_->streams[i];
		if(candidate->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			attached_picture_index_ = static_cast<int>(i);
			break;
		}
	}

	if(attached_picture_index_ >= 0) {
		AVStream *pic = format_ctx_->streams[attached_picture_index_];
		if(pic->attached_pic.size > 0 && pic->attached_pic.data) {
			picture_mime_ = picture_mime_for_codec(pic->codecpar->codec_id);
			picture_bytes_.assign(pic->attached_pic.data,
			                      pic->attached_pic.data + pic->attached_pic.size);
		}
	}

	rebuild_metadata();
	return props_.format.valid();
}

void FfmpegDecoder::close() {
	if(packet_) {
		av_packet_free(&packet_);
		packet_ = nullptr;
	}
	if(frame_) {
		av_frame_free(&frame_);
		frame_ = nullptr;
	}
	if(codec_ctx_) {
		avcodec_free_context(&codec_ctx_);
		codec_ctx_ = nullptr;
	}
	if(format_ctx_) {
		avformat_close_input(&format_ctx_);
		format_ctx_ = nullptr;
	}
	if(io_ctx_) {
		avio_context_free(&io_ctx_);
		io_ctx_ = nullptr;
	}
	if(swr_) {
		swr_free(&swr_);
		swr_ = nullptr;
	}

	source_ = nullptr;
	props_ = {};
	stream_index_ = -1;
	attached_picture_index_ = -1;
	packet_eof_ = false;
	audio_eof_ = false;
	pending_samples_.clear();
	pending_offset_ = 0;
	pending_timestamp_ = 0.0;
	current_frame_ = 0;
	base_metadata_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
}

bool FfmpegDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!codec_ctx_ || max_frames == 0) return false;

	const size_t channels = props_.format.channels;
	std::vector<float> samples;
	samples.reserve(max_frames * channels);
	double timestamp = 0.0;
	bool timestamp_set = false;

	while(samples.size() / channels < max_frames) {
		if(pending_offset_ < pending_samples_.size()) {
			if(!timestamp_set) {
				timestamp = pending_timestamp_;
				timestamp_set = true;
			}

			size_t pending_frames = (pending_samples_.size() - pending_offset_) / channels;
			size_t take_frames = std::min(max_frames - (samples.size() / channels), pending_frames);
			size_t take_samples = take_frames * channels;
			samples.insert(samples.end(),
			               pending_samples_.begin() + pending_offset_,
			               pending_samples_.begin() + pending_offset_ + take_samples);
			pending_offset_ += take_samples;
			if(pending_offset_ >= pending_samples_.size()) {
				pending_samples_.clear();
				pending_offset_ = 0;
			}
			continue;
		}

		if(audio_eof_ || !decode_more()) break;
	}

	if(samples.empty()) return false;
	out = AudioChunk(props_.format, std::move(samples), timestamp);
	return true;
}

int64_t FfmpegDecoder::seek(int64_t frame) {
	if(!format_ctx_ || !codec_ctx_ || stream_index_ < 0 || !source_ || !source_->seekable()) return -1;

	AVStream *stream = format_ctx_->streams[stream_index_];
	int64_t target = av_rescale_q(frame, AVRational{1, static_cast<int>(props_.format.sample_rate)},
	                              stream->time_base);
	if(av_seek_frame(format_ctx_, stream_index_, target, AVSEEK_FLAG_BACKWARD) < 0) return -1;

	avcodec_flush_buffers(codec_ctx_);
	if(swr_) swr_init(swr_);
	packet_eof_ = false;
	audio_eof_ = false;
	pending_samples_.clear();
	pending_offset_ = 0;
	pending_timestamp_ = static_cast<double>(frame) / props_.format.sample_rate;
	current_frame_ = frame;
	rebuild_metadata();
	return frame;
}

nlohmann::json FfmpegDecoder::metadata() const {
	nlohmann::json out = base_metadata_;
	if(source_) {
		nlohmann::json source_metadata = source_->metadata();
		for(auto it = source_metadata.begin(); it != source_metadata.end(); ++it) {
			out[it.key()] = it.value();
		}
	}
	out["codec"] = props_.codec;
	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", vorbis_common::base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}
	return out;
}

bool FfmpegDecoder::decode_more() {
	if(audio_eof_) return false;

	for(;;) {
		int rc = avcodec_receive_frame(codec_ctx_, frame_);
		if(rc == 0) {
			bool appended = append_decoded_frame();
			av_frame_unref(frame_);
			if(appended) return true;
			continue;
		}
		if(rc == AVERROR_EOF) {
			return drain_resampler();
		}
		if(rc != AVERROR(EAGAIN)) {
			audio_eof_ = true;
			return false;
		}

		while(!packet_eof_) {
			rc = av_read_frame(format_ctx_, packet_);
			if(rc < 0) {
				packet_eof_ = true;
				avcodec_send_packet(codec_ctx_, nullptr);
				break;
			}

			if(packet_->stream_index != stream_index_) {
				av_packet_unref(packet_);
				continue;
			}

			rc = avcodec_send_packet(codec_ctx_, packet_);
			av_packet_unref(packet_);
			if(rc == 0 || rc == AVERROR(EAGAIN)) break;
			audio_eof_ = true;
			return false;
		}

		if(packet_eof_) return drain_resampler() || !audio_eof_;
	}
}

bool FfmpegDecoder::append_decoded_frame() {
	if(!frame_) return false;

	const int out_frames = av_rescale_rnd(
	    swr_get_delay(swr_, codec_ctx_->sample_rate) + frame_->nb_samples,
	    codec_ctx_->sample_rate, codec_ctx_->sample_rate, AV_ROUND_UP);
	if(out_frames <= 0) return false;

	std::vector<float> converted(static_cast<size_t>(out_frames) * props_.format.channels);
	uint8_t *out_data[] = {reinterpret_cast<uint8_t *>(converted.data())};
	int produced = swr_convert(swr_, out_data, out_frames,
	                           const_cast<const uint8_t **>(frame_->extended_data),
	                           frame_->nb_samples);
	if(produced < 0) {
		audio_eof_ = true;
		return false;
	}
	if(produced == 0) return false;

	converted.resize(static_cast<size_t>(produced) * props_.format.channels);
	pending_offset_ = 0;
	pending_timestamp_ = frame_->best_effort_timestamp != AV_NOPTS_VALUE
	    ? av_q2d(format_ctx_->streams[stream_index_]->time_base) * frame_->best_effort_timestamp
	    : static_cast<double>(current_frame_) / props_.format.sample_rate;
	current_frame_ += produced;
	pending_samples_ = std::move(converted);
	return true;
}

bool FfmpegDecoder::drain_resampler() {
	if(!swr_) {
		audio_eof_ = true;
		return false;
	}

	const int out_frames = swr_get_out_samples(swr_, 0);
	if(out_frames <= 0) {
		audio_eof_ = true;
		return false;
	}

	std::vector<float> converted(static_cast<size_t>(out_frames) * props_.format.channels);
	uint8_t *out_data[] = {reinterpret_cast<uint8_t *>(converted.data())};
	int produced = swr_convert(swr_, out_data, out_frames, nullptr, 0);
	if(produced <= 0) {
		audio_eof_ = true;
		return false;
	}

	converted.resize(static_cast<size_t>(produced) * props_.format.channels);
	pending_offset_ = 0;
	pending_timestamp_ = static_cast<double>(current_frame_) / props_.format.sample_rate;
	current_frame_ += produced;
	pending_samples_ = std::move(converted);
	return true;
}

void FfmpegDecoder::rebuild_metadata() {
	base_metadata_ = nlohmann::json::object();
	if(!format_ctx_) return;

	auto accept_dict = [this](AVDictionary *dict) {
		AVDictionaryEntry *tag = nullptr;
		while((tag = av_dict_get(dict, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
			std::string key = canonical_tag_name(tag->key ? tag->key : "");
			std::string value = tag->value ? tag->value : "";
			push_tag(base_metadata_, key, value);
		}
	};

	accept_dict(format_ctx_->metadata);
	if(stream_index_ >= 0) accept_dict(format_ctx_->streams[stream_index_]->metadata);
}

} // namespace tuxedo
