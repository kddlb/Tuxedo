#include "plugin/input/mp3_decoder.hpp"

#include "plugin/input/vorbis_common.hpp"

// Match the build flags used by vendor/minimp3/minimp3_impl.c — without
// these, mp3d_sample_t is int16_t here but float in the implementation
// TU, so the mp3dec_ex_t::buffer member is half the size we expect and
// minimp3's internal memset clobbers fields past the end of `ex`.
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

#include <id3tag.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace tuxedo {

struct Mp3Decoder::Impl {
	// Seekable path: full minimp3 streaming-with-index decoder.
	mp3dec_ex_t ex{};
	bool ex_inited = false;

	// Streaming path: bare frame-by-frame decoder + our own input ring.
	mp3dec_t dec{};
	std::vector<uint8_t> input_buf;
	size_t input_filled = 0;
	bool input_eof = false;

	// Common: first frame's samples are pre-decoded by open() so the
	// caller sees the correct format on properties() before pulling audio.
	std::vector<float> pending_samples; // interleaved float32
	size_t pending_frames = 0;

	mp3dec_frame_info_t last_info{};
	int64_t frames_decoded = 0;

	mp3dec_io_t io{};
	Source *src = nullptr;
};

namespace {

// Source-backed minimp3 IO callbacks.
size_t mp3_read_cb(void *buf, size_t size, void *user_data) {
	auto *src = static_cast<Source *>(user_data);
	int64_t n = src->read(buf, size);
	return n < 0 ? 0 : static_cast<size_t>(n);
}

int mp3_seek_cb(uint64_t position, void *user_data) {
	auto *src = static_cast<Source *>(user_data);
	return src->seek(static_cast<int64_t>(position), SEEK_SET) ? 0 : -1;
}

std::string ucs4_to_utf8(const id3_ucs4_t *src) {
	if(!src) return {};
	id3_utf8_t *raw = id3_ucs4_utf8duplicate(src);
	if(!raw) return {};
	std::string out(reinterpret_cast<const char *>(raw));
	std::free(raw);
	return out;
}

std::string lower_ascii(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return s;
}

void push_tag(nlohmann::json &tags, const std::string &key, std::string value) {
	if(value.empty()) return;
	std::string k = vorbis_common::canonicalise_tag(lower_ascii(key));
	auto it = tags.find(k);
	if(it == tags.end()) {
		tags[k] = nlohmann::json::array({std::move(value)});
	} else {
		it->push_back(std::move(value));
	}
}

void handle_text_frame(const id3_frame *frame, nlohmann::json &tags) {
	struct Map { const char *id; const char *key; };
	static const Map kMap[] = {
	    {"TIT2", "title"},       {"TT2", "title"},
	    {"TPE1", "artist"},      {"TP1", "artist"},
	    {"TPE2", "albumartist"}, {"TP2", "albumartist"},
	    {"TALB", "album"},       {"TAL", "album"},
	    {"TCON", "genre"},       {"TCO", "genre"},
	    {"TCOM", "composer"},    {"TCM", "composer"},
	    {"TPUB", "label"},       {"TPB", "label"},
	    {"TRCK", "tracknumber"}, {"TRK", "tracknumber"},
	    {"TPOS", "discnumber"},  {"TPA", "discnumber"},
	    {"TDRC", "date"},        {"TYER", "date"}, {"TYE", "date"},
	    {"TSSE", "encoder"},     {"TSS", "encoder"},
	    {"TENC", "encodedby"},   {"TEN", "encodedby"},
	    {"TCOP", "copyright"},
	    {"TBPM", "bpm"},
	    {"TSOP", "artistsort"},
	    {"TSOA", "albumsort"},
	    {"TSOT", "titlesort"},
	};
	const char *canonical = nullptr;
	for(const auto &m : kMap) {
		if(std::strcmp(frame->id, m.id) == 0) { canonical = m.key; break; }
	}
	if(!canonical) return;

	if(frame->nfields < 2) return;
	const id3_field *f = id3_frame_field(const_cast<id3_frame *>(frame), 1);
	if(!f) return;
	unsigned int n = id3_field_getnstrings(f);
	for(unsigned int i = 0; i < n; ++i) {
		const id3_ucs4_t *ucs = id3_field_getstrings(f, i);
		std::string s = ucs4_to_utf8(ucs);
		if(s.empty()) continue;
		push_tag(tags, canonical, s);

		// TRCK / TPOS can be "N/M" — split the totals out.
		if((std::strcmp(frame->id, "TRCK") == 0 ||
		    std::strcmp(frame->id, "TRK")  == 0 ||
		    std::strcmp(frame->id, "TPOS") == 0 ||
		    std::strcmp(frame->id, "TPA")  == 0)) {
			auto slash = s.find('/');
			if(slash != std::string::npos && slash + 1 < s.size()) {
				std::string total = s.substr(slash + 1);
				const char *total_key =
				    (frame->id[1] == 'R') ? "tracktotal" : "totaldiscs";
				push_tag(tags, total_key, std::move(total));
			}
		}
	}
}

void handle_txxx(const id3_frame *frame, nlohmann::json &tags) {
	if(frame->nfields < 3) return;
	const id3_field *desc_f = id3_frame_field(const_cast<id3_frame *>(frame), 1);
	const id3_field *val_f  = id3_frame_field(const_cast<id3_frame *>(frame), 2);
	if(!desc_f || !val_f) return;
	std::string desc = ucs4_to_utf8(id3_field_getstring(desc_f));
	if(desc.empty()) return;
	unsigned int n = id3_field_getnstrings(val_f);
	for(unsigned int i = 0; i < n; ++i) {
		std::string v = ucs4_to_utf8(id3_field_getstrings(val_f, i));
		if(v.empty()) continue;
		push_tag(tags, desc, v);
	}
}

void handle_comm(const id3_frame *frame, nlohmann::json &tags) {
	if(frame->nfields < 4) return;
	const id3_field *full = id3_frame_field(const_cast<id3_frame *>(frame), 3);
	if(!full) return;
	std::string s = ucs4_to_utf8(id3_field_getfullstring(full));
	push_tag(tags, "comment", s);
}

void handle_uslt(const id3_frame *frame, nlohmann::json &tags) {
	if(frame->nfields < 4) return;
	const id3_field *full = id3_frame_field(const_cast<id3_frame *>(frame), 3);
	if(!full) return;
	std::string s = ucs4_to_utf8(id3_field_getfullstring(full));
	push_tag(tags, "unsyncedlyrics", s);
}

void handle_apic(const id3_frame *frame,
                 std::string &picture_mime, std::vector<uint8_t> &picture_bytes) {
	if(!picture_bytes.empty()) return;
	if(frame->nfields < 5) return;
	const id3_field *mime_f = id3_frame_field(const_cast<id3_frame *>(frame), 1);
	const id3_field *data_f = id3_frame_field(const_cast<id3_frame *>(frame), 4);
	if(!mime_f || !data_f) return;

	const id3_latin1_t *mime = id3_field_getlatin1(mime_f);
	picture_mime = mime ? reinterpret_cast<const char *>(mime) : "application/octet-stream";

	id3_length_t len = 0;
	const id3_byte_t *data = id3_field_getbinarydata(data_f, &len);
	if(!data || len == 0) { picture_mime.clear(); return; }
	picture_bytes.assign(data, data + len);
}

bool try_parse_itun_smpb(const id3_frame *frame, int64_t file_size,
                         uint32_t &start_padding, uint32_t &end_padding,
                         int64_t &total_frames) {
	if(std::strcmp(frame->id, "COMM") != 0 && std::strcmp(frame->id, "COM") != 0)
		return false;
	if(frame->nfields < 4) return false;

	const id3_field *desc_f = id3_frame_field(const_cast<id3_frame *>(frame), 2);
	const id3_field *val_f  = id3_frame_field(const_cast<id3_frame *>(frame), 3);
	if(!desc_f || !val_f) return false;

	std::string desc = ucs4_to_utf8(id3_field_getstring(desc_f));
	if(desc != "iTunSMPB") return false;

	std::string value = ucs4_to_utf8(id3_field_getfullstring(val_f));

	uint32_t zero, start_pad, end_pad, zero2;
	uint64_t temp_duration_u, last_eight_frames_offset;
	if(std::sscanf(value.c_str(),
	               "%" SCNx32 " %" SCNx32 " %" SCNx32 " %" SCNx64 " %" SCNx32 " %" SCNx64,
	               &zero, &start_pad, &end_pad, &temp_duration_u,
	               &zero2, &last_eight_frames_offset) != 6) {
		return false;
	}
	int64_t temp_duration = static_cast<int64_t>(temp_duration_u);
	if(temp_duration < 0) return false;
	if(start_pad > 576u * 2u * 32u) return false;
	if(end_pad > 576u * 2u * 64u) return false;
	if(file_size > 0 &&
	   static_cast<int64_t>(last_eight_frames_offset) >= file_size) {
		return false;
	}
	if(end_pad < 528u + 1u) return false;

	start_padding = start_pad + 528u + 1u;
	end_padding = end_pad - (528u + 1u);
	total_frames = temp_duration;
	return true;
}

const char *layer_codec_name(int layer) {
	switch(layer) {
		case 1: return "MP1";
		case 2: return "MP2";
		case 3: return "MP3";
		default: return "MP3";
	}
}

// Common loop: walk parsed id3_tag, collect canonical fields plus
// iTunSMPB, APIC art, etc. file_size is used to validate iTunSMPB; pass 0
// when unknown (unseekable streams).
void collect_id3_frames(id3_tag *tag, int64_t file_size,
                        nlohmann::json &id3_tags,
                        std::string &picture_mime,
                        std::vector<uint8_t> &picture_bytes,
                        uint32_t &start_padding, uint32_t &end_padding,
                        int64_t &total_frames_explicit, bool &found_itun_smpb) {
	for(unsigned int i = 0; i < tag->nframes; ++i) {
		const id3_frame *frame = tag->frames[i];
		if(!frame) continue;

		uint32_t sp = 0, ep = 0;
		int64_t td = 0;
		if(!found_itun_smpb &&
		   try_parse_itun_smpb(frame, file_size, sp, ep, td)) {
			start_padding = sp;
			end_padding = ep;
			total_frames_explicit = td;
			found_itun_smpb = true;
			continue;
		}

		if(frame->id[0] == 'T' && std::strcmp(frame->id, "TXXX") != 0 &&
		   std::strcmp(frame->id, "TXX") != 0) {
			handle_text_frame(frame, id3_tags);
		} else if(std::strcmp(frame->id, "TXXX") == 0 ||
		          std::strcmp(frame->id, "TXX")  == 0) {
			handle_txxx(frame, id3_tags);
		} else if(std::strcmp(frame->id, "COMM") == 0 ||
		          std::strcmp(frame->id, "COM")  == 0) {
			handle_comm(frame, id3_tags);
		} else if(std::strcmp(frame->id, "USLT") == 0 ||
		          std::strcmp(frame->id, "ULT")  == 0) {
			handle_uslt(frame, id3_tags);
		} else if(std::strcmp(frame->id, "APIC") == 0 ||
		          std::strcmp(frame->id, "PIC")  == 0) {
			handle_apic(frame, picture_mime, picture_bytes);
		}
	}
}

// Read the full 128-byte ID3v1 trailer from a seekable source (last 128
// bytes), parse via libid3tag, and merge.
void read_id3v1_from_source(Source *src, int64_t file_size,
                            nlohmann::json &id3_tags) {
	if(file_size < 128) return;
	if(!src->seek(file_size - 128, SEEK_SET)) return;
	uint8_t buf[128];
	int64_t got = 0;
	while(got < 128) {
		int64_t n = src->read(buf + got, 128 - got);
		if(n <= 0) return;
		got += n;
	}
	if(buf[0] != 'T' || buf[1] != 'A' || buf[2] != 'G') return;
	id3_tag *tag = id3_tag_parse(buf, 128);
	if(!tag) return;

	// Reuse the v2 frame collector — only the canonical text frames it
	// emits are useful here, but it's the same surface we expose.
	std::string mime;
	std::vector<uint8_t> bytes;
	uint32_t sp = 0, ep = 0;
	int64_t td = 0;
	bool found = false;
	collect_id3_frames(tag, file_size, id3_tags, mime, bytes, sp, ep, td, found);
	id3_tag_delete(tag);
}

} // namespace

