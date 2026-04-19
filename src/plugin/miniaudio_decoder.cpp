#include "plugin/miniaudio_decoder.hpp"

#include "miniaudio.h"

#include <cstring>

namespace tuxedo {

struct MiniaudioDecoder::Impl {
	ma_decoder dec{};
	bool inited = false;
	Source *src = nullptr;
};

namespace {

ma_result read_cb(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead,
                  size_t *pBytesRead) {
	auto *src = static_cast<Source *>(pDecoder->pUserData);
	int64_t n = src->read(pBufferOut, bytesToRead);
	if(n < 0) {
		*pBytesRead = 0;
		return MA_ERROR;
	}
	*pBytesRead = static_cast<size_t>(n);
	return n == 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result seek_cb(ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin) {
	auto *src = static_cast<Source *>(pDecoder->pUserData);
	int whence = SEEK_SET;
	switch(origin) {
		case ma_seek_origin_start: whence = SEEK_SET; break;
		case ma_seek_origin_current: whence = SEEK_CUR; break;
		case ma_seek_origin_end: whence = SEEK_END; break;
	}
	return src->seek(static_cast<int64_t>(byteOffset), whence) ? MA_SUCCESS : MA_ERROR;
}

} // namespace

MiniaudioDecoder::MiniaudioDecoder() : impl_(new Impl) {}

MiniaudioDecoder::~MiniaudioDecoder() {
	close();
	delete impl_;
}

bool MiniaudioDecoder::open(Source *source) {
	close();
	impl_->src = source;

	ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
	// format=f32 / channels=0 / sample_rate=0 means: convert to float
	// but keep the source's native channel count and sample rate.

	ma_result r = ma_decoder_init(read_cb, seek_cb, source, &cfg, &impl_->dec);
	if(r != MA_SUCCESS) {
		impl_->src = nullptr;
		return false;
	}
	impl_->inited = true;

	props_.format.sample_rate = impl_->dec.outputSampleRate;
	props_.format.channels = impl_->dec.outputChannels;
	ma_uint64 total = 0;
	if(ma_decoder_get_length_in_pcm_frames(&impl_->dec, &total) == MA_SUCCESS) {
		props_.total_frames = static_cast<int64_t>(total);
	} else {
		props_.total_frames = -1;
	}
	return true;
}

void MiniaudioDecoder::close() {
	if(impl_ && impl_->inited) {
		ma_decoder_uninit(&impl_->dec);
		impl_->inited = false;
	}
	if(impl_) impl_->src = nullptr;
	props_ = {};
}

bool MiniaudioDecoder::read(AudioChunk &out, size_t max_frames) {
	if(!impl_->inited || max_frames == 0) return false;

	const uint32_t ch = props_.format.channels;
	std::vector<float> buf(max_frames * ch);

	ma_uint64 frames_read = 0;
	ma_result r = ma_decoder_read_pcm_frames(&impl_->dec, buf.data(), max_frames, &frames_read);
	if(frames_read == 0) return false;

	buf.resize(frames_read * ch);
	out = AudioChunk(props_.format, std::move(buf));
	return r == MA_SUCCESS || r == MA_AT_END;
}

int64_t MiniaudioDecoder::seek(int64_t frame) {
	if(!impl_->inited) return -1;
	if(ma_decoder_seek_to_pcm_frame(&impl_->dec, static_cast<ma_uint64>(frame)) != MA_SUCCESS) {
		return -1;
	}
	return frame;
}

} // namespace tuxedo
