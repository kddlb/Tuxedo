#include "core/archive_url.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace tuxedo {

namespace {

std::string strip_suffix(std::string url) {
	size_t suffix = url.find_first_of("?#");
	if(suffix != std::string::npos) url.erase(suffix);
	return url;
}

bool parse_length_prefix(const std::string &input, size_t &cursor, size_t &out_len) {
	const size_t bar = input.find('|', cursor);
	if(bar == std::string::npos || bar == cursor) return false;
	out_len = 0;
	for(size_t i = cursor; i < bar; ++i) {
		unsigned char c = static_cast<unsigned char>(input[i]);
		if(!std::isdigit(c)) return false;
		out_len = (out_len * 10) + static_cast<size_t>(c - '0');
	}
	cursor = bar + 1;
	return true;
}

int hex_value(char c) {
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

std::string percent_decode(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for(size_t i = 0; i < value.size(); ++i) {
		if(value[i] == '%' && i + 2 < value.size()) {
			int hi = hex_value(value[i + 1]);
			int lo = hex_value(value[i + 2]);
			if(hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(value[i]);
	}
	return out;
}

std::string percent_encode(const std::string &value) {
	std::ostringstream out;
	out.fill('0');
	out.setf(std::ios::uppercase);
	for(unsigned char c : value) {
		if(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
			out << static_cast<char>(c);
		} else {
			out << '%' << std::hex << std::uppercase;
			out.width(2);
			out << static_cast<int>(c);
			out << std::dec;
		}
	}
	return out.str();
}

std::string normalize_entry_path(std::string path) {
	std::replace(path.begin(), path.end(), '\\', '/');
	if(path.rfind("./", 0) == 0) path.erase(0, 2);
	return path;
}

} // namespace

bool parse_archive_url(const std::string &url, ArchiveUrlParts &out) {
	out = {};
	static const std::string prefix = "unpack://";
	if(url.compare(0, prefix.size(), prefix) != 0) return false;

	std::string core = strip_suffix(url.substr(prefix.size()));
	size_t cursor = core.find('|');
	if(cursor == std::string::npos || cursor == 0) return false;

	out.backend = core.substr(0, cursor);
	cursor += 1;

	size_t archive_len = 0;
	if(!parse_length_prefix(core, cursor, archive_len)) return false;
	if(cursor + archive_len > core.size()) return false;

	out.archive_path = percent_decode(core.substr(cursor, archive_len));
	cursor += archive_len;
	if(cursor >= core.size() || core[cursor] != '|') return false;
	cursor += 1;

	out.entry_path = normalize_entry_path(percent_decode(core.substr(cursor)));
	return !out.backend.empty() && !out.archive_path.empty() && !out.entry_path.empty();
}

std::string build_archive_url(const ArchiveUrlParts &parts) {
	if(parts.backend.empty() || parts.archive_path.empty() || parts.entry_path.empty()) return {};

	const std::string archive = percent_encode(parts.archive_path);
	const std::string entry = percent_encode(normalize_entry_path(parts.entry_path));
	return "unpack://" + parts.backend + "|" + std::to_string(archive.size()) + "|" + archive + "|" + entry;
}

std::string resolve_archive_relative_url(const std::string &entry, const std::string &base_url) {
	ArchiveUrlParts base;
	if(entry.empty() || !parse_archive_url(base_url, base)) return {};

	std::string path = entry;
	std::replace(path.begin(), path.end(), '\\', '/');

	std::filesystem::path resolved = std::filesystem::path(path).is_absolute()
	    ? std::filesystem::path(path).relative_path()
	    : std::filesystem::path(base.entry_path).parent_path() / path;
	return build_archive_url({
	    base.backend,
	    base.archive_path,
	    resolved.lexically_normal().generic_string(),
	});
}

} // namespace tuxedo
