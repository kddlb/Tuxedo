// In-memory Source backed by a queue of byte chunks. Producer
// (HlsSegmentManager's fetch thread) calls append_data; consumer
// (FfmpegDecoder via its AVIO context) calls read which blocks until
// data, EOF, or close. Ported from Cog's HLSMemorySource.
#pragma once

#include "plugin/source.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tuxedo {

class HlsMemorySource : public Source {
public:
	HlsMemorySource(std::string url, std::string mime_type);
	~HlsMemorySource() override;

	// Producer API.
	void append_data(const uint8_t *data, size_t size);
	void mark_end_of_stream();
	void reset();
	size_t buffered_segment_count() const;
	void set_url(std::string url);
	void set_mime_type(std::string mime_type);

	// Source overrides.
	bool open(const std::string &url) override;
	void close() override;
	bool seekable() const override { return false; }
	bool seek(int64_t, int) override { return false; }
	int64_t tell() const override;
	int64_t read(void *buffer, size_t amount) override;

	const std::string &url() const override { return url_; }
	const std::string &mime_type() const override { return mime_type_; }

private:
	mutable std::mutex mtx_;
	std::condition_variable cv_;

	std::deque<std::vector<uint8_t>> chunks_;
	size_t front_offset_ = 0;
	int64_t position_ = 0;
	bool eof_ = false;
	bool closed_ = false;

	std::string url_;
	std::string mime_type_;
};

} // namespace tuxedo
