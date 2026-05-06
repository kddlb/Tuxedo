#include "core/player.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace tuxedo {

namespace {

nlohmann::json metadata_from_chain(const BufferChain *c) {
	if(!c || !c->input() || !c->input()->decoder())
		return nlohmann::json::object();
	return c->input()->decoder()->metadata();
}

double duration_of_chain(const BufferChain *c) {
	if(!c) return 0.0;
	auto fmt = c->format();
	int64_t total = c->input() ? c->input()->properties().total_frames : -1;
	if(total < 0 || !fmt.valid()) return 0.0;
	return double(total) / fmt.sample_rate;
}

nlohmann::json metadata_from_chain_unlocked(const BufferChain *c) {
	if(!c || !c->input() || !c->input()->decoder())
		return nlohmann::json::object();
	return c->input()->decoder()->metadata();
}

template <typename T>
void shuffle_container(std::deque<T> &items, std::mt19937 &rng) {
	std::vector<T> tmp;
	tmp.reserve(items.size());
	while(!items.empty()) {
		tmp.push_back(std::move(items.front()));
		items.pop_front();
	}
	std::shuffle(tmp.begin(), tmp.end(), rng);
	for(auto &item : tmp) items.push_back(std::move(item));
}

} // namespace

const char *shuffle_mode_name(ShuffleMode mode) {
	switch(mode) {
		case ShuffleMode::Off: return "off";
		case ShuffleMode::All: return "all";
	}
	return "off";
}

const char *repeat_mode_name(RepeatMode mode) {
	switch(mode) {
		case RepeatMode::Off: return "off";
		case RepeatMode::One: return "one";
		case RepeatMode::All: return "all";
	}
	return "off";
}

std::optional<ShuffleMode> shuffle_mode_from_string(const std::string &mode) {
	if(mode == "off") return ShuffleMode::Off;
	if(mode == "all") return ShuffleMode::All;
	return std::nullopt;
}

std::optional<RepeatMode> repeat_mode_from_string(const std::string &mode) {
	if(mode == "off") return RepeatMode::Off;
	if(mode == "one") return RepeatMode::One;
	if(mode == "all") return RepeatMode::All;
	return std::nullopt;
}

Player::Player() { watchdog_ = std::thread([this] { watchdog_loop(); }); }

Player::~Player() {
	{
		std::lock_guard<std::mutex> g(watchdog_mtx_);
		watchdog_stop_ = true;
		watchdog_cv_.notify_all();
	}
	if(watchdog_.joinable()) watchdog_.join();
	teardown();
}

void Player::set_event_callback(PlayerEventCallback cb) {
	std::lock_guard<std::mutex> g(mtx_);
	cb_ = std::move(cb);
}

bool Player::play(const std::string &url, bool from_playlist) {
	teardown();

	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(url)) {
		emit({PlayerEvent::Kind::Error, PlaybackStatus::Stopped, url, "open failed", {}});
		return false;
	}

	PlaylistItem item;
	item.url = url;
	item.format = chain->format();
	item.duration_seconds = duration_of_chain(chain.get());
	item.metadata = metadata_from_chain(chain.get());
	item.from_playlist = from_playlist;

	{
		std::lock_guard<std::mutex> g(mtx_);
		clear_playlist_locked();
		items_.push_back(std::move(item));
		if(!start_chain_locked(std::move(chain), 0)) {
			clear_playlist_locked();
			emit({PlayerEvent::Kind::Error, PlaybackStatus::Stopped, url, "output open failed", {}});
			return false;
		}
	}

	set_status(PlaybackStatus::Playing);

	PlayerEvent began;
	began.kind = PlayerEvent::Kind::StreamBegan;
	began.status = PlaybackStatus::Playing;
	began.url = url;
	began.metadata = current_metadata();
	emit(std::move(began));
	return true;
}