Mp3Decoder::Mp3Decoder() : impl_(new Impl) {}

Mp3Decoder::~Mp3Decoder() {
	close();
	delete impl_;
}

bool Mp3Decoder::open_seekable() {
	Source *src = impl_->src;

	// Discover total file size up front (used to sanity-check iTunSMPB
	// offsets and to bound the ID3v1 read).
	int64_t file_size = -1;
	if(src->seek(0, SEEK_END)) {
		file_size = src->tell();
	}
	if(!src->seek(0, SEEK_SET)) return false;

	// ID3v2 header (10 bytes), if present, drives the buffer-based tag
	// read. Same approach as Cog's MP3Decoder so the decoder can pull
	// tags through any source — local file, HTTP, archive entry, etc.
	uint8_t hdr[10];
	int64_t got = src->read(hdr, 10);
	if(got == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
		size_t body_len = (static_cast<size_t>(hdr[6] & 0x7f) << 21) |
		                  (static_cast<size_t>(hdr[7] & 0x7f) << 14) |
		                  (static_cast<size_t>(hdr[8] & 0x7f) <<  7) |
		                   static_cast<size_t>(hdr[9] & 0x7f);
		std::vector<uint8_t> tag_buf(10 + body_len);
		std::memcpy(tag_buf.data(), hdr, 10);
		size_t off = 10;
		while(off < tag_buf.size()) {
			int64_t n = src->read(tag_buf.data() + off, tag_buf.size() - off);
			if(n <= 0) return false;
			off += static_cast<size_t>(n);
		}
		if(id3_tag *tag = id3_tag_parse(tag_buf.data(), tag_buf.size())) {
			collect_id3_frames(tag, file_size, id3_tags_, picture_mime_,
			                   picture_bytes_, start_padding_, end_padding_,
			                   total_frames_explicit_, found_itun_smpb_);
			id3_tag_delete(tag);
		}
	}

	if(file_size > 0) {
		read_id3v1_from_source(src, file_size, id3_tags_);
	}

	// Reset to byte 0 — minimp3 will do its own ID3v2 skip during the scan.
	if(!src->seek(0, SEEK_SET)) return false;

	impl_->io.read = mp3_read_cb;
	impl_->io.read_data = src;
	impl_->io.seek = mp3_seek_cb;
	impl_->io.seek_data = src;

	int err = mp3dec_ex_open_cb(&impl_->ex, &impl_->io, MP3D_SEEK_TO_SAMPLE);
	if(err) return false;
	impl_->ex_inited = true;

	if(found_itun_smpb_) {
		uint32_t ch = impl_->ex.info.channels;
		impl_->ex.start_delay = static_cast<int>(start_padding_) * ch;
		impl_->ex.to_skip = static_cast<int>(start_padding_) * ch;
		impl_->ex.detected_samples = static_cast<uint64_t>(total_frames_explicit_) * ch;
		impl_->ex.samples = (static_cast<uint64_t>(total_frames_explicit_) +
		                     start_padding_ + end_padding_) * ch;
	}

	// Pre-decode one frame so callers see correct format from properties().
	mp3d_sample_t *sample_ptr = nullptr;
	size_t samples = 0;
	for(int retry = 0; retry < 10 && samples == 0; ++retry) {
		samples = mp3dec_ex_read_frame(&impl_->ex, &sample_ptr,
		                               &impl_->last_info,
		                               MINIMP3_MAX_SAMPLES_PER_FRAME);
	}
	if(samples == 0 || sample_ptr == nullptr) return false;

	impl_->pending_samples.assign(sample_ptr, sample_ptr + samples);
	impl_->pending_frames = samples / impl_->last_info.channels;

	props_.format.sample_rate = impl_->last_info.hz;
	props_.format.channels = impl_->last_info.channels;
	props_.codec = layer_codec_name(impl_->last_info.layer);

	if(found_itun_smpb_) {
		props_.total_frames = total_frames_explicit_;
	} else {
		uint64_t total_samples = impl_->ex.detected_samples;
		if(total_samples == 0) total_samples = impl_->ex.samples;
		props_.total_frames = total_samples
		    ? static_cast<int64_t>(total_samples / impl_->last_info.channels)
		    : -1;
	}

	return true;
}

