#include "plugin/input/musepack_decoder.hpp"

#include <mpc/mpcdec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace tuxedo {

struct MusepackDecoder::Impl {
	mpc_reader reader{};
	mpc_demux *demux = nullptr;
	mpc_streaminfo info{};
	Source *source = nullptr;
	std::vector<MPC_SAMPLE_FORMAT> sample_buffer = std::vector<MPC_SAMPLE_FORMAT>(MPC_DECODER_BUFFER_LENGTH);
};

namespace {

Source *source_for(mpc_reader *reader) {
	return static_cast<Source *>(reader->data);
}

mpc_int32_t read_cb(mpc_reader *reader, void *ptr, mpc_int32_t size) {
	if(size <= 0) return 0;
	int64_t n = source_for(reader)->read(ptr, static_cast<size_t>(size));
	if(n <= 0) return 0;
	return static_cast<mpc_int32_t>(std::min<int64_t>(n, std::numeric_limits<mpc_int32_t>::max()));
}

mpc_bool_t seek_cb(mpc_reader *reader, mpc_int32_t offset) {
	return offset >= 0 && source_for(reader)->seek(offset, SEEK_SET);
}

mpc_int32_t tell_cb(mpc_reader *reader) {
	int64_t pos = source_for(reader)->tell();
	if(pos < 0) return 0;
	return static_cast<mpc_int32_t>(std::min<int64_t>(pos, std::numeric_limits<mpc_int32_t>::max()));
}

mpc_int32_t get_size_cb(mpc_reader *reader) {
	Source *source = source_for(reader);
	if(!source->seekable()) return 0;

	int64_t here = source->tell();
	if(here < 0) return 0;
	if(!source->seek(0, SEEK_END)) return 0;
	int64_t size = source->tell();
	source->seek(here, SEEK_SET);
	if(size < 0) return 0;
	return static_cast<mpc_int32_t>(std::min<int64_t>(size, std::numeric_limits<mpc_int32_t>::max()));
}

mpc_bool_t canseek_cb(mpc_reader *reader) {
	return source_for(reader)->seekable();
}

void push_string_tag(nlohmann::json &metadata, const char *key, const std::string &value) {
	if(value.empty()) return;
	metadata[key] = nlohmann::json::array({value});
}

void push_musepack_gain(nlohmann::json &metadata, const char *key, mpc_uint16_t q8) {
	if(q8 == 0) return;
	double gain_db = MPC_OLD_GAIN_REF - static_cast<double>(static_cast<int16_t>(q8)) / 256.0;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.2f dB", gain_db);
	metadata[key] = nlohmann::json::array({std::string(buf)});
}

} // namespace

MusepackDecoder::MusepackDecoder() : impl_(new Impl) {}

MusepackDecoder::~MusepackDecoder() {
	close();
	delete impl_;
}

bool MusepackDecoder::open(Source *source) {
	close();
	impl_->source = source;

	impl_->reader.read = read_cb;
	impl_->reader.seek = seek_cb;
	impl_->reader.tell = tell_cb;
	impl_->reader.get_size = get_size_cb;
	impl_->reader.canseek = canseek_cb;
	impl_->reader.data = source;

	impl_->demux = mpc_demux_init(&impl_->reader);
	if(!impl_->demux) {
		impl_->source = nullptr;
		return false;
	}

	mpc_demux_get_info(impl_->demux, &impl_->info);

	props_.format.sample_rate = impl_->info.sample_freq;
	props_.format.channels = impl_->info.channels ? impl_->info.channels : 2;
	props_.total_frames = static_cast<int64_t>(mpc_streaminfo_get_length_samples(&impl_->info));
	props_.codec = "Musepack";

	metadata_["codec"] = "Musepack";
	if(impl_->info.encoder[0] != '\0') push_string_tag(metadata_, "encoder", impl_->info.encoder);
	push_musepack_gain(metadata_, "replaygain_track_gain", impl_->info.gain_title);
	push_musepack_gain(metadata_, "replaygain_album_gain", impl_->info.gain_album);

	return props_.format.valid();
}

void MusepackDecoder::close() {
	if(impl_ && impl_->demux) {
		mpc_demux_exit(impl_->demux);
		impl_->demux = nullptr;
	}
	if(impl_) {
		impl_->reader = {};
		impl_->info = {};
		impl_->source = nullptr;
	}
	props_ = {};
	block_.clear();
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	current_frame_ = 0;
	metadata_ = nlohmann::json::object();
}

bool MusepackDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!impl_->demux || max_frames == 0) return false;

	if(block_frames_consumed_ >= block_frames_) {
		mpc_frame_info frame{};
		frame.buffer = impl_->sample_buffer.data();
		mpc_status status = mpc_demux_decode(impl_->demux, &frame);
		if(status != MPC_STATUS_OK || frame.bits == -1 || frame.samples == 0) return false;

		const size_t channels = props_.format.channels;
		const size_t total_samples = static_cast<size_t>(frame.samples) * channels;
		block_.resize(total_samples);
#ifdef MPC_FIXED_POINT
		const float scale = 1.0f / MPC_FIXED_POINT_SCALE;
		for(size_t i = 0; i < total_samples; ++i) {
			block_[i] = static_cast<float>(impl_->sample_buffer[i]) * scale;
		}
#else
		std::copy_n(impl_->sample_buffer.data(), total_samples, block_.begin());
#endif
		block_frames_ = frame.samples;
		block_frames_consumed_ = 0;
	}

	const size_t channels = props_.format.channels;
	const size_t available_frames = block_frames_ - block_frames_consumed_;
	const size_t take = std::min(available_frames, max_frames);
	const float *src = block_.data() + block_frames_consumed_ * channels;
	std::vector<float> samples(src, src + take * channels);
	block_frames_consumed_ += take;

	out = AudioChunk(props_.format, std::move(samples),
	                 static_cast<double>(current_frame_) / props_.format.sample_rate);
	current_frame_ += static_cast<int64_t>(take);
	return true;
}

int64_t MusepackDecoder::seek(int64_t frame) {
	if(!impl_->demux || frame < 0) return -1;
	if(mpc_demux_seek_sample(impl_->demux, static_cast<mpc_uint64_t>(frame)) != MPC_STATUS_OK) {
		return -1;
	}
	current_frame_ = frame;
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	return frame;
}

nlohmann::json MusepackDecoder::metadata() const {
	nlohmann::json out = metadata_;
	if(impl_ && impl_->source) {
		nlohmann::json source_metadata = impl_->source->metadata();
		for(auto it = source_metadata.begin(); it != source_metadata.end(); ++it) {
			out[it.key()] = it.value();
		}
	}
	out["codec"] = "Musepack";
	return out;
}

} // namespace tuxedo
