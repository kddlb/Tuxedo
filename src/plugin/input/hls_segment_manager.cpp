#include "plugin/input/hls_segment_manager.hpp"

#include "plugin/input/hls_memory_source.hpp"
#include "plugin/input/http_source.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace tuxedo {

namespace {

// Idle sleep when the buffer is full or no live refresh is due.
constexpr auto kFetchIdleSleep = std::chrono::milliseconds(50);

// After this many consecutive failures, live streams skip the offending
// segment so playback can keep up. VOD never skips — it retries
// indefinitely until the user stops playback.
constexpr int kMaxConsecutiveFailures = 5;

constexpr size_t kSegmentReadChunk = 64 * 1024;

} // namespace

HlsSegmentManager::HlsSegmentManager(HlsPlaylist playlist)
    : playlist_(std::move(playlist)) {
	for(const HlsSegment &seg : playlist_.segments) {
		seen_sequence_numbers_.insert(seg.sequence_number);
	}
}

HlsSegmentManager::~HlsSegmentManager() { stop(); }

double HlsSegmentManager::total_duration() const {
	if(playlist_.is_live) return 0.0;
	double total = 0.0;
	for(const HlsSegment &seg : playlist_.segments) total += seg.duration;
	return total;
}

std::string HlsSegmentManager::last_observed_mime_type() const {
	std::lock_guard<std::mutex> g(mime_mtx_);
	return last_observed_mime_;
}

bool HlsSegmentManager::download_url(const std::string &url,
                                     std::vector<uint8_t> &out,
                                     std::string &mime,
                                     std::string &error) {
	HttpSource src;
	if(!src.open(url)) {
		error = "Failed to open " + url;
		return false;
	}
	mime = src.mime_type();

	out.clear();
	std::vector<uint8_t> buf(kSegmentReadChunk);
	for(;;) {
		int64_t got = src.read(buf.data(), buf.size());
		if(got < 0) {
			error = "Read error from " + url;
			src.close();
			return false;
		}
		if(got == 0) break;
		out.insert(out.end(), buf.data(), buf.data() + got);
	}
	src.close();

	if(out.empty()) {
		error = "Empty segment from " + url;
		return false;
	}
	return true;
}

bool HlsSegmentManager::download_segment_at_index(size_t index,
                                                  std::vector<uint8_t> &out,
                                                  std::string &error) {
	std::string seg_url;
	{
		std::lock_guard<std::mutex> g(mtx_);
		if(index >= playlist_.segments.size()) {
			error = "Segment index out of range";
			return false;
		}
		seg_url = playlist_.segments[index].url;
	}
	if(seg_url.empty()) {
		error = "Segment has no URL";
		return false;
	}

	std::string mime;
	if(!download_url(seg_url, out, mime, error)) return false;

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(index < playlist_.segments.size()) {
			playlist_.segments[index].mime_type = mime;
		}
	}
	if(!mime.empty()) {
		std::lock_guard<std::mutex> g(mime_mtx_);
		last_observed_mime_ = mime;
	}
	return true;
}

void HlsSegmentManager::start_fetching_from(size_t index) {
	bool need_spawn = false;
	{
		std::lock_guard<std::mutex> g(mtx_);
		next_fetch_index_ = index;
		stop_requested_ = false;
		last_refresh_ = std::chrono::steady_clock::now();
		need_spawn = !running_;
		running_ = true;
		cv_.notify_all();
	}

	if(need_spawn) {
		worker_ = std::thread([this] { fetch_loop(); });
	}
}

void HlsSegmentManager::stop() {
	{
		std::lock_guard<std::mutex> g(mtx_);
		stop_requested_ = true;
		cv_.notify_all();
	}
	if(worker_.joinable()) worker_.join();
	std::lock_guard<std::mutex> g(mtx_);
	running_ = false;
}

bool HlsSegmentManager::refresh_due() const {
	auto interval_secs = std::max(static_cast<double>(playlist_.target_duration) / 2.0, 1.0);
	auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
	    std::chrono::duration<double>(interval_secs));
	return std::chrono::steady_clock::now() - last_refresh_ >= interval;
}

void HlsSegmentManager::refresh_live_playlist() {
	last_refresh_ = std::chrono::steady_clock::now();

	std::string playlist_url;
	{
		std::lock_guard<std::mutex> g(mtx_);
		playlist_url = playlist_.url;
	}

	std::vector<uint8_t> bytes;
	std::string mime;
	std::string error;
	if(!download_url(playlist_url, bytes, mime, error)) return;

	std::string text(bytes.begin(), bytes.end());
	HlsPlaylist fresh;
	std::string parse_err;
	if(!parse_hls_playlist(text, playlist_url, fresh, parse_err)) return;

	std::lock_guard<std::mutex> g(mtx_);
	if(fresh.has_endlist) {
		playlist_.is_live = false;
		playlist_.has_endlist = true;
	}

	for(HlsSegment &seg : fresh.segments) {
		if(seen_sequence_numbers_.count(seg.sequence_number)) continue;
		seen_sequence_numbers_.insert(seg.sequence_number);
		playlist_.segments.push_back(std::move(seg));
	}
}

void HlsSegmentManager::handle_fetch_failure() {
	consecutive_failures_++;

	// Progressive backoff: 0.5s, 1s, 2s, 4s, 8s, then cap at 8s.
	int shift = std::min(consecutive_failures_ - 1, 4);
	double backoff = 0.5 * static_cast<double>(1 << shift);
	if(backoff > 8.0) backoff = 8.0;
	std::this_thread::sleep_for(std::chrono::duration<double>(backoff));

	if(consecutive_failures_ >= kMaxConsecutiveFailures) {
		std::lock_guard<std::mutex> g(mtx_);
		if(playlist_.is_live) {
			// Live: skip past the bad segment so we keep up with the
			// broadcast. VOD: keep retrying forever — the user controls
			// when to give up by calling stop.
			next_fetch_index_++;
			consecutive_failures_ = 0;
		}
	}
}

void HlsSegmentManager::fetch_loop() {
	for(;;) {
		{
			std::lock_guard<std::mutex> g(mtx_);
			if(stop_requested_) break;
		}

		bool did_work = false;

		if(playlist_.is_live && refresh_due()) {
			refresh_live_playlist();
			did_work = true;
		}

		HlsMemorySource *mem = memory_source_;
		size_t buffered = mem ? mem->buffered_segment_count() : 0;
		if(mem && buffered < buffer_size_) {
			size_t fetch_index = 0;
			bool have_index = false;
			{
				std::lock_guard<std::mutex> g(mtx_);
				if(next_fetch_index_ < playlist_.segments.size()) {
					fetch_index = next_fetch_index_;
					have_index = true;
				}
			}

			if(have_index) {
				std::vector<uint8_t> data;
				std::string error;
				if(download_segment_at_index(fetch_index, data, error)) {
					mem->append_data(data.data(), data.size());
					{
						std::lock_guard<std::mutex> g(mtx_);
						next_fetch_index_++;
					}
					consecutive_failures_ = 0;
					did_work = true;
				} else {
					handle_fetch_failure();
				}
			} else if(!playlist_.is_live) {
				// VOD playlist exhausted. Tell the consumer no more bytes
				// will arrive so it can drain cleanly.
				mem->mark_end_of_stream();
				break;
			}
		}

		if(!did_work) std::this_thread::sleep_for(kFetchIdleSleep);
	}

	std::lock_guard<std::mutex> g(mtx_);
	running_ = false;
}

} // namespace tuxedo
