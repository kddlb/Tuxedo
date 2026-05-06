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
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

enum class ShuffleMode {
	Off = 0,
	All,
};

enum class RepeatMode {
	Off = 0,
	One,
	All,
};

const char *shuffle_mode_name(ShuffleMode mode);
const char *repeat_mode_name(RepeatMode mode);
std::optional<ShuffleMode> shuffle_mode_from_string(const std::string &mode);
std::optional<RepeatMode> repeat_mode_from_string(const std::string &mode);

class Player {
public:
	Player();
	~Player();

	void set_event_callback(PlayerEventCallback cb);

	bool play(const std::string &url, bool from_playlist = false);   // stop + clear queue + play now
	bool queue(const std::string &url, bool from_playlist = false);  // append to queue (starts playing if idle)
	void queue_clear();                     // drop queue (current track unaffected)
	bool previous();
	bool skip();                            // stop current, promote queue head
	bool queue_jump(size_t index);
	void stop();
	void pause();
	void resume();
	bool seek_seconds(double seconds);

	void set_volume(double v);
	double volume() const;
	void set_replaygain_mode(ReplayGainMode mode);
	ReplayGainMode replaygain_mode() const;
	void set_shuffle_mode(ShuffleMode mode);
	ShuffleMode shuffle_mode() const;
	void set_repeat_mode(RepeatMode mode);
	RepeatMode repeat_mode() const;

	PlaybackStatus status() const;
	double position_seconds() const;
	double duration_seconds() const;
	std::string current_url() const;
	nlohmann::json current_metadata() const;
	std::optional<size_t> current_queue_index() const;
	bool current_from_playlist() const;

	// Snapshot of the logical queue for the queue_list op: current item
	// first, then upcoming items in play order.
	struct QueueEntry {
		size_t index = 0;
		bool current = false;
		std::string url;
		StreamFormat format;
		double duration_seconds = 0.0;
		nlohmann::json metadata;
		bool from_playlist = false;
	};
	std::vector<QueueEntry> queue_snapshot() const;
	size_t queue_length() const;

private:
	struct PlaylistItem {
		std::string url;
		StreamFormat format{};
		double duration_seconds = 0.0;
		nlohmann::json metadata = nlohmann::json::object();
		bool from_playlist = false;
	};

	// Start playback from the queue head (must be called with queue non-empty).
	// Caller holds mtx_. Returns false if queue head failed to open device.
	bool start_head_locked();
	bool start_chain_locked(std::unique_ptr<BufferChain> chain, size_t index);
	bool restart_current_locked();
	bool start_index_locked(size_t index);
	std::unique_ptr<BufferChain> open_chain_for_item_locked(size_t index);
	void insert_upcoming_locked(size_t item_index, std::unique_ptr<BufferChain> chain, std::optional<size_t> pos = std::nullopt);
	void reshuffle_upcoming_locked();
	void sort_upcoming_locked();
	bool prepare_repeat_all_cycle_locked();
	// Try to arm next_source on the output node if queue head format matches.
	void maybe_arm_next_locked();
	// Called by the watchdog thread when OutputNode signals drain/advance.
	void watchdog_loop();
	void attach_metadata_callback_locked();
	void clear_playlist_locked();

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
	std::deque<size_t> upcoming_indices_;
	std::vector<size_t> history_indices_;
	std::vector<PlaylistItem> items_;
	std::optional<size_t> current_index_;

	double desired_volume_ = 1.0;
	ReplayGainMode replaygain_mode_ = ReplayGainMode::AlbumPeak;
	ShuffleMode shuffle_mode_ = ShuffleMode::Off;
	RepeatMode repeat_mode_ = RepeatMode::Off;
	std::mt19937 rng_{std::random_device{}()};

	// Watchdog thread + signalling.
	std::thread watchdog_;
	std::condition_variable watchdog_cv_;
	std::mutex watchdog_mtx_;
	bool wake_for_advance_ = false;
	bool wake_for_consumed_ = false;
	bool watchdog_stop_ = false;
};

} // namespace tuxedo
