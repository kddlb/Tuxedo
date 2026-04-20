// Controller: single JSON-in / JSON-out command surface for the daemon.
// Every transport (stdin, unix socket, HTTP) goes through this, so the
// wire protocol stays consistent across them.
//
// Request shape:  {"id": <optional>, "op": "<name>", ...args}
// Response shape: {"id": <echoed>, "ok": <bool>, ...fields / "error": "<msg>"}
// Event shape:    {"event": "<name>", ...fields}   (broadcast, no id)
#pragma once

#include "core/player.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tuxedo {

using json = nlohmann::json;

class Controller {
public:
	using EventCallback = std::function<void(const json &)>;
	using Token = uint64_t;

	explicit Controller(Player &player);
	~Controller();

	// Dispatch a request object; always returns a response object.
	// Malformed JSON and unknown ops return `{"ok": false, "error": ...}`.
	json dispatch(const json &req);

	// Subscribe to player events (broadcast). The callback may fire on
	// any thread; it must not call back into dispatch synchronously.
	Token subscribe(EventCallback cb);
	void unsubscribe(Token t);

private:
	void on_player_event(const PlayerEvent &ev);
	void publish(const json &ev);

	Player &player_;

	std::mutex subs_mtx_;
	std::unordered_map<Token, EventCallback> subs_;
	std::atomic<Token> next_token_{1};
};

} // namespace tuxedo
