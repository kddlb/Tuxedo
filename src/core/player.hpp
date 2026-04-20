// Player: top-level playback orchestrator. Mirrors Cog's AudioPlayer,
// minus the gapless-queue / next-stream and HDCD machinery — those
// come later once the MVP renders sound.
#pragma once

#include "core/chain/buffer_chain.hpp"
#include "core/chain/output_node.hpp"
#include "core/status.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

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

	bool play(const std::string &url);
	void stop();
	void pause();
	void resume();
	bool seek_seconds(double seconds);

	void set_volume(double v);
	double volume() const;

	PlaybackStatus status() const;
	double position_seconds() const;
	double duration_seconds() const;
	std::string current_url() const;
	nlohmann::json current_metadata() const;

private:
	void set_status(PlaybackStatus s);
	void emit(PlayerEvent ev);
	void teardown();

	mutable std::mutex mtx_;
	PlaybackStatus status_ = PlaybackStatus::Stopped;
	std::string current_url_;
	PlayerEventCallback cb_;

	std::unique_ptr<BufferChain> chain_;
	std::unique_ptr<OutputNode> output_;

	double desired_volume_ = 1.0;
};

} // namespace tuxedo
