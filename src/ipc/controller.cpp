#include "ipc/controller.hpp"

namespace tuxedo {

namespace {

const char *kind_name(PlayerEvent::Kind k) {
	switch(k) {
		case PlayerEvent::Kind::StatusChanged: return "status_changed";
		case PlayerEvent::Kind::StreamBegan: return "stream_began";
		case PlayerEvent::Kind::StreamEnded: return "stream_ended";
		case PlayerEvent::Kind::MetadataChanged: return "metadata_changed";
		case PlayerEvent::Kind::Error: return "error";
	}
	return "unknown";
}

json make_ok(const json &req) {
	json out;
	out["ok"] = true;
	if(req.contains("id")) out["id"] = req["id"];
	return out;
}

json make_err(const json &req, const std::string &msg) {
	json out;
	out["ok"] = false;
	out["error"] = msg;
	if(req.contains("id")) out["id"] = req["id"];
	return out;
}

} // namespace

Controller::Controller(Player &player) : player_(player) {
	player_.set_event_callback([this](const PlayerEvent &ev) { on_player_event(ev); });
}

Controller::~Controller() {
	player_.set_event_callback(nullptr);
}

json Controller::dispatch(const json &req) {
	if(!req.is_object() || !req.contains("op") || !req["op"].is_string()) {
		return make_err(req, "request must be an object with a string `op`");
	}
	const std::string op = req["op"].get<std::string>();

	if(op == "play") {
		if(!req.contains("url") || !req["url"].is_string())
			return make_err(req, "play requires string `url`");
		if(!player_.play(req["url"].get<std::string>()))
			return make_err(req, "play failed");
		return make_ok(req);
	}
	if(op == "pause") { player_.pause(); return make_ok(req); }
	if(op == "resume") { player_.resume(); return make_ok(req); }
	if(op == "stop") { player_.stop(); return make_ok(req); }
	if(op == "seek") {
		if(!req.contains("seconds") || !req["seconds"].is_number())
			return make_err(req, "seek requires numeric `seconds`");
		if(!player_.seek_seconds(req["seconds"].get<double>()))
			return make_err(req, "seek failed");
		return make_ok(req);
	}
	if(op == "volume") {
		if(!req.contains("value") || !req["value"].is_number())
			return make_err(req, "volume requires numeric `value`");
		player_.set_volume(req["value"].get<double>());
		return make_ok(req);
	}
	if(op == "status") {
		json out = make_ok(req);
		out["state"] = status_name(player_.status());
		out["position"] = player_.position_seconds();
		out["duration"] = player_.duration_seconds();
		out["volume"] = player_.volume();
		out["url"] = player_.current_url();
		out["metadata"] = player_.current_metadata();
		return out;
	}
	if(op == "metadata") {
		json out = make_ok(req);
		out["metadata"] = player_.current_metadata();
		return out;
	}
	return make_err(req, "unknown op: " + op);
}

Controller::Token Controller::subscribe(EventCallback cb) {
	Token t = next_token_.fetch_add(1);
	std::lock_guard<std::mutex> g(subs_mtx_);
	subs_[t] = std::move(cb);
	return t;
}

void Controller::unsubscribe(Token t) {
	std::lock_guard<std::mutex> g(subs_mtx_);
	subs_.erase(t);
}

void Controller::on_player_event(const PlayerEvent &ev) {
	json j;
	j["event"] = kind_name(ev.kind);
	j["state"] = status_name(ev.status);
	if(!ev.url.empty()) j["url"] = ev.url;
	if(!ev.message.empty()) j["message"] = ev.message;
	if(!ev.metadata.empty()) j["metadata"] = ev.metadata;
	publish(j);
}

void Controller::publish(const json &ev) {
	// Snapshot subscribers under lock, then invoke outside so a
	// callback's own subscribe/unsubscribe doesn't deadlock us.
	std::vector<EventCallback> snapshot;
	{
		std::lock_guard<std::mutex> g(subs_mtx_);
		snapshot.reserve(subs_.size());
		for(const auto &kv : subs_) snapshot.push_back(kv.second);
	}
	for(const auto &cb : snapshot) cb(ev);
}

} // namespace tuxedo
