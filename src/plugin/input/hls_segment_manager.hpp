// Background HLS segment fetcher. Owns a worker thread that downloads
// segments via HttpSource and pushes their bytes into an
// HlsMemorySource, applying backpressure when the memory queue is
// full. For live streams it also re-fetches the playlist on
// targetDuration/2 second intervals and splices new segments onto the
// end. Ported from Cog's HLSSegmentManager.
#pragma once

#include "plugin/input/hls_playlist.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace tuxedo {

class HlsMemorySource;

class HlsSegmentManager {
public:
	explicit HlsSegmentManager(HlsPlaylist playlist);
	~HlsSegmentManager();

	void set_memory_source(HlsMemorySource *source) { memory_source_ = source; }
	void set_buffer_size(size_t segments) { buffer_size_ = segments; }

	// Synchronous fetch. Returns true on success and fills `out` with
	// the segment bytes. Updates last_observed_mime_type_.
	bool download_segment_at_index(size_t index,
	                               std::vector<uint8_t> &out,
	                               std::string &error);

	// Start the background fetcher beginning at the given playlist
	// index. If already running, just updates the cursor.
	void start_fetching_from(size_t index);

	// Stop the background fetcher and wait for the worker to exit.
	void stop();

	double total_duration() const;
	std::string last_observed_mime_type() const;

	// VOD playlist? Convenience wrapper.
	bool is_live() const { return playlist_.is_live; }

	const HlsPlaylist &playlist() const { return playlist_; }

private:
	void fetch_loop();
	bool refresh_due() const;
	void refresh_live_playlist();
	void handle_fetch_failure();

	bool download_url(const std::string &url,
	                  std::vector<uint8_t> &out,
	                  std::string &mime,
	                  std::string &error);

	HlsPlaylist playlist_;
	HlsMemorySource *memory_source_ = nullptr;

	mutable std::mutex mtx_;
	std::condition_variable cv_;
	std::thread worker_;
	bool running_ = false;
	bool stop_requested_ = false;
	size_t next_fetch_index_ = 0;
	size_t buffer_size_ = 5;
	std::chrono::steady_clock::time_point last_refresh_;

	std::unordered_set<int64_t> seen_sequence_numbers_;

	mutable std::mutex mime_mtx_;
	std::string last_observed_mime_;

	// Touched only from the worker thread.
	int consecutive_failures_ = 0;
};

} // namespace tuxedo
