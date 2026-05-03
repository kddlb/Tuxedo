#pragma once

#include "plugin/source.hpp"

#include <vector>

namespace tuxedo {

class ArchiveSource final : public Source {
public:
	~ArchiveSource() override;

	bool open(const std::string &url) override;
	void close() override;

	bool seekable() const override { return true; }
	bool seek(int64_t offset, int whence) override;
	int64_t tell() const override;
	int64_t read(void *buffer, size_t amount) override;

	const std::string &url() const override { return url_; }
	const std::string &mime_type() const override { return mime_; }

private:
	std::string url_;
	std::string mime_;
	std::vector<uint8_t> data_;
	size_t offset_ = 0;
};

} // namespace tuxedo
