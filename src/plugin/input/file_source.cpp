#include "plugin/input/file_source.hpp"

#include <cstdio>
#include <string>

namespace tuxedo {

namespace {
// Strip "file://" (and a possible leading slash-triplet for file:///).
std::string path_from_url(const std::string &url) {
	const std::string pfx = "file://";
	if(url.compare(0, pfx.size(), pfx) == 0) return url.substr(pfx.size());
	return url;
}
} // namespace

FileSource::~FileSource() { close(); }

bool FileSource::open(const std::string &url) {
	close();
	url_ = url;
	const std::string path = path_from_url(url);
	fp_ = std::fopen(path.c_str(), "rb");
	return fp_ != nullptr;
}

void FileSource::close() {
	if(fp_) {
		std::fclose(fp_);
		fp_ = nullptr;
	}
}

bool FileSource::seek(int64_t offset, int whence) {
	if(!fp_) return false;
	return std::fseek(fp_, static_cast<long>(offset), whence) == 0;
}

int64_t FileSource::tell() const {
	return fp_ ? std::ftell(fp_) : -1;
}

int64_t FileSource::read(void *buffer, size_t amount) {
	if(!fp_) return -1;
	size_t n = std::fread(buffer, 1, amount, fp_);
	if(n == 0 && std::ferror(fp_)) return -1;
	return static_cast<int64_t>(n);
}

} // namespace tuxedo