bool Player::queue(const std::string &url, bool from_playlist) {
	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(url)) {
		emit({PlayerEvent::Kind::Error, status(), url, "queue open failed", {}});
		return false;
	}

	PlaylistItem item;
	item.url = url;
	item.format = chain->format();
	item.duration_seconds = duration_of_chain(chain.get());
	item.metadata = metadata_from_chain(chain.get());
	item.from_playlist = from_playlist;

	bool needs_start = false;
	{
		std::lock_guard<std::mutex> g(mtx_);
		size_t index = items_.size();
		items_.push_back(std::move(item));
		if(!chain_) {
			needs_start = start_chain_locked(std::move(chain), index);
		} else {
			if(shuffle_mode_ == ShuffleMode::All && !queue_.empty()) {
				std::uniform_int_distribution<size_t> dist(0, queue_.size());
				insert_upcoming_locked(index, std::move(chain), dist(rng_));
			} else {
				insert_upcoming_locked(index, std::move(chain));
			}
			maybe_arm_next_locked();
		}
	}

	if(needs_start) {
		set_status(PlaybackStatus::Playing);
		PlayerEvent began;
		began.kind = PlayerEvent::Kind::StreamBegan;
		began.status = PlaybackStatus::Playing;
		began.url = current_url();
		began.metadata = current_metadata();
		emit(std::move(began));
	}
	return true;
}

void Player::queue_clear() {
	std::deque<std::unique_ptr<BufferChain>> victims;
	{
		std::lock_guard<std::mutex> g(mtx_);
		victims = std::move(queue_);
		upcoming_indices_.clear();
		if(output_) output_->set_next_source(nullptr);
	}
	for(auto &c : victims) if(c) c->close();
}

bool Player::previous() {
	std::unique_ptr<BufferChain> old_chain;
	std::string old_url;
	std::string new_url;
	nlohmann::json new_meta;

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!chain_ || !current_index_) return false;

		if(output_ && output_->seconds_played() > 5.0) {
			auto fmt = output_->format();
			chain_->seek(0);
			output_->flush_leftover();
			if(fmt.valid()) output_->set_position_frames(0);
			return true;
		}

		if(history_indices_.empty()) {
			auto fmt = output_ ? output_->format() : StreamFormat{};
			chain_->seek(0);
			if(output_) {
				output_->flush_leftover();
				if(fmt.valid()) output_->set_position_frames(0);
			}
			return true;
		}

		const size_t old_index = *current_index_;
		const size_t prev_index = history_indices_.back();
		history_indices_.pop_back();

		auto old_requeued = open_chain_for_item_locked(old_index);
		if(!old_requeued) return false;
		insert_upcoming_locked(old_index, std::move(old_requeued), 0);

		old_url = current_url_;
		old_chain = std::move(chain_);
		if(output_) {
			output_->close();
			output_.reset();
		}
		current_url_.clear();
		current_index_.reset();

		if(!start_index_locked(prev_index)) return false;
		new_url = current_url_;
		new_meta = metadata_from_chain_unlocked(chain_.get());
	}

	if(old_chain) old_chain->close();
	set_status(PlaybackStatus::Playing);

	PlayerEvent ended;
	ended.kind = PlayerEvent::Kind::StreamEnded;
	ended.status = PlaybackStatus::Stopped;
	ended.url = old_url;
	emit(std::move(ended));

	PlayerEvent began;
	began.kind = PlayerEvent::Kind::StreamBegan;
	began.status = PlaybackStatus::Playing;
	began.url = new_url;
	began.metadata = std::move(new_meta);
	emit(std::move(began));
	return true;
}

