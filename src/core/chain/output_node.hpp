// OutputNode: pulls chunks from the previous node and hands them to an
// OutputBackend for device playback. Unlike Cog's OutputNode it is not
// threaded itself — the backend calls us from its own audio callback.
#pragma once

#include "core/audio_chunk.hpp"
#include "core/chain/node.hpp"
#include "plugin/output_backend.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace tuxedo {

class OutputNode {
public:
	OutputNode();
	~OutputNode();

	void set_previous(Node *p) { previous_.store(p); }
	Node *previous() const { return previous_.load(); }

	// Arm the hot-swap slot: when `previous_` reaches end-of-stream and
	// drains, the render callback atomically takes this pointer and makes
	// it the new `previous_`, so track transitions are gapless. Safe to
	// call at any time from any thread.
	void set_next_source(Node *n) { next_source_.store(n); }
	Node *next_source() const { return next_source_.load(); }

	// Fires once, on the audio thread, when a track drained and no
	// next_source was armed (i.e. genuine natural end of playback). The
	// callback must be cheap and non-blocking — it's meant to wake a
	// watchdog thread via condvar, not do real work.
	void set_on_stream_consumed(std::function<void()> cb);

	// Fires on the audio thread the instant a hot-swap happens —
	// `previous_` went from chain A's tail to chain B's head. Also cheap.
	void set_on_stream_advanced(std::function<void()> cb);

	bool open(StreamFormat format);
	void close();

	void start();
	void pause();
	void resume();
	bool is_paused() const { return paused_.load(); }

	void set_volume(double v) { volume_.store(v); }
	double volume() const { return volume_.load(); }

	// Absolute playback position, in frames, at the output device.
	// Advances as render() emits samples; `set_position_frames` resets
	// the counter (e.g. after a seek).
	int64_t frames_played() const { return frames_played_.load(); }
	void set_position_frames(int64_t f) { frames_played_.store(f); }
	double seconds_played() const;

	StreamFormat format() const { return format_; }

	// Drop any partial chunk the render callback is holding back. Called
	// alongside a seek so the next render() can only emit post-seek audio.
	void flush_leftover();

	// Called by the backend on its audio thread.
	void render(float *dst, size_t frames);

private:
	std::atomic<Node *> previous_{nullptr};
	std::atomic<Node *> next_source_{nullptr};
	StreamFormat format_{};
	std::unique_ptr<OutputBackend> backend_;

	std::atomic<bool> paused_{false};
	std::atomic<double> volume_{1.0};
	std::atomic<int64_t> frames_played_{0};

	// Holds the tail of a chunk we couldn't fully push in the last render.
	std::mutex leftover_mtx_;
	AudioChunk leftover_;

	// Guarded by callbacks_mtx_; set only from non-audio threads.
	std::mutex callbacks_mtx_;
	std::function<void()> on_stream_consumed_;
	std::function<void()> on_stream_advanced_;
};

} // namespace tuxedo
