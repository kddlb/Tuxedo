#include "core/player.hpp"

#include <utility>

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

} // namespace

Player::Player() {
	watchdog_ = std::thread([this] { watchdog_loop(); });
}

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

bool Player::play(const std::string &url) {
	// Clean slate, then queue + promote.
	teardown();

	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(url)) {
		emit({PlayerEvent::Kind::Error, PlaybackStatus::Stopped, url, "open failed", {}});
		return false;
	}

	{
		std::lock_guard<std::mutex> g(mtx_);
		queue_.clear();
		queue_.push_back(std::move(chain));
		if(!start_head_locked()) {
			queue_.clear();
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

bool Player::queue(const std::string &url) {
	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(url)) {
		emit({PlayerEvent::Kind::Error, status(), url, "queue open failed", {}});
		return false;
	}

	bool needs_start = false;
	{
		std::lock_guard<std::mutex> g(mtx_);
		queue_.push_back(std::move(chain));
		if(!chain_) {
			needs_start = start_head_locked();
		} else {
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
		if(output_) output_->set_next_source(nullptr);
	}
	for(auto &c : victims) {
		if(c) c->close();
	}
}

bool Player::skip() {
	// Advance the queue by faking a "current finished" on the watchdog.
	std::unique_ptr<BufferChain> old_chain;
	std::string old_url;
	bool had_current = false;
	{
		std::lock_guard<std::mutex> g(mtx_);
		if(!chain_) return false;
		had_current = true;
		old_url = current_url_;
		old_chain = std::move(chain_);
		if(output_) {
			// Hard stop the current output path; we'll re-open for the new head.
			output_->close();
			output_.reset();
		}
	}
	if(old_chain) old_chain->close();

	if(had_current) {
		PlayerEvent ended;
		ended.kind = PlayerEvent::Kind::StreamEnded;
		ended.status = PlaybackStatus::Stopped;
		ended.url = old_url;
		emit(std::move(ended));
	}

	bool started = false;
	{
		std::lock_guard<std::mutex> g(mtx_);
		current_url_.clear();
		if(!queue_.empty()) started = start_head_locked();
	}
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

void Player::stop() {
	teardown();
	set_status(PlaybackStatus::Stopped);
}

void Player::pause() {
	std::lock_guard<std::mutex> g(mtx_);
	if(output_) output_->pause();
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
	if(output_) output_->resume();
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
	// Seek in the input's native frame rate, not the output rate —
	// otherwise a 48 kHz output playing a 44.1 kHz source would request
	// a frame offset past the end of the file.
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

size_t Player::queue_length() const {
	std::lock_guard<std::mutex> g(mtx_);
	return queue_.size();
}

std::vector<Player::QueueEntry> Player::queue_snapshot() const {
	std::vector<QueueEntry> out;
	std::lock_guard<std::mutex> g(mtx_);
	out.reserve(queue_.size());
	for(const auto &c : queue_) {
		QueueEntry e;
		if(c) {
			e.url = c->url();
			e.format = c->format();
			e.duration_seconds = duration_of_chain(c.get());
			e.metadata = metadata_from_chain(c.get());
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
		}
		emit(std::move(ev));
	});
}

bool Player::start_head_locked() {
	// queue_ must be non-empty; mtx_ held.
	auto chain = std::move(queue_.front());
	queue_.pop_front();

	// If the chain was pre-launched as an armed-next under the previous
	// output format, its converter captured a stale target; reopen so
	// we can retarget cleanly at the new device's native format.
	if(chain->launched()) {
		std::string url = chain->url();
		chain->close();
		if(!chain->open(url)) return false;
	}

	// Head of playback — open the device at the track's native format
	// and run the chain's converter in identity passthrough. Queued
	// followers will retarget to this same format when they're armed.
	chain->retarget(std::nullopt);
	apply_replaygain_locked(chain.get());
	chain->launch();

	auto output = std::make_unique<OutputNode>();
	if(!output->open(chain->format())) return false;

	// set_previous must happen *after* open() — open() calls close()
	// internally, which resets the atomic previous_ pointer.
	output->set_previous(chain->final_node());
	output->set_volume(desired_volume_);

	// Wire up audio-thread notifications.
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

	current_url_ = chain->url();
	chain_ = std::move(chain);
	output_ = std::move(output);
	attach_metadata_callback_locked();

	maybe_arm_next_locked();
	return true;
}

void Player::maybe_arm_next_locked() {
	if(!output_ || queue_.empty()) return;
	auto &next = queue_.front();
	if(!next) return;
	// First arm: configure the chain's converter for the current output
	// format and launch its workers so it pre-buffers while the current
	// track plays. Subsequent calls short-circuit via launched().
	if(!next->launched()) {
		next->retarget(output_->format());
		apply_replaygain_locked(next.get());
		next->launch();
	}
	output_->set_next_source(next->final_node());
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
			// Hot-swap happened — pop the queue front (it's now active),
			// emit events, arm the *new* next. An advance subsumes any
			// consumed flag that races with it (see `else if` below);
			// the new track's arrival is the end of the old one.
			std::unique_ptr<BufferChain> old_chain;
			std::string old_url, new_url;
			nlohmann::json new_meta;
			{
				std::lock_guard<std::mutex> g(mtx_);
				if(!queue_.empty()) {
					old_chain = std::move(chain_);
					chain_ = std::move(queue_.front());
					queue_.pop_front();
					old_url = current_url_;
					current_url_ = chain_ ? chain_->url() : std::string{};
					attach_metadata_callback_locked();
				}
				new_meta = metadata_from_chain(chain_.get());
				new_url = current_url_;
				// Reset the position clock so callers see the new track
				// start from 0 instead of inheriting the previous track's
				// frame count.
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
		}

		else if(consumed) {
			// True end-of-playback with no armed next. Either the queue is
			// empty (natural stop), or a queue() call lost the race with
			// drain (rare — fall back to teardown + restart at new head).
			bool has_queue = false;
			std::string finished_url;
			{
				std::lock_guard<std::mutex> g(mtx_);
				has_queue = !queue_.empty();
				finished_url = current_url_;
			}
			if(!has_queue) {
				PlayerEvent ended;
				ended.kind = PlayerEvent::Kind::StreamEnded;
				ended.status = PlaybackStatus::Stopped;
				ended.url = finished_url;
				emit(std::move(ended));
				stop();
			} else {
				// Late-queue race: teardown + restart at new head.
				std::unique_ptr<BufferChain> old_chain;
				std::string old_url;
				{
					std::lock_guard<std::mutex> g(mtx_);
					old_chain = std::move(chain_);
					old_url = current_url_;
					if(output_) { output_->close(); output_.reset(); }
					current_url_.clear();
				}
				if(old_chain) old_chain->close();
				PlayerEvent ended;
				ended.kind = PlayerEvent::Kind::StreamEnded;
				ended.status = PlaybackStatus::Stopped;
				ended.url = old_url;
				emit(std::move(ended));

				bool started;
				{
					std::lock_guard<std::mutex> g(mtx_);
					started = start_head_locked();
				}
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
			}
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
		current_url_.clear();
	}
	if(output) output->close();
	if(chain) chain->close();
	for(auto &c : q) if(c) c->close();
}

void Player::teardown_locked() {
	// Caller holds mtx_. Releases + closes while briefly dropping the lock
	// so close() (which joins decoder threads) doesn't deadlock anyone.
	auto chain = std::move(chain_);
	auto output = std::move(output_);
	auto q = std::move(queue_);
	current_url_.clear();
	mtx_.unlock();
	if(output) output->close();
	if(chain) chain->close();
	for(auto &c : q) if(c) c->close();
	mtx_.lock();
}

} // namespace tuxedo
