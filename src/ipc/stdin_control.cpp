#include "ipc/stdin_control.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace tuxedo {

namespace {

void println_event(const PlayerEvent &ev) {
	const char *kind = "?";
	switch(ev.kind) {
		case PlayerEvent::Kind::StatusChanged: kind = "status"; break;
		case PlayerEvent::Kind::StreamBegan: kind = "began"; break;
		case PlayerEvent::Kind::StreamEnded: kind = "ended"; break;
		case PlayerEvent::Kind::Error: kind = "error"; break;
	}
	std::cout << "event: " << kind << ' ' << status_name(ev.status);
	if(!ev.url.empty()) std::cout << " url=" << ev.url;
	if(!ev.message.empty()) std::cout << " msg=" << ev.message;
	std::cout << '\n' << std::flush;
}

void reply_ok() { std::cout << "ok\n" << std::flush; }
void reply_err(const std::string &m) { std::cout << "err " << m << '\n' << std::flush; }

} // namespace

void run_stdin_control(Player &player) {
	player.set_event_callback(println_event);

	std::string line;
	while(std::getline(std::cin, line)) {
		std::istringstream iss(line);
		std::string cmd;
		iss >> cmd;
		if(cmd.empty()) continue;

		if(cmd == "quit" || cmd == "exit") {
			player.stop();
			reply_ok();
			return;
		}
		if(cmd == "play") {
			std::string url;
			std::getline(iss, url);
			// Trim leading whitespace.
			size_t i = 0;
			while(i < url.size() && std::isspace(static_cast<unsigned char>(url[i]))) ++i;
			url.erase(0, i);
			if(url.empty()) { reply_err("play requires a path"); continue; }
			if(!player.play(url)) reply_err("play failed");
			else reply_ok();
			continue;
		}
		if(cmd == "pause") { player.pause(); reply_ok(); continue; }
		if(cmd == "resume") { player.resume(); reply_ok(); continue; }
		if(cmd == "stop") { player.stop(); reply_ok(); continue; }
		if(cmd == "seek") {
			double t = 0.0;
			if(!(iss >> t)) { reply_err("seek requires seconds"); continue; }
			if(!player.seek_seconds(t)) reply_err("seek failed");
			else reply_ok();
			continue;
		}
		if(cmd == "volume") {
			double v = 1.0;
			if(!(iss >> v)) { reply_err("volume requires a number 0..1"); continue; }
			player.set_volume(v);
			reply_ok();
			continue;
		}
		if(cmd == "status") {
			std::cout << "status " << status_name(player.status())
			          << " position=" << player.position_seconds()
			          << " duration=" << player.duration_seconds()
			          << " volume=" << player.volume()
			          << " url=" << player.current_url()
			          << '\n' << std::flush;
			continue;
		}
		reply_err("unknown command: " + cmd);
	}
}

} // namespace tuxedo
