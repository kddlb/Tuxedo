// Source: byte-stream I/O interface. Mirrors Cog's CogSource protocol.
// Decoders read through a Source rather than directly from the
// filesystem, so we can plug in http/smb/archive sources later without
// touching the decoders.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tuxedo {

class Source {
public:
	using MetadataChangedCallback = std::function<void()>;

	virtual ~Source() = default;

	virtual bool open(const std::string &url) = 0;
	virtual void close() = 0;

	virtual bool seekable() const = 0;
	// whence uses SEEK_SET / SEEK_CUR / SEEK_END semantics.
	virtual bool seek(int64_t offset, int whence) = 0;
	virtual int64_t tell() const = 0;

	// Reads up to `amount` bytes; returns bytes actually read. 0 at EOF, <0 on error.
	virtual int64_t read(void *buffer, size_t amount) = 0;

	virtual const std::string &url() const = 0;
	virtual const std::string &mime_type() const = 0;
	virtual nlohmann::json metadata() const { return nlohmann::json::object(); }
	virtual void set_metadata_changed_callback(MetadataChangedCallback cb) { (void)cb; }

	// Schemes this source handles (e.g. "file", "http").
	static std::vector<std::string> schemes() { return {}; }
};

using SourcePtr = std::unique_ptr<Source>;

} // namespace tuxedo
