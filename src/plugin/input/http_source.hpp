#pragma once

#include "plugin/source.hpp"

#include <curl/curl.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tuxedo {

class HttpSource : public Source {
public:
	HttpSource();
	~HttpSource() override;

	bool open(const std::string &url) override;
	void close() override;

	bool seekable() const override;
	bool seek(int64_t offset, int whence) override;
	int64_t tell() const override;

	int64_t read(void *buffer, size_t amount) override;

	const std::string &url() const override { return url_; }
	const std::string &mime_type() const override { return mime_type_; }
	nlohmann::json metadata() const override;
	void set_metadata_changed_callback(MetadataChangedCallback cb) override;

	static std::vector<std::string> schemes() { return {"http", "https"}; }

private:
	static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
	static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
	static int progress_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

	static constexpr size_t kBufferSize = 256 * 1024;

	bool start_transfer(int64_t offset);
	void stop_transfer();
	void worker_loop(int64_t offset);

	size_t on_header(const char *data, size_t len);
	size_t on_write(const uint8_t *data, size_t len);

	void parse_header_line(const std::string &line);
	bool push_audio_bytes(const uint8_t *data, size_t len);
	void handle_stream_title(const std::string &title);
	void parse_icy_metadata(const std::string &block);

	mutable std::mutex mtx_;
	std::condition_variable cv_;
	std::thread worker_;

	std::vector<uint8_t> buffer_;
	size_t read_pos_ = 0;
	size_t write_pos_ = 0;
	size_t buffered_ = 0;

	std::string url_;
	std::string mime_type_;
	nlohmann::json metadata_ = nlohmann::json::object();
	MetadataChangedCallback metadata_changed_cb_;

	bool headers_ready_ = false;
	bool stop_requested_ = false;
	bool eof_ = false;
	bool failed_ = false;
	bool accept_ranges_ = false;
	bool seekable_ = false;

	int64_t position_ = 0;
	int64_t content_length_ = -1;

	long icy_metaint_ = 0;
	long icy_wait_ = 0;
	size_t icy_block_remaining_ = 0;
	std::string icy_block_;
};

} // namespace tuxedo
