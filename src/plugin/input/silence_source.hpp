#pragma once

#include "plugin/source.hpp"

#include <cstdint>
#include <string>

namespace tuxedo {

class SilenceSource : public Source {
public:
	bool open(const std::string &url) override;
	void close() override;

	bool seekable() const override { return true; }
	bool seek(int64_t offset, int whence) override;
	int64_t tell() const override { return position_; }

	int64_t read(void *buffer, size_t amount) override;

	const std::string &url() const override { return url_; }
	const std::string &mime_type() const override { return mime_type_; }

	static std::vector<std::string> schemes() { return {"silence"}; }

private:
	std::string url_;
	std::string mime_type_ = "audio/x-silence";
	int64_t position_ = 0;
};

} // namespace tuxedo
