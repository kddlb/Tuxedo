// OutputNode: pulls chunks from the previous node and hands them to an
// OutputBackend for device playback. Unlike Cog's OutputNode it is not
// threaded itself — the backend calls us from its own audio callback.
#pragma once

#include "core/audio_chunk.hpp"
#include "core/chain/node.hpp"
#include "output/backend.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace tuxedo {

class OutputNode {
public:
	OutputNode();
	~OutputNode();

	void set_previous(Node *p) { previous_ = p; }

	bool open(StreamFormat format);
	void close();

	void start();
	void pause();
	void resume();
	bool is_paused() const { return paused_.load(); }

	void set_volume(double v) { volume_.store(v); }
	double volume() const { return volume_.load(); }

	// Frames played since open(); used to derive the position clock.
	int64_t frames_played() const { return frames_played_.load(); }
	double seconds_played() const;

	StreamFormat format() const { return format_; }

	// Called by the backend on its audio thread.
	void render(float *dst, size_t frames);

private:
	Node *previous_ = nullptr;
	StreamFormat format_{};
	std::unique_ptr<OutputBackend> backend_;

	std::atomic<bool> paused_{false};
	std::atomic<double> volume_{1.0};
	std::atomic<int64_t> frames_played_{0};

	// Holds the tail of a chunk we couldn't fully push in the last render.
	std::mutex leftover_mtx_;
	AudioChunk leftover_;
};

} // namespace tuxedo