bool Player::skip() {
	std::unique_ptr<BufferChain> old_chain;
	std::string old_url;
	bool started = false;

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!chain_ || !current_index_) return false;
		old_url = current_url_;
		history_indices_.push_back(*current_index_);
		old_chain = std::move(chain_);
		if(output_) {
			output_->close();
			output_.reset();
		}
		current_url_.clear();
		current_index_.reset();
		if(upcoming_indices_.empty() && repeat_mode_ == RepeatMode::All) {
			prepare_repeat_all_cycle_locked();
		}
		if(!queue_.empty()) started = start_head_locked();
	}

	if(old_chain) old_chain->close();

	PlayerEvent ended;
	ended.kind = PlayerEvent::Kind::StreamEnded;
	ended.status = started ? PlaybackStatus::Playing : PlaybackStatus::Stopped;
	ended.url = old_url;
	emit(std::move(ended));

	if(started) {
		set_status(PlaybackStatus::Playing);
		PlayerEvent began;
		began.kind = PlayerEvent::Kind::StreamBegan;
		began.status = PlaybackStatus::Playing;
		began.url = current_url();
		began.metadata = current_metadata();
		emit(std::move(began));
	} else {
		set_status(PlaybackStatus::Stopped);
	}
	return true;
}

bool Player::queue_jump(size_t index) {
	std::unique_ptr<BufferChain> old_chain;
	std::string old_url;
	bool started = false;

	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!current_index_ || !chain_ || !output_) return false;
		if(index == *current_index_) {
			chain_->seek(0);
			output_->flush_leftover();
			output_->set_position_frames(0);
			return true;
		}

		old_url = current_url_;
		history_indices_.push_back(*current_index_);
		size_t distance = 0;
		bool found = false;
		for(size_t i = 0; i < upcoming_indices_.size(); ++i) {
			if(upcoming_indices_[i] == index) {
				distance = i;
				found = true;
				break;
			}
		}
		if(!found) {
			history_indices_.pop_back();
			return false;
		}
		for(size_t i = 0; i < distance; ++i) history_indices_.push_back(upcoming_indices_[i]);
		for(size_t i = 0; i < distance; ++i) {
			auto skipped = std::move(queue_.front());
			queue_.pop_front();
			upcoming_indices_.pop_front();
			if(skipped) skipped->close();
		}

		old_chain = std::move(chain_);
		if(output_) {
			output_->close();
			output_.reset();
		}
		current_url_.clear();
		current_index_.reset();
		started = start_head_locked();
	}

	if(old_chain) old_chain->close();
	if(!started) return false;

	set_status(PlaybackStatus::Playing);
	PlayerEvent ended;
	ended.kind = PlayerEvent::Kind::StreamEnded;
	ended.status = PlaybackStatus::Stopped;
	ended.url = old_url;
	emit(std::move(ended));

	PlayerEvent began;
	began.kind = PlayerEvent::Kind::StreamBegan;
	began.status = PlaybackStatus::Playing;
	began.url = current_url();
	began.metadata = current_metadata();
	emit(std::move(began));
	return true;
}

void Player::stop() {
	teardown();
	set_status(PlaybackStatus::Stopped);
}

void Player::pause() {
	std::lock_guard<std::mutex> g(mtx_);
	if(!output_ || !chain_ || status_ != PlaybackStatus::Playing) return;
	if(chain_->fader()) chain_->fader()->pause_and_wait();
	output_->pause();
	status_ = PlaybackStatus::Paused;
	if(cb_) {
		PlayerEvent e;
		e.kind = PlayerEvent::Kind::StatusChanged;
		e.status = status_;
		e.url = current_url_;
		cb_(e);
	}
}

void Player::resume() {
	std::lock_guard<std::mutex> g(mtx_);
	if(!output_ || !chain_ || status_ != PlaybackStatus::Paused) return;
	if(chain_->fader()) chain_->fader()->resume_with_fade_in();
	output_->resume();
	status_ = PlaybackStatus::Playing;
	if(cb_) {
		PlayerEvent e;
		e.kind = PlayerEvent::Kind::StatusChanged;
		e.status = status_;
		e.url = current_url_;
		cb_(e);
	}
}

