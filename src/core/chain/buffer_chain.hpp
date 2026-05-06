// BufferChain: owns an InputNode and a ConverterNode that adapts its
// output to whatever format the OutputNode currently wants. Mirrors
// Cog's BufferChain. The converter runs in identity passthrough when
// input and target formats match, so the same-format fast path stays
// free of resampling overhead.
#pragma once

#include "core/chain/converter_node.hpp"
#include "core/chain/dsp_fader_node.hpp"
#include "core/chain/input_node.hpp"

#include <memory>
#include <optional>
#include <string>

namespace tuxedo {

class BufferChain {
public:
	BufferChain();
	~BufferChain();

	bool open(const std::string &url);
	void close();

	// Configure the converter's target format. Pass std::nullopt (the
	// default) to keep it in identity passthrough at the input's native
	// format. Must be called before launch().
	void retarget(std::optional<StreamFormat> target);

	// Orchestrate a seek across both input and converter: clears both
	// ring buffers, resets the resampler's internal state, and tells the
	// input decoder where to resume.
	void seek(int64_t frame);
	void set_gain(float gain);

	void launch();
	void request_stop();
	bool launched() const { return launched_; }

	InputNode *input() { return input_.get(); }
	const InputNode *input() const { return input_.get(); }
	DSPFaderNode *fader() { return fader_.get(); }
	const DSPFaderNode *fader() const { return fader_.get(); }
	Node *final_node() { return fader_ ? static_cast<Node *>(fader_.get()) : converter_.get(); }

	StreamFormat format() const { return format_; }
	const std::string &url() const { return url_; }

private:
	std::unique_ptr<InputNode> input_;
	std::unique_ptr<ConverterNode> converter_;
	std::unique_ptr<DSPFaderNode> fader_;
	StreamFormat format_{};
	std::string url_;
	bool launched_ = false;
};

} // namespace tuxedo
