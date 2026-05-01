// Plugin registry. Replaces Cog's NSBundle-based PluginController.mm
// with a simple static-registration model (compiled-in plugins only).
#pragma once

#include "plugin/decoder.hpp"
#include "plugin/source.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tuxedo {

using SourceFactory = std::function<SourcePtr()>;
using DecoderFactory = std::function<DecoderPtr()>;

class PluginRegistry {
public:
	static PluginRegistry &instance();

	void register_source(const std::string &scheme, SourceFactory f);
	void register_decoder(const std::string &ext, DecoderFactory f);
	void register_fallback_decoder(DecoderFactory f);

	SourcePtr source_for_url(const std::string &url);
	DecoderPtr decoder_for_extension(const std::string &ext);
	std::vector<DecoderPtr> fallback_decoders();

	static std::string extension_of(const std::string &path);
	static std::string scheme_of(const std::string &url);

private:
	PluginRegistry() = default;
	std::unordered_map<std::string, SourceFactory> sources_;
	std::unordered_map<std::string, DecoderFactory> decoders_;
	std::vector<DecoderFactory> fallback_decoders_;
};

// Register the built-in MVP plugins (file source + miniaudio decoder).
void register_builtin_plugins();

} // namespace tuxedo
