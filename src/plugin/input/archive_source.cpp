#include "plugin/input/archive_source.hpp"

#include "core/archive_url.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>

namespace tuxedo {

namespace {

std::string normalize_entry_name(std::string value) {
	std::replace(value.begin(), value.end(), '\\', '/');
	while(value.rfind("./", 0) == 0) value.erase(0, 2);
	return value;
}

bool read_entry_data(struct archive *archive, std::vector<uint8_t> &out) {
	out.clear();

	for(;;) {
		const void *block = nullptr;
		size_t size = 0;
		la_int64_t offset = 0;
		int rc = archive_read_data_block(archive, &block, &size, &offset);
		if(rc == ARCHIVE_EOF) return true;
		if(rc != ARCHIVE_OK) return false;

		size_t begin = static_cast<size_t>(offset);
		if(out.size() < begin + size) out.resize(begin + size);
		std::memcpy(out.data() + begin, block, size);
	}
}

} // namespace

ArchiveSource::~ArchiveSource() { close(); }

bool ArchiveSource::open(const std::string &url) {
	close();

	ArchiveUrlParts parts;
	if(!parse_archive_url(url, parts)) return false;
	if(parts.backend != "libarchive") return false;

	struct archive *archive = archive_read_new();
	if(!archive) return false;

	archive_read_support_filter_all(archive);
	archive_read_support_format_all(archive);

	bool ok = false;
	if(archive_read_open_filename(archive, parts.archive_path.c_str(), 10240) == ARCHIVE_OK) {
		const std::string wanted = normalize_entry_name(parts.entry_path);
		struct archive_entry *entry = nullptr;
		while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
			const char *name = archive_entry_pathname(entry);
			if(name && normalize_entry_name(name) == wanted) {
				ok = read_entry_data(archive, data_);
				break;
			}
			archive_read_data_skip(archive);
		}
	}

	archive_read_free(archive);
	if(!ok) {
		data_.clear();
		return false;
	}

	url_ = url;
	offset_ = 0;
	return true;
}

void ArchiveSource::close() {
	url_.clear();
	mime_.clear();
	data_.clear();
	offset_ = 0;
}

bool ArchiveSource::seek(int64_t offset, int whence) {
	int64_t absolute = 0;
	switch(whence) {
		case SEEK_SET:
			absolute = offset;
			break;
		case SEEK_CUR:
			absolute = static_cast<int64_t>(offset_) + offset;
			break;
		case SEEK_END:
			absolute = static_cast<int64_t>(data_.size()) + offset;
			break;
		default:
			return false;
	}
	if(absolute < 0 || absolute > static_cast<int64_t>(data_.size())) return false;
	offset_ = static_cast<size_t>(absolute);
	return true;
}

int64_t ArchiveSource::tell() const {
	return static_cast<int64_t>(offset_);
}

int64_t ArchiveSource::read(void *buffer, size_t amount) {
	if(offset_ >= data_.size()) return 0;
	size_t take = std::min(amount, data_.size() - offset_);
	std::memcpy(buffer, data_.data() + offset_, take);
	offset_ += take;
	return static_cast<int64_t>(take);
}

} // namespace tuxedo
