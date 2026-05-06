#pragma once

#include "plugin/output_backend.hpp"

namespace tuxedo {

class MiniaudioBackend : public OutputBackend {
public:
	MiniaudioBackend();
	~MiniaudioBackend() override;

	bool open(StreamFormat format, RenderCallback cb) override;
	void close() override;

	void start() override;
	void stop() override;
	void pause() override;
	void resume() override;

	struct Impl;

private:
	Impl *impl_ = nullptr;
};

} // namespace tuxedo
