#include "plugin/input/opus_decoder.hpp"

#include <opusfile.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tuxedo {

namespace {

OggOpusFile *cast(void *p) { return static_cast<OggOpusFile *>(p); }

// Shims mapping libopusfile's OpusFileCallbacks onto our Source API.
int read_cb(void *stream, unsigned char *ptr, int nbytes) {
	auto *src = static_cast<Source *>(stream);
	int64_t n = src->read(ptr, static_cast<size_t>(nbytes));
	if(n < 0) return -1;
	return static_cast<int>(n);
}

int seek_cb(void *stream, opus_int64 offset, int whence) {
	auto *src = static_cast<Source *>(stream);
	return src->seek(static_cast<int64_t>(offset), whence) ? 0 : -1;
}

opus_int64 tell_cb(void *stream) {
	auto *src = static_cast<Source *>(stream);
	return static_cast<opus_int64>(src->tell());
}

int close_cb(void *) {
	// Source lifetime is owned by the InputNode; do nothing here.
	return 0;
}

// Cog's channel-remap table, Vorbis → tuxedo/WAVEFORMATEXTENSIBLE order.
// Indexed as chmap[channels-1][out_channel] = in_channel.
const int kChmap[8][8] = {
    {0},                         // 1ch: mono
    {0, 1},                      // 2ch: L R
    {0, 2, 1},                   // 3ch: L C R
    {0, 1, 2, 3},                // 4ch: FL FR BL BR
    {0, 2, 1, 3, 4},             // 5ch: FL C FR BL BR
    {0, 2, 1, 4, 5, 3},          // 5.1: FL C FR BL BR LFE
    {0, 2, 1, 5, 6, 4, 3},       // 6.1
    {0, 2, 1, 6, 7, 4, 5, 3},    // 7.1
};

std::string lowercase(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return out;
}

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

OpusDecoder::OpusDecoder() = default;

OpusDecoder::~OpusDecoder() { close(); }

bool OpusDecoder::open(Source *source) {
	close();
	source_ = source;

	static const OpusFileCallbacks kCallbacks = {
	    read_cb, seek_cb, tell_cb, close_cb,
	};

	int err = 0;
	of_ = op_open_callbacks(source_, &kCallbacks, nullptr, 0, &err);
	if(!of_ || err != 0) {
		of_ = nullptr;
		source_ = nullptr;
		return false;
	}

	parse_head();
	parse_tags();
	props_.codec = "Opus";
	return props_.format.valid();
}

void OpusDecoder::close() {
	if(of_) {
		op_free(cast(of_));
		of_ = nullptr;
	}
	source_ = nullptr;
	props_ = {};
	block_.clear();
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	current_frame_ = 0;
	vorbis_tags_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
	r128_header_gain_q8_ = 0;
	r128_track_gain_q8_ = 0;
	r128_album_gain_q8_ = 0;
	has_track_gain_ = false;
	has_album_gain_ = false;
}

void OpusDecoder::parse_head() {
	OggOpusFile *of = cast(of_);
	const OpusHead *head = op_head(of, 0);
	if(!head) return;

	props_.format.sample_rate = 48000; // Opus canonical output rate
	props_.format.channels = static_cast<uint32_t>(head->channel_count);

	ogg_int64_t total = op_pcm_total(of, -1);
	props_.total_frames = total >= 0 ? static_cast<int64_t>(total) : -1;

	r128_header_gain_q8_ = head->output_gain;

	// Populate channel remap from the static table when we know the count.
	const uint32_t ch = props_.format.channels;
	if(ch >= 1 && ch <= 8) {
		for(uint32_t i = 0; i < ch; ++i) chmap_[i] = kChmap[ch - 1][i];
	}
}

void OpusDecoder::parse_tags() {
	OggOpusFile *of = cast(of_);
	const OpusTags *tags = op_tags(of, 0);
	if(!tags) return;

	for(int i = 0; i < tags->comments; ++i) {
		const char *entry = tags->user_comments[i];
		const int length = tags->comment_lengths[i];
		const char *eq = static_cast<const char *>(std::memchr(entry, '=', length));
		if(!eq) continue;

		const size_t name_len = static_cast<size_t>(eq - entry);
		const size_t value_len = static_cast<size_t>(length) - name_len - 1;
		accept_vorbis_entry(entry, name_len, eq + 1, value_len);
	}

	opus_int32 track_gain = 0;
	if(opus_tags_get_track_gain(tags, &track_gain) == 0) {
		r128_track_gain_q8_ = track_gain;
		has_track_gain_ = true;
	}
	opus_int32 album_gain = 0;
	if(opus_tags_get_album_gain(tags, &album_gain) == 0) {
		r128_album_gain_q8_ = album_gain;
		has_album_gain_ = true;
	}
}

void OpusDecoder::accept_vorbis_entry(const char *name, size_t name_len,
                                      const char *value, size_t value_len) {
	std::string key = lowercase(std::string(name, name_len));
	std::string val(value, value_len);

	if(key == "metadata_block_picture") {
		// Base64-encoded METADATA_BLOCK_PICTURE. Defer to libopusfile.
		OpusPictureTag pic;
		opus_picture_tag_init(&pic);
		if(opus_picture_tag_parse(&pic, val.c_str()) == 0) {
			const char *mime = pic.mime_type && *pic.mime_type
			                   ? pic.mime_type : "application/octet-stream";
			if(picture_bytes_.empty()) {
				picture_mime_ = mime;
				picture_bytes_.assign(pic.data, pic.data + pic.data_length);
			}
		}
		opus_picture_tag_clear(&pic);
		return;
	}
	if(key == "waveformatextensible_channel_mask") return; // side-channel only

	key = canonicalise_tag(key);
	auto it = vorbis_tags_.find(key);
	if(it == vorbis_tags_.end()) {
		vorbis_tags_[key] = nlohmann::json::array({std::move(val)});
	} else {
		it->push_back(std::move(val));
	}
}

bool OpusDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!of_ || max_frames == 0) return false;

	if(block_frames_consumed_ >= block_frames_) {
		// Refill. libopusfile returns as many frames as it has ready
		// (a few tens to low-hundreds typically); one call per read is
		// enough for our purposes.
		const uint32_t ch = props_.format.channels;
		if(ch == 0) return false;

		// Cog caps at 512 frames per op_read_float; matching keeps memory
		// and latency predictable.
		constexpr int kMaxRead = 512;
		std::vector<float> raw(static_cast<size_t>(kMaxRead) * ch);
		int got = op_read_float(cast(of_), raw.data(), kMaxRead * ch, nullptr);
		if(got <= 0) return false;

		// op_read_float yields interleaved samples in Vorbis channel order.
		// Re-pack through chmap_ into tuxedo-native order.
		const size_t frames = static_cast<size_t>(got);
		block_.assign(frames * ch, 0.0f);
		for(size_t f = 0; f < frames; ++f) {
			for(uint32_t c = 0; c < ch; ++c) {
				block_[f * ch + c] = raw[f * ch + chmap_[c]];
			}
		}
		block_frames_ = frames;
		block_frames_consumed_ = 0;
	}

	const uint32_t ch = props_.format.channels;
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