bool Player::seek_seconds(double seconds) {
	std::lock_guard<std::mutex> g(mtx_);
	if(!chain_ || !output_) return false;
	auto fmt = output_->format();
	if(!fmt.valid()) return false;
	auto input_fmt = chain_->format();
	int64_t frame = static_cast<int64_t>(seconds * input_fmt.sample_rate);
	chain_->seek(frame);
	output_->flush_leftover();
	output_->set_position_frames(static_cast<int64_t>(seconds * fmt.sample_rate));
	return true;
}

void Player::set_volume(double v) {
	std::lock_guard<std::mutex> g(mtx_);
	desired_volume_ = v;
	if(output_) output_->set_volume(v);
}

void Player::set_replaygain_mode(ReplayGainMode mode) {
	std::lock_guard<std::mutex> g(mtx_);
	replaygain_mode_ = mode;
	if(chain_) apply_replaygain_locked(chain_.get());
	for(auto &queued : queue_) {
		if(queued) apply_replaygain_locked(queued.get());
	}
}

ReplayGainMode Player::replaygain_mode() const {
	std::lock_guard<std::mutex> g(mtx_);
	return replaygain_mode_;
}

void Player::set_shuffle_mode(ShuffleMode mode) {
	std::lock_guard<std::mutex> g(mtx_);
	if(shuffle_mode_ == mode) return;
	shuffle_mode_ = mode;
	if(mode == ShuffleMode::All) {
		reshuffle_upcoming_locked();
	} else {
		sort_upcoming_locked();
	}
	maybe_arm_next_locked();
}

ShuffleMode Player::shuffle_mode() const {
	std::lock_guard<std::mutex> g(mtx_);
	return shuffle_mode_;
}

void Player::set_repeat_mode(RepeatMode mode) {
	std::lock_guard<std::mutex> g(mtx_);
	repeat_mode_ = mode;
}

RepeatMode Player::repeat_mode() const {
	std::lock_guard<std::mutex> g(mtx_);
	return repeat_mode_;
}

double Player::volume() const {
	std::lock_guard<std::mutex> g(mtx_);
	return desired_volume_;
}

PlaybackStatus Player::status() const {
	std::lock_guard<std::mutex> g(mtx_);
	return status_;
}

double Player::position_seconds() const {
	std::lock_guard<std::mutex> g(mtx_);
	return output_ ? output_->seconds_played() : 0.0;
}

double Player::duration_seconds() const {
	std::lock_guard<std::mutex> g(mtx_);
	return duration_of_chain(chain_.get());
}

std::string Player::current_url() const {
	std::lock_guard<std::mutex> g(mtx_);
	return current_url_;
}

nlohmann::json Player::current_metadata() const {
	std::lock_guard<std::mutex> g(mtx_);
	return metadata_from_chain_unlocked(chain_.get());
}

std::optional<size_t> Player::current_queue_index() const {
	std::lock_guard<std::mutex> g(mtx_);
	return current_index_;
}

bool Player::current_from_playlist() const {
	std::lock_guard<std::mutex> g(mtx_);
	if(!current_index_ || *current_index_ >= items_.size()) return false;
	return items_[*current_index_].from_playlist;
}

size_t Player::queue_length() const {
	std::lock_guard<std::mutex> g(mtx_);
	return current_index_ ? (queue_.size() + 1) : queue_.size();
}

std::vector<Player::QueueEntry> Player::queue_snapshot() const {
	std::vector<QueueEntry> out;
	std::lock_guard<std::mutex> g(mtx_);
	out.reserve((current_index_ ? 1 : 0) + queue_.size());

	if(current_index_ && chain_) {
		QueueEntry e;
		e.index = *current_index_;
		e.current = true;
		e.url = current_url_;
		e.format = chain_->format();
		e.duration_seconds = duration_of_chain(chain_.get());
		e.metadata = metadata_from_chain_unlocked(chain_.get());
		e.from_playlist = items_[*current_index_].from_playlist;
		out.push_back(std::move(e));
	}

	for(size_t i = 0; i < queue_.size(); ++i) {
		QueueEntry e;
		if(queue_[i]) {
			const size_t item_index = upcoming_indices_[i];
			e.index = item_index;
			e.url = items_[item_index].url;
			e.format = items_[item_index].format;
			e.duration_seconds = items_[item_index].duration_seconds;
			e.metadata = items_[item_index].metadata;
			e.from_playlist = items_[item_index].from_playlist;
		}
		out.push_back(std::move(e));
	}
	return out;
}