bool Mp3Decoder::refill_input() {
	if(impl_->input_eof) return impl_->input_filled > 0;
	const size_t want = impl_->input_buf.size() - impl_->input_filled;
	if(want == 0) return true;
	int64_t n = impl_->src->read(impl_->input_buf.data() + impl_->input_filled, want);
	if(n <= 0) {
		impl_->input_eof = true;
		return impl_->input_filled > 0;
	}
	impl_->input_filled += static_cast<size_t>(n);
	return true;
}

bool Mp3Decoder::open_streaming() {
	impl_->input_buf.assign(MINIMP3_BUF_SIZE, 0);
	impl_->input_filled = 0;
	impl_->input_eof = false;

	// Pull the first 10 bytes through the streaming buffer — they're
	// either an ID3v2 header (which we drain into a tag buffer and then
	// discard from the stream, leaving us at the MP3 frame boundary) or
	// the start of the first MP3 frame.
	int64_t got = 0;
	while(got < 10) {
		int64_t n = impl_->src->read(impl_->input_buf.data() + got, 10 - got);
		if(n <= 0) {
			impl_->input_eof = true;
			break;
		}
		got += n;
	}
	if(got < 10) return false;
	impl_->input_filled = 10;

	if(impl_->input_buf[0] == 'I' && impl_->input_buf[1] == 'D' &&
	   impl_->input_buf[2] == '3') {
		size_t body_len = (static_cast<size_t>(impl_->input_buf[6] & 0x7f) << 21) |
		                  (static_cast<size_t>(impl_->input_buf[7] & 0x7f) << 14) |
		                  (static_cast<size_t>(impl_->input_buf[8] & 0x7f) <<  7) |
		                   static_cast<size_t>(impl_->input_buf[9] & 0x7f);
		std::vector<uint8_t> tag_buf(10 + body_len);
		std::memcpy(tag_buf.data(), impl_->input_buf.data(), 10);
		size_t off = 10;
		while(off < tag_buf.size()) {
			int64_t n = impl_->src->read(tag_buf.data() + off,
			                             tag_buf.size() - off);
			if(n <= 0) return false;
			off += static_cast<size_t>(n);
		}
		if(id3_tag *tag = id3_tag_parse(tag_buf.data(), tag_buf.size())) {
			collect_id3_frames(tag, /*file_size=*/0, id3_tags_, picture_mime_,
			                   picture_bytes_, start_padding_, end_padding_,
			                   total_frames_explicit_, found_itun_smpb_);
			id3_tag_delete(tag);
		}
		// Tag bytes are gone from the source; the next refill brings in
		// pure MP3 frame data.
		impl_->input_filled = 0;
	}

	mp3dec_init(&impl_->dec);

	refill_input();
	if(impl_->input_filled == 0) return false;

	while(true) {
		mp3dec_frame_info_t info{};
		mp3d_sample_t out[MINIMP3_MAX_SAMPLES_PER_FRAME];
		int samples = mp3dec_decode_frame(&impl_->dec, impl_->input_buf.data(),
		                                  static_cast<int>(impl_->input_filled),
		                                  out, &info);
		if(info.frame_bytes == 0) {
			if(impl_->input_eof) return false;
			if(!refill_input()) return false;
			continue;
		}
		if(static_cast<size_t>(info.frame_bytes) > impl_->input_filled) {
			if(impl_->input_eof) return false;
			if(!refill_input()) return false;
			continue;
		}
		impl_->input_filled -= info.frame_bytes;
		std::memmove(impl_->input_buf.data(),
		             impl_->input_buf.data() + info.frame_bytes,
		             impl_->input_filled);

		if(samples > 0) {
			impl_->last_info = info;
			impl_->pending_samples.assign(out, out + samples * info.channels);
			impl_->pending_frames = static_cast<size_t>(samples);

			props_.format.sample_rate = info.hz;
			props_.format.channels = info.channels;
			props_.codec = layer_codec_name(info.layer);
			props_.total_frames = -1;
			return true;
		}
		// samples == 0 with frame_bytes > 0: junk skipped (e.g. tag prefix).
		if(impl_->input_filled < MINIMP3_BUF_SIZE && !impl_->input_eof) {
			refill_input();
		}
	}
}