int64_t OpusDecoder::seek(int64_t frame) {
	if(!of_) return -1;
	if(op_pcm_seek(cast(of_), static_cast<ogg_int64_t>(frame)) != 0) return -1;
	current_frame_ = frame;
	block_frames_ = 0;
	block_frames_consumed_ = 0;
	return frame;
}

nlohmann::json OpusDecoder::metadata() const {
	nlohmann::json out = vorbis_tags_; // copy
	out["codec"] = "Opus";

	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}

	// R128 gains — expose raw q7.8 centibel values plus pre-formatted
	// dB strings under the standard replaygain_* keys, without Cog's
	// +5 dBFS offset (let the client decide).
	if(r128_header_gain_q8_) out["r128_output_gain_q8"] = r128_header_gain_q8_;
	if(has_track_gain_) {
		out["r128_track_gain_q8"] = r128_track_gain_q8_;
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.2f dB", r128_track_gain_q8_ / 256.0);
		out["replaygain_track_gain"] = nlohmann::json::array({std::string(buf)});
	}
	if(has_album_gain_) {
		out["r128_album_gain_q8"] = r128_album_gain_q8_;
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.2f dB", r128_album_gain_q8_ / 256.0);
		out["replaygain_album_gain"] = nlohmann::json::array({std::string(buf)});
	}

	return out;
}

} // namespace tuxedo