void Player::attach_metadata_callback_locked() {
	if(!chain_ || !chain_->input()) return;
	InputNode *active_input = chain_->input();
	active_input->set_metadata_changed_callback([this, active_input] {
		PlayerEvent ev;
		{
			std::lock_guard<std::mutex> g(mtx_);
			if(!chain_ || chain_->input() != active_input) return;
			ev.kind = PlayerEvent::Kind::MetadataChanged;
			ev.status = status_;
			ev.url = current_url_;
			ev.metadata = metadata_from_chain_unlocked(chain_.get());
			if(current_index_) items_[*current_index_].metadata = ev.metadata;
		}
		emit(std::move(ev));
	});
}

bool Player::start_head_locked() {
	if(queue_.empty() || upcoming_indices_.empty()) return false;
	auto chain = std::move(queue_.front());
	queue_.pop_front();
	size_t index = upcoming_indices_.front();
	upcoming_indices_.pop_front();
	return start_chain_locked(std::move(chain), index);
}

bool Player::start_chain_locked(std::unique_ptr<BufferChain> chain, size_t index) {
	if(!chain) return false;

	if(chain->launched()) {
		std::string url = chain->url();
		chain->close();
		if(!chain->open(url)) return false;
	}

	items_[index].format = chain->format();
	items_[index].duration_seconds = duration_of_chain(chain.get());
	items_[index].metadata = metadata_from_chain(chain.get());

	chain->retarget(std::nullopt);
	apply_replaygain_locked(chain.get());
	chain->launch();

	auto output = std::make_unique<OutputNode>();
	if(!output->open(chain->format())) return false;

	output->set_previous(chain->final_node());
	output->set_volume(desired_volume_);
	output->set_on_stream_consumed([this] {
		std::lock_guard<std::mutex> g(watchdog_mtx_);
		wake_for_consumed_ = true;
		watchdog_cv_.notify_all();
	});
	output->set_on_stream_advanced([this] {
		std::lock_guard<std::mutex> g(watchdog_mtx_);
		wake_for_advance_ = true;
		watchdog_cv_.notify_all();
	});
	output->start();

	current_index_ = index;
	current_url_ = items_[index].url;
	chain_ = std::move(chain);
	output_ = std::move(output);
	attach_metadata_callback_locked();
	maybe_arm_next_locked();
	return true;
}

bool Player::restart_current_locked() {
	if(!current_index_) return false;
	std::unique_ptr<BufferChain> old_chain = std::move(chain_);
	if(output_) {
		output_->close();
		output_.reset();
	}
	current_url_.clear();
	size_t idx = *current_index_;
	current_index_.reset();
	if(old_chain) old_chain->close();
	return start_index_locked(idx);
}

bool Player::start_index_locked(size_t index) {
	auto chain = open_chain_for_item_locked(index);
	if(!chain) return false;
	return start_chain_locked(std::move(chain), index);
}

std::unique_ptr<BufferChain> Player::open_chain_for_item_locked(size_t index) {
	if(index >= items_.size()) return nullptr;
	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(items_[index].url)) return nullptr;
	items_[index].format = chain->format();
	items_[index].duration_seconds = duration_of_chain(chain.get());
	items_[index].metadata = metadata_from_chain(chain.get());
	return chain;
}

