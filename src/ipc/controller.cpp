#include "ipc/controller.hpp"

#include "core/metadata_query.hpp"
#include "core/playlist_parser.hpp"

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

json dispatch_playlist(Player &player, const json &req, bool replace_current) {
	if(!req.contains("url") || !req["url"].is_string())
		return make_err(req, "request requires string `url`");

	const std::string url = req["url"].get<std::string>();
	PlaylistParseResult parsed = parse_playlist_url(url);
	if(parsed.recognized && !parsed.passthrough_original) {
		if(parsed.urls.empty()) return make_err(req, "playlist was empty");
		bool ok = replace_current ? player.play(parsed.urls.front(), true) : player.queue(parsed.urls.front(), true);
		if(!ok) return make_err(req, replace_current ? "play failed" : "queue failed");
		for(size_t i = 1; i < parsed.urls.size(); ++i) {
			if(!player.queue(parsed.urls[i], true)) return make_err(req, "queue failed");
		}
		return make_ok(req);
	}

	if(!(replace_current ? player.play(url) : player.queue(url)))
		return make_err(req, replace_current ? "play failed" : "queue failed");
	return make_ok(req);
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
		return dispatch_playlist(player_, req, true);
	}
	if(op == "pause") { player_.pause(); return make_ok(req); }
	if(op == "resume") { player_.resume(); return make_ok(req); }
	if(op == "stop") { player_.stop(); return make_ok(req); }
	if(op == "previous") {
		if(!player_.previous()) return make_err(req, "previous failed");
		return make_ok(req);
	}
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
		out["replaygain_mode"] = replaygain_mode_name(player_.replaygain_mode());
		out["shuffle_mode"] = shuffle_mode_name(player_.shuffle_mode());
		out["repeat_mode"] = repeat_mode_name(player_.repeat_mode());
		out["current_queue_index"] = player_.current_queue_index().value_or(0);
		out["from_playlist"] = player_.current_from_playlist();
		out["url"] = player_.current_url();
		out["metadata"] = player_.current_metadata();
		out["queue_length"] = player_.queue_length();
		return out;
	}
	if(op == "metadata") {
		json out = make_ok(req);
		out["metadata"] = player_.current_metadata();
		return out;
	}
	if(op == "metadata_for_url") {
		if(!req.contains("url") || !req["url"].is_string())
			return make_err(req, "metadata_for_url requires string `url`");
		json metadata;
		if(!read_metadata_for_url(req["url"].get<std::string>(), metadata))
			return make_err(req, "metadata query failed");
		json out = make_ok(req);
		out["url"] = req["url"];
		out["metadata"] = std::move(metadata);
		return out;
	}
	if(op == "properties_for_url") {
		if(!req.contains("url") || !req["url"].is_string())
			return make_err(req, "properties_for_url requires string `url`");
		json properties;
		if(!read_properties_for_url(req["url"].get<std::string>(), properties))
			return make_err(req, "properties query failed");
		json out = make_ok(req);
		out["url"] = req["url"];
		out["properties"] = std::move(properties);
		return out;
	}
	if(op == "queue") {
		return dispatch_playlist(player_, req, false);
	}
	if(op == "load_playlist") {
		const bool replace_current = req.value("action", std::string{"queue"}) == "play";
		return dispatch_playlist(player_, req, replace_current);
	}
	if(op == "queue_clear") { player_.queue_clear(); return make_ok(req); }
	if(op == "queue_jump") {
		if(!req.contains("index") || !req["index"].is_number_unsigned())
			return make_err(req, "queue_jump requires unsigned `index`");
		if(!player_.queue_jump(req["index"].get<size_t>()))
			return make_err(req, "queue jump failed");
		return make_ok(req);
	}
	if(op == "skip") {
		if(!player_.skip()) return make_err(req, "nothing to skip");
		return make_ok(req);
	}
	if(op == "shuffle") {
		if(req.contains("mode")) {
			if(!req["mode"].is_string())
				return make_err(req, "shuffle requires string `mode`");
			auto mode = shuffle_mode_from_string(req["mode"].get<std::string>());
			if(!mode) return make_err(req, "unknown shuffle mode");
			player_.set_shuffle_mode(*mode);
		}
		json out = make_ok(req);
		out["mode"] = shuffle_mode_name(player_.shuffle_mode());
		return out;
	}
	if(op == "repeat") {
		if(req.contains("mode")) {
			if(!req["mode"].is_string())
				return make_err(req, "repeat requires string `mode`");
			auto mode = repeat_mode_from_string(req["mode"].get<std::string>());
			if(!mode) return make_err(req, "unknown repeat mode");
			player_.set_repeat_mode(*mode);
		}
		json out = make_ok(req);
		out["mode"] = repeat_mode_name(player_.repeat_mode());
		return out;
	}
	if(op == "queue_list") {
		json out = make_ok(req);
		out["current_index"] = player_.current_queue_index().value_or(0);
		out["shuffle_mode"] = shuffle_mode_name(player_.shuffle_mode());
		out["repeat_mode"] = repeat_mode_name(player_.repeat_mode());
		json arr = json::array();
		for(const auto &e : player_.queue_snapshot()) {
			json o;
			o["index"] = e.index;
			o["current"] = e.current;
			o["from_playlist"] = e.from_playlist;
			o["url"] = e.url;
			o["duration"] = e.duration_seconds;
			o["sample_rate"] = e.format.sample_rate;
			o["channels"] = e.format.channels;
			o["metadata"] = e.metadata;
			arr.push_back(std::move(o));
		}
		out["queue"] = std::move(arr);
		return out;
	}
	if(op == "replaygain") {
		if(req.contains("mode")) {
			if(!req["mode"].is_string())
				return make_err(req, "replaygain requires string `mode`");
			auto mode = replaygain_mode_from_string(req["mode"].get<std::string>());
			if(!mode) return make_err(req, "unknown replaygain mode");
			player_.set_replaygain_mode(*mode);
		}
		json out = make_ok(req);
		out["mode"] = replaygain_mode_name(player_.replaygain_mode());
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
