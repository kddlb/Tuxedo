#include "plugin/input/flac_decoder.hpp"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tuxedo {

namespace {

FLAC__StreamDecoder *cast(void *p) { return static_cast<FLAC__StreamDecoder *>(p); }

FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *, FLAC__byte buf[],
                                      size_t *bytes, void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	int64_t n = self->source()->read(buf, *bytes);
	if(n < 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	if(n == 0) {
		*bytes = 0;
		self->set_end_of_stream(true);
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	*bytes = static_cast<size_t>(n);
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder *, FLAC__uint64 offset,
                                      void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	if(!self->source()->seek(static_cast<int64_t>(offset), SEEK_SET))
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	self->set_end_of_stream(false);
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder *, FLAC__uint64 *offset,
                                      void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	int64_t pos = self->source()->tell();
	if(pos < 0) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
	*offset = static_cast<FLAC__uint64>(pos);
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder *, FLAC__uint64 *len,
                                          void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	int64_t sz = self->file_size();
	if(sz <= 0) {
		*len = 0;
		return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
	}
	*len = static_cast<FLAC__uint64>(sz);
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool eof_cb(const FLAC__StreamDecoder *, void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	return self->end_of_stream() ? 1 : 0;
}

FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *,
                                        const FLAC__Frame *frame,
                                        const FLAC__int32 *const planes[],
                                        void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	self->accept_block(planes, frame->header.blocksize, frame->header.channels,
	                   frame->header.bits_per_sample, frame->header.sample_rate);
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_cb(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *meta,
                 void *client) {
	auto *self = static_cast<FlacDecoder *>(client);
	switch(meta->type) {
		case FLAC__METADATA_TYPE_STREAMINFO:
			self->accept_streaminfo(meta->data.stream_info.channels,
			                        meta->data.stream_info.sample_rate,
			                        meta->data.stream_info.bits_per_sample,
			                        meta->data.stream_info.total_samples);
			break;
		case FLAC__METADATA_TYPE_VORBIS_COMMENT: {
			const auto &vc = meta->data.vorbis_comment;
			for(uint32_t i = 0; i < vc.num_comments; ++i) {
				self->accept_vorbis_entry(
				    reinterpret_cast<const char *>(vc.comments[i].entry),
				    vc.comments[i].length);
			}
			break;
		}
		case FLAC__METADATA_TYPE_PICTURE:
			self->accept_picture(meta->data.picture.mime_type,
			                     meta->data.picture.data,
			                     meta->data.picture.data_length);
			break;
		default:
			break;
	}
}

void error_cb(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status,
              void *client) {
	// Lost-sync is recoverable; everything else aborts the decode.
	if(status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
		static_cast<FlacDecoder *>(client)->set_abort();
	}
}

} // namespace

FlacDecoder::FlacDecoder() = default;

FlacDecoder::~FlacDecoder() { close(); }

bool FlacDecoder::open(Source *source) {
	close();
	source_ = source;

	if(source_->seekable()) {
		source_->seek(0, SEEK_END);
		file_size_ = source_->tell();
		source_->seek(0, SEEK_SET);
	}

	dec_ = FLAC__stream_decoder_new();
	if(!dec_) return false;

	FLAC__StreamDecoder *d = cast(dec_);
	if(!source_->seekable()) FLAC__stream_decoder_set_md5_checking(d, 0);
	FLAC__stream_decoder_set_metadata_ignore_all(d);
	FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_STREAMINFO);
	FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT);
	FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_PICTURE);

	auto init = FLAC__stream_decoder_init_stream(
	    d, read_cb,
	    source_->seekable() ? seek_cb : nullptr,
	    source_->seekable() ? tell_cb : nullptr,
	    source_->seekable() ? length_cb : nullptr,
	    source_->seekable() ? eof_cb : nullptr,
	    write_cb, metadata_cb, error_cb, this);

	if(init != FLAC__STREAM_DECODER_INIT_STATUS_OK) return false;
	if(!FLAC__stream_decoder_process_until_end_of_metadata(d)) return false;
	if(!has_stream_info_) return false;

	props_.codec = "FLAC";
	return true;
}

void FlacDecoder::close() {
	if(dec_) {
		FLAC__StreamDecoder *d = cast(dec_);
		FLAC__stream_decoder_finish(d);
		FLAC__stream_decoder_delete(d);
		dec_ = nullptr;
	}
	source_ = nullptr;
	has_stream_info_ = false;
	end_of_stream_ = false;
	abort_ = false;
	block_.clear();
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	current_frame_ = 0;
	props_ = {};
	file_size_ = 0;
	bits_per_sample_ = 0;
	vorbis_tags_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
}

void FlacDecoder::accept_streaminfo(uint32_t channels, uint32_t sample_rate,
                                    uint32_t bits_per_sample, uint64_t total_samples) {
	if(has_stream_info_) return; // Observed in the wild: multiple STREAMINFO blocks.
	props_.format.channels = channels;
	props_.format.sample_rate = sample_rate;
	props_.total_frames = total_samples ? static_cast<int64_t>(total_samples) : -1;
	bits_per_sample_ = bits_per_sample;
	has_stream_info_ = true;
}

