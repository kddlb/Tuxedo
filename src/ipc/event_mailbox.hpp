// EventMailbox: thread-safe bounded queue of pre-serialised event
// lines, sized for a single SSE subscriber. Producers (Controller
// event callbacks) push; the SSE handler pops with a timeout and
// streams each message as a `data: …\n\n` SSE frame.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace tuxedo {

class EventMailbox {
public:
	explicit EventMailbox(size_t capacity = 64) : capacity_(capacity) {}

	EventMailbox(const EventMailbox &) = delete;
	EventMailbox &operator=(const EventMailbox &) = delete;

	void push(std::string msg) {
		std::lock_guard<std::mutex> g(mtx_);
		if(closed_) return;
		if(q_.size() >= capacity_) q_.pop_front(); // drop oldest under pressure
		q_.push_back(std::move(msg));
		cv_.notify_one();
	}

	// Waits up to `timeout` for a message. Returns true if `out` was
	// populated, false on timeout or close.
	template <class Rep, class Period>
	bool pop_for(std::string &out, std::chrono::duration<Rep, Period> timeout) {
		std::unique_lock<std::mutex> lk(mtx_);
		const bool got = cv_.wait_for(lk, timeout, [this] {
			return !q_.empty() || closed_;
		});
		if(!got || q_.empty()) return false;
		out = std::move(q_.front());
		q_.pop_front();
		return true;
	}

	void close() {
		std::lock_guard<std::mutex> g(mtx_);
		closed_ = true;
		cv_.notify_all();
	}

	bool closed() const {
		std::lock_guard<std::mutex> g(mtx_);
		return closed_;
	}

private:
	mutable std::mutex mtx_;
	std::condition_variable cv_;
	std::deque<std::string> q_;
	size_t capacity_;
	bool closed_ = false;
};

} // namespace tuxedo
