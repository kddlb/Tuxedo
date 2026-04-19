// This TU is where miniaudio is instantiated.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "output/miniaudio_backend.hpp"

namespace tuxedo {

struct MiniaudioBackend::Impl {
	ma_device device{};
	bool inited = false;
	bool started = false;
	RenderCallback cb;
};

namespace {
void data_cb(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
	(void)pInput;
	auto *impl = static_cast<MiniaudioBackend::Impl *>(pDevice->pUserData);
	if(impl && impl->cb) impl->cb(static_cast<float *>(pOutput), frameCount);
}
} // namespace

MiniaudioBackend::MiniaudioBackend() : impl_(new Impl) {}

MiniaudioBackend::~MiniaudioBackend() {
	close();
	delete impl_;
}

bool MiniaudioBackend::open(StreamFormat format, RenderCallback cb) {
	close();
	impl_->cb = std::move(cb);

	ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
	cfg.playback.format = ma_format_f32;
	cfg.playback.channels = format.channels;
	cfg.sampleRate = format.sample_rate;
	cfg.dataCallback = data_cb;
	cfg.pUserData = impl_;

	if(ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
		impl_->cb = nullptr;
		return false;
	}
	impl_->inited = true;
	return true;
}

void MiniaudioBackend::close() {
	if(impl_->started) {
		ma_device_stop(&impl_->device);
		impl_->started = false;
	}
	if(impl_->inited) {
		ma_device_uninit(&impl_->device);
		impl_->inited = false;
	}
	impl_->cb = nullptr;
}

void MiniaudioBackend::start() {
	if(impl_->inited && !impl_->started) {
		if(ma_device_start(&impl_->device) == MA_SUCCESS) impl_->started = true;
	}
}

void MiniaudioBackend::stop() {
	if(impl_->started) {
		ma_device_stop(&impl_->device);
		impl_->started = false;
	}
}

} // namespace tuxedo
