// Player: top-level playback orchestrator. Mirrors Cog's AudioPlayer,
// minus the gapless-queue / next-stream and HDCD machinery — those
// come later once the MVP renders sound.
#pragma once

#include "core/chain/buffer_chain.hpp"
#include "core/chain/output_node.hpp"
#include "core/replaygain.hpp"
#include "core/status.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tuxedo {

struct PlayerEvent {
	enum class Kind {
		StatusChanged,
		StreamBegan,
		StreamEnded,
		MetadataChanged,
		Error,
	};
	Kind kind;
	PlaybackStatus status = PlaybackStatus::Stopped;
	std::string url;
	std::string message;
	nlohmann::json metadata = nlohmann::json::object();
};

using PlayerEventCallback = std::function<void(const PlayerEvent &)>;

class Player {
public:
	Player();
	~Player();

	void set_event_callback(PlayerEventCallback cb);

	bool play(const std::string &url);      // stop + clear queue + play now
	bool queue(const std::string &url);     // append to queue (starts playing if idle)
	void queue_clear();                     // drop queue (current track unaffected)
	bool skip();                            // stop current, promote queue head
	void stop();
	void pause();
	void resume();
	bool seek_seconds(double seconds);

	void set_volume(double v);
	double volume() const;
	void set_replaygain_mode(ReplayGainMode mode);
	ReplayGainMode replaygain_mode() const;

	PlaybackStatus status() const;
	double position_seconds() const;
	double duration_seconds() const;
	std::string current_url() const;
	nlohmann::json current_metadata() const;

	// Snapshot of the upcoming queue for the queue_list op.
	struct QueueEntry {
		std::string url;
		StreamFormat format;
		double duration_seconds = 0.0;
		nlohmann::json metadata;
	};
	std::vector<QueueEntry> queue_snapshot() const;
	size_t queue_length() const;

private:
	// Start playback from the queue head (must be called with queue non-empty).
	// Caller holds mtx_. Returns false if queue head failed to open device.
	bool start_head_locked();
	// Try to arm next_source on the output node if queue head format matches.
	void maybe_arm_next_locked();
	// Called by the watchdog thread when OutputNode signals drain/advance.
	void watchdog_loop();
	void attach_metadata_callback_locked();

	void set_status(PlaybackStatus s);
	void emit(PlayerEvent ev);
	void apply_replaygain_locked(BufferChain *chain);
	void teardown();
	void teardown_locked();

	mutable std::mutex mtx_;
	PlaybackStatus status_ = PlaybackStatus::Stopped;
	std::string current_url_;
	PlayerEventCallback cb_;

	std::unique_ptr<BufferChain> chain_;
	std::unique_ptr<OutputNode> output_;

	std::deque<std::unique_ptr<BufferChain>> queue_;

	double desired_volume_ = 1.0;
	ReplayGainMode replaygain_mode_ = ReplayGainMode::AlbumPeak;

	// Watchdog thread + signalling.
	std::thread watchdog_;
	std::condition_variable watchdog_cv_;
	std::mutex watchdog_mtx_;
	bool wake_for_advance_ = false;
	bool wake_for_consumed_ = false;
	bool watchdog_stop_ = false;
};

} // namespace tuxedo
