// ConverterNode: sits between InputNode and OutputNode, resampling and
// remixing chunks to a target StreamFormat so gapless playback across
// mismatched sample rates / channel counts doesn't require reopening
// the audio device. Backed by miniaudio's ma_data_converter.
#pragma once

#include "core/chain/node.hpp"

#include <atomic>
#include <optional>

namespace tuxedo {

class ConverterNode : public Node {
public:
	ConverterNode();
	~ConverterNode() override;

	// Configure the target output format. Pass std::nullopt (the default)
	// to run as an identity passthrough. Must be called before launch();
	// retargeting a running converter is not supported.
	void set_target_format(std::optional<StreamFormat> target);

	// Arm a flush: on the next process-loop iteration the resampler's
	// internal state is reset and the output ring buffer is dropped.
	// Call alongside a seek on the upstream InputNode.
	void request_flush();

	void process() override;

private:
	std::optional<StreamFormat> target_;
	std::atomic<bool> flush_requested_{false};
	// void * because ma_data_converter is an opaque struct from
	// miniaudio.h and we don't want that header leaking through here.
	void *converter_ = nullptr;
};

} // namespace tuxedo