void Player::insert_upcoming_locked(size_t item_index, std::unique_ptr<BufferChain> chain, std::optional<size_t> pos) {
	size_t at = pos ? std::min(*pos, queue_.size()) : queue_.size();
	auto qit = queue_.begin() + static_cast<std::deque<std::unique_ptr<BufferChain>>::difference_type>(at);
	auto iit = upcoming_indices_.begin() + static_cast<std::deque<size_t>::difference_type>(at);
	queue_.insert(qit, std::move(chain));
	upcoming_indices_.insert(iit, item_index);
}

void Player::reshuffle_upcoming_locked() {
	std::vector<std::pair<size_t, std::unique_ptr<BufferChain>>> zipped;
	zipped.reserve(queue_.size());
	while(!queue_.empty()) {
		zipped.emplace_back(upcoming_indices_.front(), std::move(queue_.front()));
		upcoming_indices_.pop_front();
		queue_.pop_front();
	}
	std::shuffle(zipped.begin(), zipped.end(), rng_);
	for(auto &entry : zipped) {
		upcoming_indices_.push_back(entry.first);
		queue_.push_back(std::move(entry.second));
	}
}

void Player::sort_upcoming_locked() {
	std::vector<std::pair<size_t, std::unique_ptr<BufferChain>>> zipped;
	zipped.reserve(queue_.size());
	while(!queue_.empty()) {
		zipped.emplace_back(upcoming_indices_.front(), std::move(queue_.front()));
		upcoming_indices_.pop_front();
		queue_.pop_front();
	}
	std::sort(zipped.begin(), zipped.end(), [](const auto &a, const auto &b) {
		return a.first < b.first;
	});
	for(auto &entry : zipped) {
		upcoming_indices_.push_back(entry.first);
		queue_.push_back(std::move(entry.second));
	}
}

bool Player::prepare_repeat_all_cycle_locked() {
	if(history_indices_.empty()) return false;
	std::vector<size_t> cycle(history_indices_.begin(), history_indices_.end());
	history_indices_.clear();
	if(shuffle_mode_ == ShuffleMode::All) std::shuffle(cycle.begin(), cycle.end(), rng_);
	for(size_t index : cycle) {
		auto chain = open_chain_for_item_locked(index);
		if(!chain) continue;
		insert_upcoming_locked(index, std::move(chain));
	}
	return !queue_.empty();
}

void Player::maybe_arm_next_locked() {
	if(!output_ || queue_.empty()) return;
	auto &next = queue_.front();
	if(!next) return;
	if(!next->launched()) {
		next->retarget(output_->format());
		apply_replaygain_locked(next.get());
		next->launch();
	}
	output_->set_next_source(next->final_node());
}

void Player::clear_playlist_locked() {
	queue_.clear();
	upcoming_indices_.clear();
	history_indices_.clear();
	items_.clear();
	current_index_.reset();
	current_url_.clear();
}

void Player::apply_replaygain_locked(BufferChain *chain) {
	if(!chain) return;
	chain->set_gain(replaygain_scale_for_metadata(
	    metadata_from_chain_unlocked(chain), replaygain_mode_));
}