bool Mp3Decoder::open(Source *source) {
	close();
	impl_->src = source;
	seekable_ = source->seekable();

	bool ok = seekable_ ? open_seekable() : open_streaming();
	if(!ok) {
		close();
		return false;
	}
	return props_.format.valid();
}

void Mp3Decoder::close() {
	if(impl_) {
		if(impl_->ex_inited) {
			mp3dec_ex_close(&impl_->ex);
			impl_->ex_inited = false;
		}
		impl_->src = nullptr;
		impl_->input_buf.clear();
		impl_->input_filled = 0;
		impl_->input_eof = false;
		impl_->pending_samples.clear();
		impl_->pending_frames = 0;
		impl_->frames_decoded = 0;
		impl_->last_info = {};
	}
	props_ = {};
	seekable_ = false;
	total_frames_explicit_ = 0;
	start_padding_ = 0;
	end_padding_ = 0;
	found_itun_smpb_ = false;
	id3_tags_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
}

bool Mp3Decoder::decode_streaming_frame() {
	while(true) {
		if(impl_->input_filled < impl_->input_buf.size() && !impl_->input_eof) {
			refill_input();
		}
		if(impl_->input_filled == 0) return false;

		mp3dec_frame_info_t info{};
		mp3d_sample_t out[MINIMP3_MAX_SAMPLES_PER_FRAME];
		int samples = mp3dec_decode_frame(&impl_->dec, impl_->input_buf.data(),
		                                  static_cast<int>(impl_->input_filled),
		                                  out, &info);

		if(info.frame_bytes == 0) {
			if(impl_->input_eof) return false;
			if(!refill_input()) return false;
			continue;
		}
		if(static_cast<size_t>(info.frame_bytes) > impl_->input_filled) {
			if(impl_->input_eof) return false;
			if(!refill_input()) return false;
			continue;
		}

		impl_->input_filled -= info.frame_bytes;
		std::memmove(impl_->input_buf.data(),
		             impl_->input_buf.data() + info.frame_bytes,
		             impl_->input_filled);

		if(samples > 0) {
			impl_->last_info = info;
			impl_->pending_samples.assign(out, out + samples * info.channels);
			impl_->pending_frames = static_cast<size_t>(samples);
			return true;
		}
	}
}

