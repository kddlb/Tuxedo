#include "core/chain/node.hpp"

namespace tuxedo {

Node::Node(Node *previous) : previous_(previous) {}

Node::~Node() {
	request_stop();
	join();
}

void Node::launch() {
	if(worker_.joinable()) return;
	worker_ = std::thread([this] { thread_entry(); });
}

void Node::request_stop() {
	should_continue_.store(false);
	std::lock_guard<std::mutex> g(mtx_);
	not_full_.notify_all();
	not_empty_.notify_all();
}

void Node::join() {
	if(worker_.joinable()) worker_.join();
}

void Node::thread_entry() {
	process();
	set_end_of_stream(true);
	std::lock_guard<std::mutex> g(mtx_);
	not_empty_.notify_all();
}

void Node::write_chunk(AudioChunk chunk) {
	if(chunk.empty()) return;

	std::unique_lock<std::mutex> lk(mtx_);
	not_full_.wait(lk, [this] {
		return !should_continue_.load() || (buffered_frames_ < max_buffered_frames() && buffered_seconds_ < max_buffered_seconds());
	});
	if(!should_continue_.load()) return;

	buffered_frames_ += chunk.frame_count();
	buffered_seconds_ += chunk.duration();
	buffer_.push_back(std::move(chunk));
	not_empty_.notify_one();
}

AudioChunk Node::read_chunk(size_t max_frames) {
	std::unique_lock<std::mutex> lk(mtx_);
	not_empty_.wait(lk, [this] {
		return !buffer_.empty() || end_of_stream_.load() || !should_continue_.load();
	});
	if(buffer_.empty()) return {};

	AudioChunk &front = buffer_.front();
	AudioChunk out;
	if(front.frame_count() <= max_frames) {
		out = std::move(front);
		buffer_.pop_front();
	} else {
		out = front.remove_frames(max_frames);
	}
	buffered_frames_ -= out.frame_count();
	buffered_seconds_ -= out.duration();
	not_full_.notify_one();
	return out;
}

void Node::wait_until_buffered_frames_at_most(size_t max_frames) {
	std::unique_lock<std::mutex> lk(mtx_);
	not_full_.wait(lk, [this, max_frames] {
		return !should_continue_.load() || buffered_frames_ <= max_frames;
	});
}

void Node::wait_until_buffered_frames_at_least(size_t min_frames) {
	std::unique_lock<std::mutex> lk(mtx_);
	not_empty_.wait(lk, [this, min_frames] {
		return !should_continue_.load() || buffered_frames_ >= min_frames || end_of_stream_.load();
	});
}

void Node::flush_buffer() {
	std::lock_guard<std::mutex> g(mtx_);
	buffer_.clear();
	buffered_frames_ = 0;
	buffered_seconds_ = 0;
	not_full_.notify_all();
	not_empty_.notify_all();
}

bool Node::peek_format(StreamFormat &out) {
	std::unique_lock<std::mutex> lk(mtx_);
	not_empty_.wait(lk, [this] {
		return buffer_.size() || end_of_stream_.load() || !should_continue_.load();
	});
	if(!buffer_.size()) return false;
	out = buffer_.front().format();
	return true;
}

size_t Node::frames_buffered() {
	std::lock_guard<std::mutex> g(mtx_);
	return buffered_frames_;
}

double Node::seconds_buffered() {
	std::lock_guard<std::mutex> g(mtx_);
	return buffered_seconds_;
}

} // namespace tuxedo
