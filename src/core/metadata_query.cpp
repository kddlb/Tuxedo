#include "core/metadata_query.hpp"

#include "core/cue_sheet.hpp"
#include "core/media_probe.hpp"
#include "plugin/input/vorbis_common.hpp"

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tbytevector.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace tuxedo {

namespace {

std::string lowercase_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

std::string canonical_tag_name(const std::string &raw) {
	std::string lower = lowercase_copy(raw);
	if(lower == "album_artist") return "albumartist";
	if(lower == "track") return "tracknumber";
	if(lower == "disc") return "discnumber";
	if(lower == "date_recorded") return "date";
	if(lower == "itunnorm") return "soundcheck";
	if(lower == "replaygain_gain") return "replaygain_album_gain";
	if(lower == "replaygain_peak") return "replaygain_album_peak";
	return vorbis_common::canonicalise_tag(lower);
}

void push_tag(nlohmann::json &metadata, const std::string &key, const std::string &value) {
	if(value.empty()) return;
	auto it = metadata.find(key);
	if(it == metadata.end()) {
		metadata[key] = nlohmann::json::array({value});
	} else {
		it->push_back(value);
	}
}

bool has_value(const nlohmann::json &value) {
	if(value.is_null()) return false;
	if(value.is_string()) return !value.get_ref<const std::string &>().empty();
	if(value.is_array() || value.is_object()) return !value.empty();
	return true;
}

void merge_missing(nlohmann::json &target, const nlohmann::json &fallback) {
	for(auto it = fallback.begin(); it != fallback.end(); ++it) {
		auto existing = target.find(it.key());
		if(existing == target.end() || !has_value(*existing)) {
			target[it.key()] = it.value();
		}
	}
}

std::string path_from_url(const std::string &url) {
	static const std::string prefix = "file://";
	if(url.compare(0, prefix.size(), prefix) == 0) return url.substr(prefix.size());
	if(url.find("://") != std::string::npos) return {};
	return url;
}

std::string guess_image_mime(const TagLib::ByteVector &data) {
	if(data.size() >= 3 &&
	   static_cast<unsigned char>(data[0]) == 0xFF &&
	   static_cast<unsigned char>(data[1]) == 0xD8 &&
	   static_cast<unsigned char>(data[2]) == 0xFF) {
		return "image/jpeg";
	}
	if(data.size() >= 8 &&
	   std::equal(data.begin(), data.begin() + 8,
	              "\x89PNG\r\n\x1A\n")) {
		return "image/png";
	}
	if(data.size() >= 6) {
		std::string sig(data.begin(), data.begin() + 6);
		if(sig == "GIF87a" || sig == "GIF89a") return "image/gif";
	}
	if(data.size() >= 2 && data[0] == 'B' && data[1] == 'M') return "image/bmp";
	if(data.size() >= 4) {
		if((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
		   (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42)) {
			return "image/tiff";
		}
	}
	return "application/octet-stream";
}

bool taglib_metadata_for_url(const std::string &url, nlohmann::json &metadata) {
	if(cue_has_track_fragment(url)) return false;
	std::string path = path_from_url(url);
	if(path.empty()) return false;

	try {
		TagLib::FileRef file(path.c_str(), true, TagLib::AudioProperties::Average);
		if(file.isNull()) return false;

		bool found = false;
		TagLib::PropertyMap tags = file.properties();
		for(auto it = tags.cbegin(); it != tags.cend(); ++it) {
			std::string key = canonical_tag_name(it->first.toCString(true));
			for(auto value = it->second.begin(); value != it->second.end(); ++value) {
				push_tag(metadata, key, value->toCString(true));
				found = true;
			}
		}

		TagLib::Tag *tag = file.tag();
		if(tag) {
			std::string title = tag->title().toCString(true);
			if(!title.empty() && !metadata.contains("title")) {
				push_tag(metadata, "title", title);
				found = true;
			}
		}

		TagLib::StringList keys = file.complexPropertyKeys();
		if(keys.contains("PICTURE")) {
			TagLib::List<TagLib::VariantMap> pictures = file.complexProperties("PICTURE");
			if(!pictures.isEmpty()) {
				const TagLib::VariantMap &picture = pictures.front();
				auto data_it = picture.find("data");
				if(data_it != picture.end()) {
					bool ok = false;
					TagLib::ByteVector bytes = data_it->second.toByteVector(&ok);
					if(ok && !bytes.isEmpty()) {
						std::string mime = guess_image_mime(bytes);
						metadata["album_art"] = {
						    {"mime", mime},
						    {"data_b64", vorbis_common::base64_encode(
						                     reinterpret_cast<const uint8_t *>(bytes.data()),
						                     bytes.size())},
						};
						found = true;
					}
				}
			}
		}

		return found;
	} catch(...) {
		return false;
	}
}

bool taglib_properties_for_url(const std::string &url, nlohmann::json &properties) {
	if(cue_has_track_fragment(url)) return false;
	std::string path = path_from_url(url);
	if(path.empty()) return false;

	try {
		TagLib::FileRef file(path.c_str(), true, TagLib::AudioProperties::Average);
		if(file.isNull()) return false;

		TagLib::AudioProperties *audio = file.audioProperties();
		if(!audio) return false;

		if(audio->sampleRate() > 0) properties["sample_rate"] = audio->sampleRate();
		if(audio->channels() > 0) properties["channels"] = audio->channels();
		if(audio->bitrate() > 0) properties["bitrate_kbps"] = audio->bitrate();
		if(audio->lengthInMilliseconds() > 0) {
			double duration = static_cast<double>(audio->lengthInMilliseconds()) / 1000.0;
			properties["duration"] = duration;
			if(audio->sampleRate() > 0) {
				properties["total_frames"] = static_cast<int64_t>(duration * audio->sampleRate());
			}
		}
		return !properties.empty();
	} catch(...) {
		return false;
	}
}

bool decoder_metadata_for_url(const std::string &url, nlohmann::json &metadata) {
	OpenedMedia opened;
	if(!open_media_url(url, opened)) return false;
	metadata = opened.decoder->metadata();
	return !metadata.empty();
}

bool decoder_properties_for_url(const std::string &url, nlohmann::json &properties) {
	OpenedMedia opened;
	if(!open_media_url(url, opened)) return false;

	properties["codec"] = opened.properties.codec;
	properties["sample_rate"] = opened.properties.format.sample_rate;
	properties["channels"] = opened.properties.format.channels;
	properties["seekable"] = opened.source->seekable();
	if(!opened.source->mime_type().empty()) properties["mime_type"] = opened.source->mime_type();
	if(opened.properties.total_frames >= 0) {
		properties["total_frames"] = opened.properties.total_frames;
		if(opened.properties.format.sample_rate > 0) {
			properties["duration"] =
			    static_cast<double>(opened.properties.total_frames) /
			    opened.properties.format.sample_rate;
		}
	}
	return true;
}

} // namespace

bool read_metadata_for_url(const std::string &url, nlohmann::json &out) {
	out = nlohmann::json::object();

	nlohmann::json decoder_metadata = nlohmann::json::object();
	nlohmann::json taglib_metadata = nlohmann::json::object();

	const bool have_decoder = decoder_metadata_for_url(url, decoder_metadata);
	const bool have_taglib = taglib_metadata_for_url(url, taglib_metadata);
	if(!have_decoder && !have_taglib) return false;

	out = std::move(decoder_metadata);
	merge_missing(out, taglib_metadata);
	return true;
}

bool read_properties_for_url(const std::string &url, nlohmann::json &out) {
	out = nlohmann::json::object();

	nlohmann::json decoder_properties = nlohmann::json::object();
	nlohmann::json taglib_properties = nlohmann::json::object();

	const bool have_decoder = decoder_properties_for_url(url, decoder_properties);
	const bool have_taglib = taglib_properties_for_url(url, taglib_properties);
	if(!have_decoder && !have_taglib) return false;

	out = std::move(decoder_properties);
	merge_missing(out, taglib_properties);
	return true;
}

} // namespace tuxedo
