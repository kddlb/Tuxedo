#include "core/player.hpp"

namespace tuxedo {

Player::Player() = default;

Player::~Player() { teardown(); }

void Player::set_event_callback(PlayerEventCallback cb) {
	std::lock_guard<std::mutex> g(mtx_);
	cb_ = std::move(cb);
}

bool Player::play(const std::string &url) {
	teardown();

	auto chain = std::make_unique<BufferChain>();
	if(!chain->open(url)) {
		emit({PlayerEvent::Kind::Error, PlaybackStatus::Stopped, url, "open failed"});
		return false;
	}

	auto output = std::make_unique<OutputNode>();
	output->set_previous(chain->final_node());
	if(!output->open(chain->format())) {
		emit({PlayerEvent::Kind::Error, PlaybackStatus::Stopped, url, "output open failed"});
		return false;
	}

	output->set_volume(desired_volume_);
	chain->launch();
	output->start();

	{
		std::lock_guard<std::mutex> g(mtx_);
		chain_ = std::move(chain);
		output_ = std::move(output);
		current_url_ = url;
	}

	set_status(PlaybackStatus::Playing);
	emit({PlayerEvent::Kind::StreamBegan, PlaybackStatus::Playing, url, {}});
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
	if(cb_) cb_({PlayerEvent::Kind::StatusChanged, status_, current_url_, {}});
}

void Player::resume() {
	std::lock_guard<std::mutex> g(mtx_);
	if(output_) output_->resume();
	status_ = PlaybackStatus::Playing;
	if(cb_) cb_({PlayerEvent::Kind::StatusChanged, status_, current_url_, {}});
}

bool Player::seek_seconds(double seconds) {
	std::lock_guard<std::mutex> g(mtx_);
	if(!chain_ || !output_) return false;
	auto fmt = output_->format();
	if(!fmt.valid()) return false;
	int64_t frame = static_cast<int64_t>(seconds * fmt.sample_rate);
	chain_->input()->request_seek(frame);
	return true;
}

void Player::set_volume(double v) {
	std::lock_guard<std::mutex> g(mtx_);
	desired_volume_ = v;
	if(output_) output_->set_volume(v);
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
	if(!chain_) return 0.0;
	auto fmt = chain_->format();
	int64_t total = chain_->input()->properties().total_frames;
	if(total < 0 || !fmt.valid()) return 0.0;
	return double(total) / fmt.sample_rate;
}

std::string Player::current_url() const {
	std::lock_guard<std::mutex> g(mtx_);
	return current_url_;
}

void Player::set_status(PlaybackStatus s) {
	{
		std::lock_guard<std::mutex> g(mtx_);
		status_ = s;
	}
	emit({PlayerEvent::Kind::StatusChanged, s, current_url(), {}});
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
	{
		std::lock_guard<std::mutex> g(mtx_);
		chain = std::move(chain_);
		output = std::move(output_);
	}
	// Close output first (detaches the render callback) before tearing down
	// the producer chain.
	if(output) output->close();
	if(chain) chain->close();
}

} // namespace tuxedo
