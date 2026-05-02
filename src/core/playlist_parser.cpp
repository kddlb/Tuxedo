#include "core/playlist_parser.hpp"

#include "plugin/registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace tuxedo {

namespace {

std::string trim(const std::string &s) {
	size_t start = 0;
	while(start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
	size_t end = s.size();
	while(end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
	return s.substr(start, end - start);
}

std::string lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

bool has_scheme(const std::string &url) {
	return !PluginRegistry::scheme_of(url).empty();
}

std::string origin_of(const std::string &url) {
	size_t scheme = url.find("://");
	if(scheme == std::string::npos) return {};
	size_t slash = url.find('/', scheme + 3);
	return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string directory_of_url(const std::string &url) {
	size_t slash = url.find_last_of('/');
	return slash == std::string::npos ? url : url.substr(0, slash + 1);
}

std::string resolve_relative_url(const std::string &entry, const std::string &base_url) {
	if(entry.empty()) return {};
	if(has_scheme(entry)) return entry;

	std::string path = entry;
	std::string fragment;
	size_t hash = path.rfind('#');
	if(hash != std::string::npos) {
		fragment = path.substr(hash);
		path.erase(hash);
	}

	const std::string base_scheme = PluginRegistry::scheme_of(base_url);
	if(base_scheme == "http" || base_scheme == "https") {
		if(!path.empty() && path[0] == '/') return origin_of(base_url) + path + fragment;
		return directory_of_url(base_url) + path + fragment;
	}

	std::replace(path.begin(), path.end(), '\\', '/');
	std::string base_path = base_url;
	static const std::string file_prefix = "file://";
	if(base_path.compare(0, file_prefix.size(), file_prefix) == 0) base_path.erase(0, file_prefix.size());
	std::filesystem::path resolved = std::filesystem::path(path).is_absolute()
	    ? std::filesystem::path(path)
	    : std::filesystem::path(base_path).parent_path() / path;
	return resolved.lexically_normal().string() + fragment;
}

std::string read_all_text(const std::string &url) {
	auto source = PluginRegistry::instance().source_for_url(url);
	if(!source || !source->open(url)) return {};
	auto mime_type = source->mime_type();
	if(mime_type.length()) {
		mime_type = PluginRegistry::normalize_mime_type(mime_type);
		if(mime_type != "audio/x-scpls" &&
		   mime_type != "application/pls" &&
		   mime_type != "audio/x-mpegurl" &&
		   mime_type != "audio/mpegurl") {
			source->close();
			return {};
		}
	}

	std::string out;
	char buf[4096];
	for(;;) {
		int64_t n = source->read(buf, sizeof(buf));
		if(n <= 0) break;
		out.append(buf, static_cast<size_t>(n));
	}
	source->close();
	return out;
}

std::vector<std::string> split_lines(std::string text) {
	for(char &c : text) if(c == '\r') c = '\n';
	std::vector<std::string> lines;
	std::istringstream iss(text);
	std::string line;
	while(std::getline(iss, line, '\n')) lines.push_back(line);
	return lines;
}

PlaylistParseResult parse_m3u(const std::string &url, const std::string &text) {
	PlaylistParseResult result;
	result.recognized = true;
	for(const std::string &raw : split_lines(text)) {
		std::string line = trim(raw);
		if(line.empty()) continue;
		if(lowercase(line).rfind("#ext-x-media-sequence", 0) == 0) {
			result.passthrough_original = true;
			result.urls = {url};
			return result;
		}
		if(line[0] == '#') continue;
		std::string resolved = resolve_relative_url(line, url);
		if(!resolved.empty()) result.urls.push_back(std::move(resolved));
	}
	return result;
}

PlaylistParseResult parse_pls(const std::string &url, const std::string &text) {
	PlaylistParseResult result;
	result.recognized = true;
	for(const std::string &raw : split_lines(text)) {
		std::string line = trim(raw);
		auto eq = line.find('=');
		if(eq == std::string::npos) continue;
		std::string lhs = lowercase(trim(line.substr(0, eq)));
		if(lhs.rfind("file", 0) != 0) continue;
		std::string rhs = trim(line.substr(eq + 1));
		std::string resolved = resolve_relative_url(rhs, url);
		if(!resolved.empty()) result.urls.push_back(std::move(resolved));
	}
	return result;
}

} // namespace

PlaylistParseResult parse_playlist_url(const std::string &url) {
	PlaylistParseResult result;
	std::string ext = lowercase(PluginRegistry::extension_of(url));
	std::string scheme = lowercase(PluginRegistry::scheme_of(url));
	if(ext == "m3u8" && (scheme == "http" || scheme == "https")) {
		result.recognized = true;
		result.passthrough_original = true;
		return result;
	}
	std::string text = read_all_text(url);
	if(ext == "m3u" || ext == "m3u8") return parse_m3u(url, text);
	if(ext == "pls") return parse_pls(url, text);
	return result;
}

} // namespace tuxedo