void FlacDecoder::accept_block(const int32_t *const *planes, uint32_t blocksize,
                               uint32_t channels, uint32_t bits_per_sample,
                               uint32_t sample_rate) {
	if(channels != props_.format.channels || sample_rate != props_.format.sample_rate) {
		props_.format.channels = channels;
		props_.format.sample_rate = sample_rate;
	}
	bits_per_sample_ = bits_per_sample;

	const float scale = bits_per_sample == 0 ? 1.0f
	    : 1.0f / static_cast<float>(int64_t(1) << (bits_per_sample - 1));

	block_.assign(static_cast<size_t>(blocksize) * channels, 0.0f);
	for(uint32_t i = 0; i < blocksize; ++i) {
		for(uint32_t c = 0; c < channels; ++c) {
			block_[i * channels + c] = static_cast<float>(planes[c][i]) * scale;
		}
	}
	block_frames_ = blocksize;
	block_frames_consumed_ = 0;
}

bool FlacDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!dec_) return false;
	FLAC__StreamDecoder *d = cast(dec_);

	if(block_frames_consumed_ >= block_frames_) {
		block_frames_ = 0;
		block_frames_consumed_ = 0;
		while(block_frames_ == 0) {
			auto state = FLAC__stream_decoder_get_state(d);
			if(state == FLAC__STREAM_DECODER_END_OF_STREAM) return false;
			if(abort_) return false;
			if(!FLAC__stream_decoder_process_single(d)) return false;
			if(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM
			   && block_frames_ == 0) {
				return false;
			}
		}
	}

	const size_t ch = props_.format.channels;
	const size_t avail = block_frames_ - block_frames_consumed_;
	const size_t take = std::min(avail, max_frames);

	const float *src = block_.data() + block_frames_consumed_ * ch;
	std::vector<float> samples(src, src + take * ch);
	block_frames_consumed_ += take;

	out = AudioChunk(props_.format, std::move(samples),
	                 static_cast<double>(current_frame_) / props_.format.sample_rate);
	current_frame_ += static_cast<int64_t>(take);
	return true;
}

int64_t FlacDecoder::seek(int64_t frame) {
	if(!dec_) return -1;
	FLAC__StreamDecoder *d = cast(dec_);
	if(!FLAC__stream_decoder_seek_absolute(d, static_cast<FLAC__uint64>(frame))) {
		FLAC__stream_decoder_flush(d);
		return -1;
	}
	current_frame_ = frame;
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	return frame;
}

// --- Metadata ---

namespace {

std::string lowercase(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return out;
}

// Cog's tag-name canonicalisations. Applied after lowercasing.
std::string canonicalise_tag(const std::string &lower_name) {
	if(lower_name == "lyrics" || lower_name == "unsynced lyrics") return "unsyncedlyrics";
	if(lower_name == "comments:itunnorm") return "soundcheck";
	return lower_name;
}

std::string base64_encode(const uint8_t *data, size_t len) {
	static const char tbl[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	size_t i = 0;
	for(; i + 2 < len; i += 3) {
		uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
		out.push_back(tbl[(v >> 18) & 0x3F]);
		out.push_back(tbl[(v >> 12) & 0x3F]);
		out.push_back(tbl[(v >> 6) & 0x3F]);
		out.push_back(tbl[v & 0x3F]);
	}
	if(i < len) {
		uint32_t v = uint32_t(data[i]) << 16;
		if(i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
		out.push_back(tbl[(v >> 18) & 0x3F]);
		out.push_back(tbl[(v >> 12) & 0x3F]);
		out.push_back(i + 1 < len ? tbl[(v >> 6) & 0x3F] : '=');
		out.push_back('=');
	}
	return out;
}

} // namespace

void FlacDecoder::accept_vorbis_entry(const char *entry, uint32_t length) {
	// Entries are "NAME=VALUE", UTF-8 per the Vorbis spec.
	const char *eq = static_cast<const char *>(std::memchr(entry, '=', length));
	if(!eq) return;

	std::string name(entry, static_cast<size_t>(eq - entry));
	std::string value(eq + 1, length - static_cast<size_t>(eq - entry) - 1);

	std::string key = lowercase(name);

	// Side-channel: channel-mask hex goes to DecoderProperties, not metadata.
	if(key == "waveformatextensible_channel_mask") {
		// Parse "0xHEX". Stashed for future use; not surfaced yet.
		return;
	}

	key = canonicalise_tag(key);

	auto it = vorbis_tags_.find(key);
	if(it == vorbis_tags_.end()) {
		vorbis_tags_[key] = nlohmann::json::array({std::move(value)});
	} else {
		it->push_back(std::move(value));
	}
}

void FlacDecoder::accept_picture(const char *mime, const uint8_t *data, size_t length) {
	// Only keep the first picture we see; FLAC can carry multiple but a
	// single album-cover cover is overwhelmingly the common case.
	if(!picture_bytes_.empty()) return;
	picture_mime_ = mime ? mime : "application/octet-stream";
	picture_bytes_.assign(data, data + length);
}

nlohmann::json FlacDecoder::metadata() const {
	nlohmann::json out = vorbis_tags_; // copy
	out["codec"] = "FLAC";
	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}
	return out;
}

} // namespace tuxedo
