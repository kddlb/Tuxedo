#include "ipc/stdin_control.hpp"

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

namespace tuxedo {

namespace {

// Build a Controller request object from a line-based command.
// Returns an empty object if the line isn't a recognised command.
json build_request(const std::string &line) {
	std::istringstream iss(line);
	std::string cmd;
	iss >> cmd;
	if(cmd.empty()) return {};

	json req;
	if(cmd == "quit" || cmd == "exit") {
		req["op"] = "stop";
		req["__quit"] = true;
		return req;
	}
	if(cmd == "play") {
		std::string url;
		std::getline(iss, url);
		size_t i = 0;
		while(i < url.size() && std::isspace(static_cast<unsigned char>(url[i]))) ++i;
		url.erase(0, i);
		if(url.empty()) return {};
		req["op"] = "play";
		req["url"] = url;
		return req;
	}
	if(cmd == "pause" || cmd == "resume" || cmd == "stop" || cmd == "status") {
		req["op"] = cmd;
		return req;
	}
	if(cmd == "seek") {
		double t = 0.0;
		if(!(iss >> t)) return {};
		req["op"] = "seek";
		req["seconds"] = t;
		return req;
	}
	if(cmd == "volume") {
		double v = 1.0;
		if(!(iss >> v)) return {};
		req["op"] = "volume";
		req["value"] = v;
		return req;
	}
	return {};
}

void print_response(const json &resp) {
	if(resp.value("ok", false)) {
		if(resp.contains("state")) {
			// It's a status response — print the detail line.
			std::cout << "status " << resp["state"].get<std::string>()
			          << " position=" << resp.value("position", 0.0)
			          << " duration=" << resp.value("duration", 0.0)
			          << " volume=" << resp.value("volume", 0.0)
			          << " url=" << resp.value("url", std::string{})
			          << '\n';
		} else {
			std::cout << "ok\n";
		}
	} else {
		std::cout << "err " << resp.value("error", std::string{"unknown"}) << '\n';
	}
	std::cout.flush();
}

void print_event(const json &ev) {
	// Match the previous stdin transport's event line style so existing
	// smoke scripts keep working.
	std::cout << "event: " << ev.value("event", std::string{"?"})
	          << ' ' << ev.value("state", std::string{"unknown"});
	if(ev.contains("url")) std::cout << " url=" << ev["url"].get<std::string>();
	if(ev.contains("message")) std::cout << " msg=" << ev["message"].get<std::string>();
	std::cout << '\n' << std::flush;
}

} // namespace

void run_stdin_control(Controller &ctl) {
	auto token = ctl.subscribe(print_event);

	std::string line;
	while(std::getline(std::cin, line)) {
		json req = build_request(line);
		if(req.is_null() || !req.is_object() || !req.contains("op")) {
			if(!line.empty())
				std::cout << "err unknown command\n" << std::flush;
			continue;
		}
		const bool is_quit = req.value("__quit", false);
		req.erase("__quit");
		json resp = ctl.dispatch(req);
		print_response(resp);
		if(is_quit) break;
	}

	ctl.unsubscribe(token);
}

} // namespace tuxedo
