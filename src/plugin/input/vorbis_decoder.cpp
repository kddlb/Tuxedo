#include "plugin/input/vorbis_decoder.hpp"

#include "plugin/input/vorbis_common.hpp"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tuxedo {

namespace {

OggVorbis_File *cast(void *p) { return static_cast<OggVorbis_File *>(p); }

// libvorbisfile's ov_callbacks mirror stdio (fread/fseek/fclose/ftell).
// Shim them onto our Source interface.
size_t read_cb(void *ptr, size_t size, size_t nmemb, void *datasource) {
	auto *src = static_cast<Source *>(datasource);
	int64_t n = src->read(ptr, size * nmemb);
	if(n < 0) return 0;
	return static_cast<size_t>(n) / (size ? size : 1);
}

int seek_cb(void *datasource, ogg_int64_t offset, int whence) {
	auto *src = static_cast<Source *>(datasource);
	if(!src->seekable()) return -1;
	return src->seek(static_cast<int64_t>(offset), whence) ? 0 : -1;
}

int close_cb(void *) { return 0; } // Source is owned by InputNode.

long tell_cb(void *datasource) {
	auto *src = static_cast<Source *>(datasource);
	return static_cast<long>(src->tell());
}

// Same channel remap table as OpusDecoder (Vorbis → WAVEFORMATEXTENSIBLE).
const int kChmap[8][8] = {
    {0},
    {0, 1},
    {0, 2, 1},
    {0, 1, 2, 3},
    {0, 2, 1, 3, 4},
    {0, 2, 1, 4, 5, 3},
    {0, 2, 1, 5, 6, 4, 3},
    {0, 2, 1, 6, 7, 4, 5, 3},
};

} // namespace

VorbisDecoder::VorbisDecoder() = default;

VorbisDecoder::~VorbisDecoder() { close(); }

bool VorbisDecoder::open(Source *source) {
	close();
	source_ = source;

	static const ov_callbacks kCallbacks = {
	    read_cb, seek_cb, close_cb, tell_cb,
	};

	vf_ = std::malloc(sizeof(OggVorbis_File));
	if(!vf_) {
		source_ = nullptr;
		return false;
	}

	int r = ov_open_callbacks(source_, cast(vf_), nullptr, 0, kCallbacks);
	if(r != 0) {
		std::free(vf_);
		vf_ = nullptr;
		source_ = nullptr;
		return false;
	}
	inited_ = true;

	parse_info();
	parse_tags();
	props_.codec = "Vorbis";
	return props_.format.valid();
}

void VorbisDecoder::close() {
	if(inited_) {
		ov_clear(cast(vf_));
		inited_ = false;
	}
	if(vf_) {
		std::free(vf_);
		vf_ = nullptr;
	}
	source_ = nullptr;
	props_ = {};
	current_frame_ = 0;
	vorbis_tags_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
}

void VorbisDecoder::parse_info() {
	vorbis_info *info = ov_info(cast(vf_), -1);
	if(!info) return;
	props_.format.sample_rate = static_cast<uint32_t>(info->rate);
	props_.format.channels = static_cast<uint32_t>(info->channels);

	ogg_int64_t total = ov_pcm_total(cast(vf_), -1);
	props_.total_frames = total >= 0 ? static_cast<int64_t>(total) : -1;
}

void VorbisDecoder::parse_tags() {
	vorbis_comment *vc = ov_comment(cast(vf_), -1);
	if(!vc) return;

	for(int i = 0; i < vc->comments; ++i) {
		const char *entry = vc->user_comments[i];
		const int length = vc->comment_lengths[i];
		if(length <= 0 || !entry) continue;

		const char *eq = static_cast<const char *>(
		    std::memchr(entry, '=', static_cast<size_t>(length)));
		if(!eq) continue;

		const size_t name_len = static_cast<size_t>(eq - entry);
		const size_t value_len = static_cast<size_t>(length) - name_len - 1;

		// Pre-filter METADATA_BLOCK_PICTURE before dispatching to the
		// shared tag accumulator (which drops the side-channel keys).
		std::string key = vorbis_common::lowercase(std::string(entry, name_len));
		if(key == "metadata_block_picture") {
			auto raw = vorbis_common::base64_decode(eq + 1, value_len);
			if(!raw.empty() && picture_bytes_.empty()) {
				std::string mime;
				std::vector<uint8_t> bytes;
				if(vorbis_common::unpack_flac_picture(raw.data(), raw.size(), mime, bytes)) {
					picture_mime_ = mime.empty() ? "application/octet-stream" : mime;
					picture_bytes_ = std::move(bytes);
				}
			}
			continue;
		}

		vorbis_common::accept_tag(vorbis_tags_, entry, name_len, eq + 1, value_len);
	}
}

bool VorbisDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!inited_ || max_frames == 0) return false;
	const uint32_t ch = props_.format.channels;
	if(ch == 0) return false;

	// ov_read_float hands back planar float samples and decides how
	// many to return per call (typically ~1024). Use a small cap that
	// matches the Opus decoder's budget.
	constexpr int kMaxRead = 4096;
	const int want = static_cast<int>(std::min<size_t>(max_frames, kMaxRead));

	float **planar = nullptr;
	int bitstream = 0;
	long got = ov_read_float(cast(vf_), &planar, want, &bitstream);
	if(got <= 0) return false;

	const size_t frames = static_cast<size_t>(got);

	// Interleave into the tuxedo channel order via the remap table.
	std::vector<float> samples(frames * ch);
	int remap[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	if(ch >= 1 && ch <= 8) {
		for(uint32_t c = 0; c < ch; ++c) remap[c] = kChmap[ch - 1][c];
	}
	for(size_t f = 0; f < frames; ++f) {
		for(uint32_t c = 0; c < ch; ++c) {
			samples[f * ch + c] = planar[remap[c]][f];
		}
	}

	out = AudioChunk(props_.format, std::move(samples),
	                 static_cast<double>(current_frame_) / props_.format.sample_rate);
	current_frame_ += static_cast<int64_t>(frames);
	return true;
}

int64_t VorbisDecoder::seek(int64_t frame) {
	if(!inited_) return -1;
	if(ov_pcm_seek(cast(vf_), static_cast<ogg_int64_t>(frame)) != 0) return -1;
	current_frame_ = frame;
	return frame;
}

nlohmann::json VorbisDecoder::metadata() const {
	nlohmann::json out = vorbis_tags_;
	out["codec"] = "Vorbis";
	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", vorbis_common::base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}
	return out;
}

} // namespace tuxedo
