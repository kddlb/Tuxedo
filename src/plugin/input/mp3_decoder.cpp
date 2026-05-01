#include "plugin/input/mp3_decoder.hpp"

#include "plugin/input/vorbis_common.hpp"

#include "miniaudio.h"

#include <id3tag.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace tuxedo {

struct Mp3Decoder::Impl {
	ma_decoder dec{};
	bool inited = false;
	Source *src = nullptr;
};

namespace {

ma_result read_cb(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead,
                  size_t *pBytesRead) {
	auto *src = static_cast<Source *>(pDecoder->pUserData);
	int64_t n = src->read(pBufferOut, bytesToRead);
	if(n < 0) {
		*pBytesRead = 0;
		return MA_ERROR;
	}
	*pBytesRead = static_cast<size_t>(n);
	return n == 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result seek_cb(ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin) {
	auto *src = static_cast<Source *>(pDecoder->pUserData);
	int whence = SEEK_SET;
	switch(origin) {
		case ma_seek_origin_start:   whence = SEEK_SET; break;
		case ma_seek_origin_current: whence = SEEK_CUR; break;
		case ma_seek_origin_end:     whence = SEEK_END; break;
	}
	return src->seek(static_cast<int64_t>(byteOffset), whence) ? MA_SUCCESS : MA_ERROR;
}

// libid3tag exposes frame text as UCS-4. Convert to UTF-8 via the
// library's own duplicator (malloc'd; caller frees).
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

// Append a value to tags[key], lowercasing key and applying Cog's
// rename rules (so ID3-derived tags interleave cleanly with
// Vorbis-derived ones).
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

// Handle an ID3v2 text frame (T***): drain its string list and map
// the frame-id into the canonical tuxedo tag key.
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

	// Text frames: field[0] = encoding, field[1] = string list.
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

// TXXX user-defined text: field[1] = description, field[2] = string
// list. Used for REPLAYGAIN_* and similar.
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

// COMM comment: field[1] = language, field[2] = description, field[3]
// = fullstring (the comment text).
void handle_comm(const id3_frame *frame, nlohmann::json &tags) {
	if(frame->nfields < 4) return;
	const id3_field *full = id3_frame_field(const_cast<id3_frame *>(frame), 3);
	if(!full) return;
	std::string s = ucs4_to_utf8(id3_field_getfullstring(full));
	push_tag(tags, "comment", s);
}

// USLT unsynchronised lyrics: field[1] = language, field[2] =
// description, field[3] = fullstring.
void handle_uslt(const id3_frame *frame, nlohmann::json &tags) {
	if(frame->nfields < 4) return;
	const id3_field *full = id3_frame_field(const_cast<id3_frame *>(frame), 3);
	if(!full) return;
	std::string s = ucs4_to_utf8(id3_field_getfullstring(full));
	push_tag(tags, "unsyncedlyrics", s);
}

// APIC embedded picture: field[1] = MIME (latin1), field[2] = type
// (byte), field[3] = description, field[4] = binary data.
void handle_apic(const id3_frame *frame,
                 std::string &picture_mime, std::vector<uint8_t> &picture_bytes) {
	if(!picture_bytes.empty()) return; // keep the first
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

// Strip a "file://" prefix if present (libid3tag wants a filesystem path).
std::string path_from_url(const std::string &url) {
	static const std::string pfx = "file://";
	if(url.compare(0, pfx.size(), pfx) == 0) return url.substr(pfx.size());
	return url;
}

} // namespace

Mp3Decoder::Mp3Decoder() : impl_(new Impl) {}

Mp3Decoder::~Mp3Decoder() {
	close();
	delete impl_;
}

bool Mp3Decoder::open(Source *source) {
	close();
	impl_->src = source;

	// Audio path: ma_decoder with MP3 hint.
	ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
	cfg.encodingFormat = ma_encoding_format_mp3;

	ma_result r = ma_decoder_init(read_cb, seek_cb, source, &cfg, &impl_->dec);
	if(r != MA_SUCCESS) {
		impl_->src = nullptr;
		return false;
	}
	impl_->inited = true;

	props_.format.sample_rate = impl_->dec.outputSampleRate;
	props_.format.channels = impl_->dec.outputChannels;
	ma_uint64 total = 0;
	if(ma_decoder_get_length_in_pcm_frames(&impl_->dec, &total) == MA_SUCCESS) {
		props_.total_frames = static_cast<int64_t>(total);
	} else {
		props_.total_frames = -1;
	}
	props_.codec = "MP3";

	// Tag path: libid3tag reopens the file directly by path. Works for
	// file:// sources; other source schemes simply yield no tags.
	read_id3_tags(path_from_url(source->url()));

	return props_.format.valid();
}

void Mp3Decoder::close() {
	if(impl_ && impl_->inited) {
		ma_decoder_uninit(&impl_->dec);
		impl_->inited = false;
	}
	if(impl_) impl_->src = nullptr;
	props_ = {};
	id3_tags_ = nlohmann::json::object();
	picture_mime_.clear();
	picture_bytes_.clear();
}

void Mp3Decoder::read_id3_tags(const std::string &path) {
	id3_file *f = id3_file_open(path.c_str(), ID3_FILE_MODE_READONLY);
	if(!f) return;
	id3_tag *tag = id3_file_tag(f);
	if(!tag) { id3_file_close(f); return; }

	for(unsigned int i = 0; i < tag->nframes; ++i) {
		const id3_frame *frame = tag->frames[i];
		if(!frame) continue;
		if(frame->id[0] == 'T' && std::strcmp(frame->id, "TXXX") != 0 &&
		   std::strcmp(frame->id, "TXX") != 0) {
			handle_text_frame(frame, id3_tags_);
		} else if(std::strcmp(frame->id, "TXXX") == 0 ||
		          std::strcmp(frame->id, "TXX")  == 0) {
			handle_txxx(frame, id3_tags_);
		} else if(std::strcmp(frame->id, "COMM") == 0 ||
		          std::strcmp(frame->id, "COM")  == 0) {
			handle_comm(frame, id3_tags_);
		} else if(std::strcmp(frame->id, "USLT") == 0 ||
		          std::strcmp(frame->id, "ULT")  == 0) {
			handle_uslt(frame, id3_tags_);
		} else if(std::strcmp(frame->id, "APIC") == 0 ||
		          std::strcmp(frame->id, "PIC")  == 0) {
			handle_apic(frame, picture_mime_, picture_bytes_);
		}
	}
	id3_file_close(f);
}

bool Mp3Decoder::read(AudioChunk &out, size_t max_frames) {
	if(!impl_->inited || max_frames == 0) return false;

	const uint32_t ch = props_.format.channels;
	std::vector<float> buf(max_frames * ch);

	ma_uint64 frames_read = 0;
	ma_result r = ma_decoder_read_pcm_frames(&impl_->dec, buf.data(), max_frames, &frames_read);
	if(frames_read == 0) return false;

	buf.resize(frames_read * ch);
	out = AudioChunk(props_.format, std::move(buf));
	return r == MA_SUCCESS || r == MA_AT_END;
}

int64_t Mp3Decoder::seek(int64_t frame) {
	if(!impl_->inited) return -1;
	if(ma_decoder_seek_to_pcm_frame(&impl_->dec, static_cast<ma_uint64>(frame)) != MA_SUCCESS) {
		return -1;
	}
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
	out["codec"] = "MP3";
	if(!picture_bytes_.empty()) {
		out["album_art"] = {
		    {"mime", picture_mime_},
		    {"data_b64", vorbis_common::base64_encode(picture_bytes_.data(), picture_bytes_.size())},
		};
	}
	return out;
}

} // namespace tuxedo
