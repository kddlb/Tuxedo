#include "plugin/input/hls_memory_source.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tuxedo {

HlsMemorySource::HlsMemorySource(std::string url, std::string mime_type)
    : url_(std::move(url)), mime_type_(std::move(mime_type)) {}

HlsMemorySource::~HlsMemorySource() { close(); }

void HlsMemorySource::append_data(const uint8_t *data, size_t size) {
	if(size == 0) return;
	std::lock_guard<std::mutex> g(mtx_);
	if(closed_) return;
	chunks_.emplace_back(data, data + size);
	cv_.notify_all();
}

void HlsMemorySource::mark_end_of_stream() {
	std::lock_guard<std::mutex> g(mtx_);
	eof_ = true;
	cv_.notify_all();
}

void HlsMemorySource::reset() {
	std::lock_guard<std::mutex> g(mtx_);
	chunks_.clear();
	front_offset_ = 0;
	position_ = 0;
	eof_ = false;
	cv_.notify_all();
}

size_t HlsMemorySource::buffered_segment_count() const {
	std::lock_guard<std::mutex> g(mtx_);
	return chunks_.size();
}

void HlsMemorySource::set_url(std::string url) {
	std::lock_guard<std::mutex> g(mtx_);
	url_ = std::move(url);
}

void HlsMemorySource::set_mime_type(std::string mime_type) {
	std::lock_guard<std::mutex> g(mtx_);
	mime_type_ = std::move(mime_type);
}

bool HlsMemorySource::open(const std::string &url) {
	std::lock_guard<std::mutex> g(mtx_);
	url_ = url;
	return true;
}

void HlsMemorySource::close() {
	std::lock_guard<std::mutex> g(mtx_);
	closed_ = true;
	eof_ = true;
	chunks_.clear();
	front_offset_ = 0;
	cv_.notify_all();
}

int64_t HlsMemorySource::tell() const {
	std::lock_guard<std::mutex> g(mtx_);
	return position_;
}

int64_t HlsMemorySource::read(void *buffer, size_t amount) {
	if(amount == 0) return 0;

	uint8_t *dst = static_cast<uint8_t *>(buffer);
	size_t total = 0;

	std::unique_lock<std::mutex> lk(mtx_);
	while(total < amount) {
		if(closed_) break;

		while(chunks_.empty() && !eof_ && !closed_) cv_.wait(lk);
		if(chunks_.empty()) break; // EOF or closed with no data left

		auto &front = chunks_.front();
		size_t available = front.size() - front_offset_;
		size_t take = std::min(available, amount - total);

		if(take > 0) {
			std::memcpy(dst + total, front.data() + front_offset_, take);
			front_offset_ += take;
			position_ += static_cast<int64_t>(take);
			total += take;
		}

		if(front_offset_ >= front.size()) {
			chunks_.pop_front();
			front_offset_ = 0;
			cv_.notify_all();
		}
	}

	return static_cast<int64_t>(total);
}

} // namespace tuxedo
