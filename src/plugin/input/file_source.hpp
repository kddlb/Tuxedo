#pragma once

#include "plugin/source.hpp"

#include <cstdio>
#include <string>

namespace tuxedo {

class FileSource : public Source {
public:
	~FileSource() override;

	bool open(const std::string &url) override;
	void close() override;

	bool seekable() const override { return fp_ != nullptr; }
	bool seek(int64_t offset, int whence) override;
	int64_t tell() const override;

	int64_t read(void *buffer, size_t amount) override;

	const std::string &url() const override { return url_; }
	const std::string &mime_type() const override { return mime_; }

	static std::vector<std::string> schemes() { return {"file"}; }

private:
	std::FILE *fp_ = nullptr;
	std::string url_;
	std::string mime_;
};

} // namespace tuxedo
