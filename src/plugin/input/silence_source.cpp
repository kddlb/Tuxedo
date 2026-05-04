#include "plugin/input/silence_source.hpp"

#include <cstring>

namespace tuxedo {

bool SilenceSource::open(const std::string &url) {
	close();
	url_ = url;
	return true;
}

void SilenceSource::close() {
	url_.clear();
	position_ = 0;
}

bool SilenceSource::seek(int64_t offset, int whence) {
	switch(whence) {
		case SEEK_SET: position_ = offset; break;
		case SEEK_CUR: position_ += offset; break;
		case SEEK_END: position_ = offset; break;
		default: return false;
	}
	if(position_ < 0) position_ = 0;
	return true;
}

int64_t SilenceSource::read(void *buffer, size_t amount) {
	if(!buffer) return -1;
	std::memset(buffer, 0, amount);
	position_ += static_cast<int64_t>(amount);
	return static_cast<int64_t>(amount);
}

} // namespace tuxedo
