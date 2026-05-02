#include "plugin/input/http_source.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

namespace tuxedo {

namespace {

enum { rewind_threshold_ = 5000 };

std::string trim_copy(std::string s) {
	while(!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
	                     std::isspace(static_cast<unsigned char>(s.back())))) {
		s.pop_back();
	}
	size_t first = 0;
	while(first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
	return s.substr(first);
}

std::string lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

void set_metadata_field(nlohmann::json &metadata, const char *key, const std::string &value) {
	if(value.empty()) {
		metadata.erase(key);
		return;
	}
	metadata[key] = nlohmann::json::array({value});
}

bool parse_int64(const std::string &text, int64_t &out) {
	try {
		size_t used = 0;
		long long value = std::stoll(text, &used, 10);
		if(used != text.size()) return false;
		out = value;
		return true;
	} catch(...) {
		return false;
	}
}

} // namespace

size_t HttpSource::write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
	return static_cast<HttpSource *>(userdata)->on_write(
	    reinterpret_cast<const uint8_t *>(ptr), size * nmemb);
}

size_t HttpSource::header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
	return static_cast<HttpSource *>(userdata)->on_header(ptr, size * nmemb);
}

int HttpSource::progress_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto *self = static_cast<HttpSource *>(clientp);
	std::lock_guard<std::mutex> g(self->mtx_);
	return self->stop_requested_ ? 1 : 0;
}

HttpSource::HttpSource() : buffer_(kBufferSize) {
	static std::once_flag once;
	std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpSource::~HttpSource() { close(); }

bool HttpSource::open(const std::string &url) {
	close();

	{
		std::lock_guard<std::mutex> g(mtx_);
		url_ = url;
	}

	if(!start_transfer(0)) return false;

	std::unique_lock<std::mutex> lk(mtx_);
	cv_.wait(lk, [this] { return headers_ready_ || eof_ || failed_; });
	return !failed_;
}

void HttpSource::close() {
	stop_transfer();

	std::lock_guard<std::mutex> g(mtx_);
	read_pos_ = 0;
	write_pos_ = 0;
	buffered_ = 0;
	headers_ready_ = false;
	eof_ = false;
	failed_ = false;
	accept_ranges_ = false;
	seekable_ = false;
	position_ = 0;
	content_length_ = -1;
	icy_metaint_ = 0;
	icy_wait_ = 0;
	icy_block_remaining_ = 0;
	icy_block_.clear();
	mime_type_.clear();
	metadata_ = nlohmann::json::object();
	url_.clear();
}

bool HttpSource::seekable() const {
	std::lock_guard<std::mutex> g(mtx_);
	return seekable_;
}

bool HttpSource::seek(int64_t offset, int whence) {
	int64_t absolute = 0;
	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!seekable_) return false;
		switch(whence) {
			case SEEK_SET:
				absolute = offset;
				break;
			case SEEK_CUR:
				absolute = position_ + offset;
				break;
			case SEEK_END:
				if(content_length_ < 0) return false;
				absolute = content_length_ + offset;
				break;
			default:
				return false;
		}
		if(absolute < 0) return false;
		if(content_length_ >= 0 && absolute > content_length_) return false;
	}

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(absolute == position_) return true;
		if(absolute < position_) {
			/* Support limited rewinding */
			int64_t relative = position_ - absolute;
			if(relative <= rewind_threshold_) {
				read_pos_ -= relative;
				if(read_pos_ > buffer_.size()) read_pos_ += buffer_.size();
				buffered_ += relative;
				return true;
			}
		} else if(absolute > position_) {
			/* Support limited skipping */
			int64_t relative = absolute - position_;
			if((size_t)relative <= buffered_) {
				read_pos_ = (read_pos_ + relative) % buffer_.size();
				buffered_ -= relative;
				return true;
			}
		}
	}

	stop_transfer();

	{
		std::lock_guard<std::mutex> g(mtx_);
		read_pos_ = 0;
		write_pos_ = 0;
		buffered_ = 0;
		headers_ready_ = false;
		eof_ = false;
		failed_ = false;
		position_ = absolute;
		icy_metaint_ = 0;
		icy_wait_ = 0;
		icy_block_remaining_ = 0;
		icy_block_.clear();
	}

	if(!start_transfer(absolute)) return false;

	std::unique_lock<std::mutex> lk(mtx_);
	cv_.wait(lk, [this] { return headers_ready_ || eof_ || failed_; });
	return !failed_;
}