bool Mp3Decoder::read(AudioChunk &out, size_t max_frames) {
	if(!impl_ || max_frames == 0) return false;
	if(seekable_ && !impl_->ex_inited) return false;
	if(!seekable_ && impl_->input_buf.empty()) return false;

	if(impl_->pending_frames == 0) {
		if(seekable_) {
			mp3d_sample_t *sample_ptr = nullptr;
			size_t samples = mp3dec_ex_read_frame(&impl_->ex, &sample_ptr,
			                                     &impl_->last_info,
			                                     MINIMP3_MAX_SAMPLES_PER_FRAME);
			if(samples == 0 || sample_ptr == nullptr) return false;
			impl_->pending_samples.assign(sample_ptr, sample_ptr + samples);
			impl_->pending_frames = samples / impl_->last_info.channels;
		} else {
			if(!decode_streaming_frame()) return false;
		}
	}

	const uint32_t ch = props_.format.channels;
	size_t frames = std::min<size_t>(impl_->pending_frames, max_frames);
	size_t samples = frames * ch;

	std::vector<float> buf(impl_->pending_samples.begin(),
	                       impl_->pending_samples.begin() + samples);

	if(samples == impl_->pending_samples.size()) {
		impl_->pending_samples.clear();
		impl_->pending_frames = 0;
	} else {
		impl_->pending_samples.erase(impl_->pending_samples.begin(),
		                             impl_->pending_samples.begin() + samples);
		impl_->pending_frames -= frames;
	}

	impl_->frames_decoded += static_cast<int64_t>(frames);
	out = AudioChunk(props_.format, std::move(buf));
	return true;
}

int64_t Mp3Decoder::seek(int64_t frame) {
	if(!impl_ || !impl_->ex_inited || !seekable_) return -1;
	if(frame < 0) frame = 0;

	uint32_t ch = props_.format.channels;
	if(mp3dec_ex_seek(&impl_->ex, static_cast<uint64_t>(frame) * ch) < 0) {
		return -1;
	}
	impl_->pending_samples.clear();
	impl_->pending_frames = 0;
	impl_->frames_decoded = frame;
	return frame;
}

nlohmann::json Mp3Decoder::metadata() const {
	nlohmann::json out = id3_tags_;
	if(impl_ && impl_->src) {
		nlohmann::json source_metadata = impl_->src->metadata();
		for(auto it = source_metadata.begin(); it != source_metadata.end(); ++it) {
			out[it.key()] = it.value();
		}
	}
	out["codec"] = props_.codec.empty() ? "MP3" : props_.codec;
	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", vorbis_common::base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}
	return out;
}

void Mp3Decoder::set_metadata_changed_callback(MetadataChangedCallback cb) {
	metadata_changed_cb_ = std::move(cb);
}

} // namespace tuxedo
