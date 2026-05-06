// Node: base class for a chunk-producing stage, with its own worker
// thread and a bounded queue of AudioChunks for the next stage to read.
// Mirrors Cog's Audio/Chain/Node but without the Cocoa / semaphore
// machinery — std::thread + condition_variable do the same job.
#pragma once

#include "core/audio_chunk.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace tuxedo {

class Node {
public:
	static constexpr size_t kMaxBufferedFrames = 4 * 1024 * 1024 / sizeof(float);
	static constexpr float kMaxBufferedSeconds = 10.0;

	explicit Node(Node *previous = nullptr);
	virtual ~Node();

	Node(const Node &) = delete;
	Node &operator=(const Node &) = delete;

	// Producers call this. Override in subclasses.
	virtual void process() = 0;

	// Start/stop the worker thread.
	void launch();
	void request_stop();
	void join();

	bool should_continue() const { return should_continue_.load(); }
	bool end_of_stream() const { return end_of_stream_.load(); }
	void set_end_of_stream(bool b) { end_of_stream_.store(b); }

	Node *previous() const { return previous_; }
	void set_previous(Node *p) { previous_ = p; }

	// Producer side: push a chunk into our output buffer. Blocks if full.
	void write_chunk(AudioChunk chunk);

	// Consumer side: pop up to max_frames frames of data from our buffer.
	// Returns an empty chunk at end-of-stream.
	AudioChunk read_chunk(size_t max_frames);

	void wait_until_buffered_frames_at_most(size_t max_frames);
	void wait_until_buffered_frames_at_least(size_t min_frames);

	// Drops all buffered chunks. Intended for seek — unblocks any waiter
	// in write_chunk so the producer can loop around and produce fresh,
	// post-seek audio.
	void flush_buffer();

	// Format declared by the chunks in our buffer (first one wins).
	bool peek_format(StreamFormat &out);

	size_t frames_buffered();
	double seconds_buffered();

protected:
	Node *previous_ = nullptr;
	std::atomic<bool> should_continue_{true};
	std::atomic<bool> end_of_stream_{false};

	virtual size_t max_buffered_frames() const { return kMaxBufferedFrames; }
	virtual double max_buffered_seconds() const { return kMaxBufferedSeconds; }

private:
	void thread_entry();

	std::thread worker_;
	std::mutex mtx_;
	std::condition_variable not_full_;
	std::condition_variable not_empty_;
	std::deque<AudioChunk> buffer_;
	size_t buffered_frames_ = 0;
	double buffered_seconds_ = 0;
};

} // namespace tuxedo