int64_t HttpSource::tell() const {
	std::lock_guard<std::mutex> g(mtx_);
	return position_;
}

int64_t HttpSource::read(void *buffer, size_t amount) {
	std::unique_lock<std::mutex> lk(mtx_);
	cv_.wait(lk, [this] { return buffered_ > 0 || eof_ || failed_ || stop_requested_; });

	if(failed_) return -1;
	if(buffered_ == 0) return 0;

	const size_t take = std::min(amount, buffered_);
	uint8_t *out = static_cast<uint8_t *>(buffer);
	size_t first = std::min(take, buffer_.size() - read_pos_);
	std::memcpy(out, buffer_.data() + read_pos_, first);
	if(first < take) {
		std::memcpy(out + first, buffer_.data(), take - first);
	}

	read_pos_ = (read_pos_ + take) % buffer_.size();
	buffered_ -= take;
	position_ += static_cast<int64_t>(take);
	cv_.notify_all();
	return static_cast<int64_t>(take);
}

nlohmann::json HttpSource::metadata() const {
	std::lock_guard<std::mutex> g(mtx_);
	return metadata_;
}

void HttpSource::set_metadata_changed_callback(MetadataChangedCallback cb) {
	std::lock_guard<std::mutex> g(mtx_);
	metadata_changed_cb_ = std::move(cb);
}

bool HttpSource::start_transfer(int64_t offset) {
	{
		std::lock_guard<std::mutex> g(mtx_);
		stop_requested_ = false;
	}

	try {
		worker_ = std::thread([this, offset] { worker_loop(offset); });
	} catch(...) {
		std::lock_guard<std::mutex> g(mtx_);
		failed_ = true;
		cv_.notify_all();
		return false;
	}
	return true;
}

void HttpSource::stop_transfer() {
	{
		std::lock_guard<std::mutex> g(mtx_);
		stop_requested_ = true;
		cv_.notify_all();
	}
	if(worker_.joinable()) worker_.join();
}

void HttpSource::worker_loop(int64_t offset) {
	CURL *curl = curl_easy_init();
	if(!curl) {
		std::lock_guard<std::mutex> g(mtx_);
		failed_ = true;
		cv_.notify_all();
		return;
	}

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Icy-MetaData: 1");

	curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpSource::write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HttpSource::header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &HttpSource::progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "tuxedo/0.0.1");
	if(offset > 0) {
		curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
	}

	CURLcode rc = curl_easy_perform(curl);
	const bool aborted = (rc == CURLE_ABORTED_BY_CALLBACK);

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!headers_ready_) headers_ready_ = true;
		if(rc == CURLE_OK) {
			eof_ = true;
		} else if(!aborted || !stop_requested_) {
			failed_ = true;
		}
		cv_.notify_all();
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
}

size_t HttpSource::on_header(const char *data, size_t len) {
	std::string line(data, len);
	parse_header_line(line);
	return len;
}

size_t HttpSource::on_write(const uint8_t *data, size_t len) {
	size_t consumed = 0;
	while(consumed < len) {
		size_t chunk = len - consumed;

		{
			std::unique_lock<std::mutex> lk(mtx_);
			if(stop_requested_) return 0;
			headers_ready_ = true;
			cv_.notify_all();

			if(icy_block_remaining_ > 0) {
				size_t take = std::min(chunk, icy_block_remaining_);
				icy_block_.append(reinterpret_cast<const char *>(data + consumed), take);
				icy_block_remaining_ -= take;
				consumed += take;
				if(icy_block_remaining_ == 0) parse_icy_metadata(icy_block_);
				continue;
			}

			if(icy_metaint_ > 0 && icy_wait_ == 0) {
				icy_block_remaining_ = static_cast<size_t>(data[consumed]) * 16;
				icy_block_.clear();
				icy_wait_ = icy_metaint_;
				++consumed;
				continue;
			}

			if(icy_metaint_ > 0) {
				chunk = std::min(chunk, static_cast<size_t>(icy_wait_));
			}
		}

		if(!push_audio_bytes(data + consumed, chunk)) return 0;
		consumed += chunk;

		std::lock_guard<std::mutex> g(mtx_);
		if(icy_metaint_ > 0) icy_wait_ -= static_cast<long>(chunk);
	}

	return len;
}

