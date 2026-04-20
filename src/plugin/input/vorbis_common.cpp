#include "plugin/input/vorbis_common.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace tuxedo::vorbis_common {

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

void accept_tag(nlohmann::json &tags, const char *name, size_t name_len,
                const char *value, size_t value_len) {
	std::string key = lowercase(std::string(name, name_len));

	// Side-channels the caller should have handled already.
	if(key == "metadata_block_picture") return;
	if(key == "waveformatextensible_channel_mask") return;

	key = canonicalise_tag(key);
	std::string val(value, value_len);

	auto it = tags.find(key);
	if(it == tags.end()) {
		tags[key] = nlohmann::json::array({std::move(val)});
	} else {
		it->push_back(std::move(val));
	}
}

void accept_entry(nlohmann::json &tags, const char *entry, size_t length) {
	const char *eq = static_cast<const char *>(std::memchr(entry, '=', length));
	if(!eq) return;
	const size_t name_len = static_cast<size_t>(eq - entry);
	const size_t value_len = length - name_len - 1;
	accept_tag(tags, entry, name_len, eq + 1, value_len);
}

std::vector<uint8_t> base64_decode(const char *data, size_t len) {
	static int8_t lut[256];
	static bool lut_init = false;
	if(!lut_init) {
		for(int i = 0; i < 256; ++i) lut[i] = -1;
		static const char tbl[] =
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for(int i = 0; i < 64; ++i) lut[(unsigned char)tbl[i]] = int8_t(i);
		lut_init = true;
	}

	std::vector<uint8_t> out;
	out.reserve((len * 3) / 4);

	uint32_t buf = 0;
	int nbits = 0;
	for(size_t i = 0; i < len; ++i) {
		unsigned char c = static_cast<unsigned char>(data[i]);
		if(c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
		if(c == '=') break;
		int8_t v = lut[c];
		if(v < 0) { out.clear(); return out; }
		buf = (buf << 6) | uint32_t(v);
		nbits += 6;
		if(nbits >= 8) {
			nbits -= 8;
			out.push_back(uint8_t((buf >> nbits) & 0xFF));
		}
	}
	return out;
}

bool unpack_flac_picture(const uint8_t *data, size_t len,
                         std::string &mime_out, std::vector<uint8_t> &bytes_out) {
	// FLAC PICTURE block format:
	//   u32 BE picture type
	//   u32 BE mime length; mime bytes (ASCII)
	//   u32 BE description length; description bytes (UTF-8)
	//   u32 BE width, height, depth, n_colors, data length; data bytes
	auto read_u32 = [&](size_t &off, uint32_t &v) -> bool {
		if(off + 4 > len) return false;
		v = (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
		    (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
		off += 4;
		return true;
	};

	size_t off = 0;
	uint32_t picture_type = 0;
	if(!read_u32(off, picture_type)) return false;

	uint32_t mime_len = 0;
	if(!read_u32(off, mime_len)) return false;
	if(off + mime_len > len) return false;
	mime_out.assign(reinterpret_cast<const char *>(data + off), mime_len);
	off += mime_len;

	uint32_t desc_len = 0;
	if(!read_u32(off, desc_len)) return false;
	if(off + desc_len > len) return false;
	off += desc_len; // skip description

	uint32_t width = 0, height = 0, depth = 0, ncolors = 0;
	if(!read_u32(off, width)) return false;
	if(!read_u32(off, height)) return false;
	if(!read_u32(off, depth)) return false;
	if(!read_u32(off, ncolors)) return false;

	uint32_t data_len = 0;
	if(!read_u32(off, data_len)) return false;
	if(off + data_len > len) return false;
	bytes_out.assign(data + off, data + off + data_len);
	(void)picture_type; // accept all types, though 3 (front cover) is canonical
	return true;
}

} // namespace tuxedo::vorbis_common
