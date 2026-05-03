#pragma once

#include <string>

namespace tuxedo {

struct ArchiveUrlParts {
	std::string backend;
	std::string archive_path;
	std::string entry_path;
};

bool parse_archive_url(const std::string &url, ArchiveUrlParts &out);
std::string build_archive_url(const ArchiveUrlParts &parts);
std::string resolve_archive_relative_url(const std::string &entry, const std::string &base_url);

} // namespace tuxedo