void Player::watchdog_loop() {
	while(true) {
		std::unique_lock<std::mutex> lk(watchdog_mtx_);
		watchdog_cv_.wait(lk, [this] {
			return watchdog_stop_ || wake_for_advance_ || wake_for_consumed_;
		});
		if(watchdog_stop_) return;

		const bool advanced = wake_for_advance_;
		const bool consumed = wake_for_consumed_;
		wake_for_advance_ = false;
		wake_for_consumed_ = false;
		lk.unlock();

		if(advanced) {
			std::unique_ptr<BufferChain> old_chain;
			std::string old_url, new_url;
			nlohmann::json new_meta;
			{
				std::lock_guard<std::mutex> g(mtx_);
				if(!queue_.empty() && current_index_) {
					history_indices_.push_back(*current_index_);
					old_chain = std::move(chain_);
					old_url = current_url_;
					chain_ = std::move(queue_.front());
					queue_.pop_front();
					current_index_ = upcoming_indices_.front();
					upcoming_indices_.pop_front();
					current_url_ = chain_ ? chain_->url() : std::string{};
					attach_metadata_callback_locked();
				}
				new_meta = metadata_from_chain(chain_.get());
				new_url = current_url_;
				if(output_) output_->set_position_frames(0);
				maybe_arm_next_locked();
			}
			if(old_chain) old_chain->close();

			PlayerEvent ended;
			ended.kind = PlayerEvent::Kind::StreamEnded;
			ended.status = PlaybackStatus::Playing;
			ended.url = old_url;
			emit(std::move(ended));

			PlayerEvent began;
			began.kind = PlayerEvent::Kind::StreamBegan;
			began.status = PlaybackStatus::Playing;
			began.url = new_url;
			began.metadata = new_meta;
			emit(std::move(began));
		} else if(consumed) {
			std::unique_ptr<BufferChain> old_chain;
			std::string old_url;
			bool started = false;
			bool repeated_one = false;
			{
				std::lock_guard<std::mutex> g(mtx_);
				old_url = current_url_;
				if(repeat_mode_ == RepeatMode::One && current_index_) {
					old_chain = std::move(chain_);
					if(output_) {
						output_->close();
						output_.reset();
					}
					current_url_.clear();
					size_t idx = *current_index_;
					current_index_.reset();
					started = start_index_locked(idx);
					repeated_one = started;
				} else {
					if(current_index_) history_indices_.push_back(*current_index_);
					old_chain = std::move(chain_);
					if(output_) { output_->close(); output_.reset(); }
					current_url_.clear();
					current_index_.reset();
					if(queue_.empty() && repeat_mode_ == RepeatMode::All) {
						prepare_repeat_all_cycle_locked();
					}
					if(!queue_.empty()) started = start_head_locked();
				}
			}
			if(old_chain) old_chain->close();

			PlayerEvent ended;
			ended.kind = PlayerEvent::Kind::StreamEnded;
			ended.status = started ? PlaybackStatus::Playing : PlaybackStatus::Stopped;
			ended.url = old_url;
			emit(std::move(ended));

			if(started) {
				set_status(PlaybackStatus::Playing);
				PlayerEvent began;
				began.kind = PlayerEvent::Kind::StreamBegan;
				began.status = PlaybackStatus::Playing;
				began.url = current_url();
				began.metadata = current_metadata();
				emit(std::move(began));
			} else {
				stop();
			}
			(void)repeated_one;
		}
	}
}

void Player::set_status(PlaybackStatus s) {
	std::string url;
	{
		std::lock_guard<std::mutex> g(mtx_);
		status_ = s;
		url = current_url_;
	}
	PlayerEvent e;
	e.kind = PlayerEvent::Kind::StatusChanged;
	e.status = s;
	e.url = url;
	emit(std::move(e));
}

void Player::emit(PlayerEvent ev) {
	PlayerEventCallback cb;
	{
		std::lock_guard<std::mutex> g(mtx_);
		cb = cb_;
	}
	if(cb) cb(ev);
}

void Player::teardown() {
	std::unique_ptr<BufferChain> chain;
	std::unique_ptr<OutputNode> output;
	std::deque<std::unique_ptr<BufferChain>> q;
	{
		std::lock_guard<std::mutex> g(mtx_);
		chain = std::move(chain_);
		output = std::move(output_);
		q = std::move(queue_);
		clear_playlist_locked();
	}
	if(output) output->close();
	if(chain) chain->close();
	for(auto &c : q) if(c) c->close();
}

void Player::teardown_locked() {
	auto chain = std::move(chain_);
	auto output = std::move(output_);
	auto q = std::move(queue_);
	clear_playlist_locked();
	mtx_.unlock();
	if(output) output->close();
	if(chain) chain->close();
	for(auto &c : q) if(c) c->close();
	mtx_.lock();
}

} // namespace tuxedo