void HttpSource::parse_header_line(const std::string &line) {
	MetadataChangedCallback metadata_changed;
	std::unique_lock<std::mutex> g(mtx_);

	if(line == "\r\n" || line == "\n") {
		headers_ready_ = true;
		seekable_ = accept_ranges_ && content_length_ >= 0 && icy_metaint_ == 0;
		icy_wait_ = icy_metaint_;
		cv_.notify_all();
		return;
	}

	auto colon = line.find(':');
	if(colon == std::string::npos) return;

	std::string key = lowercase(trim_copy(line.substr(0, colon)));
	std::string value = trim_copy(line.substr(colon + 1));

	if(key == "content-type") {
		mime_type_ = value;
	} else if(key == "content-length") {
		parse_int64(value, content_length_);
	} else if(key == "accept-ranges") {
		accept_ranges_ = lowercase(value) == "bytes";
	} else if(key == "content-range") {
		auto slash = value.rfind('/');
		if(slash != std::string::npos) {
			int64_t total = -1;
			if(parse_int64(value.substr(slash + 1), total)) {
				content_length_ = total;
				accept_ranges_ = true;
			}
		}
	} else if(key == "icy-metaint") {
		int64_t metaint = 0;
		if(parse_int64(value, metaint) && metaint > 0 &&
		   metaint <= std::numeric_limits<long>::max()) {
			icy_metaint_ = static_cast<long>(metaint);
		}
	} else if(key == "icy-name") {
		if(metadata_.value("title", nlohmann::json::array()) != nlohmann::json::array({value}))
			metadata_changed = metadata_changed_cb_;
		set_metadata_field(metadata_, "title", value);
	} else if(key == "icy-genre") {
		if(metadata_.value("genre", nlohmann::json::array()) != nlohmann::json::array({value}))
			metadata_changed = metadata_changed_cb_;
		set_metadata_field(metadata_, "genre", value);
	} else if(key == "icy-url") {
		if(metadata_.value("album", nlohmann::json::array()) != nlohmann::json::array({value}))
			metadata_changed = metadata_changed_cb_;
		set_metadata_field(metadata_, "album", value);
	}
	g.unlock();
	if(metadata_changed) metadata_changed();
}

bool HttpSource::push_audio_bytes(const uint8_t *data, size_t len) {
	size_t written = 0;
	while(written < len) {
		std::unique_lock<std::mutex> lk(mtx_);
		cv_.wait(lk, [this] { return buffered_ + rewind_threshold_ < buffer_.size() || stop_requested_; });
		if(stop_requested_) return false;

		size_t free_space = buffer_.size() - buffered_;
		size_t take = std::min(len - written, free_space);
		size_t first = std::min(take, buffer_.size() - write_pos_);
		std::memcpy(buffer_.data() + write_pos_, data + written, first);
		if(first < take) {
			std::memcpy(buffer_.data(), data + written + first, take - first);
		}

		write_pos_ = (write_pos_ + take) % buffer_.size();
		buffered_ += take;
		written += take;
		cv_.notify_all();
	}

	return true;
}

void HttpSource::handle_stream_title(const std::string &title) {
	std::string artist;
	std::string track = trim_copy(title);

	auto sep = track.find(" - ");
	if(sep != std::string::npos) {
		artist = trim_copy(track.substr(0, sep));
		track = trim_copy(track.substr(sep + 3));
	}

	MetadataChangedCallback metadata_changed;
	{
		std::lock_guard<std::mutex> g(mtx_);
		nlohmann::json old_artist = metadata_.contains("artist") ? metadata_["artist"] : nlohmann::json::array();
		nlohmann::json old_title = metadata_.contains("title") ? metadata_["title"] : nlohmann::json::array();
		set_metadata_field(metadata_, "artist", artist);
		set_metadata_field(metadata_, "title", track);
		if(old_artist != metadata_.value("artist", nlohmann::json::array()) ||
		   old_title != metadata_.value("title", nlohmann::json::array())) {
			metadata_changed = metadata_changed_cb_;
		}
	}
	if(metadata_changed) metadata_changed();
}

void HttpSource::parse_icy_metadata(const std::string &block) {
	auto begin = block.find("StreamTitle='");
	if(begin != std::string::npos) {
		begin += std::strlen("StreamTitle='");
		auto end = block.find("';", begin);
		if(end != std::string::npos) {
			handle_stream_title(block.substr(begin, end - begin));
		}
	}
}

} // namespace tuxedo
