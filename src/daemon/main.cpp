// tuxedod: headless audio daemon.
//
// Transports:
//   - unix socket  (JSON-lines, always on unless --no-socket)
//   - HTTP REST    (JSON, opt-in via --http <port>)
//   - stdin        (line-based dev console, always on unless --no-stdin)
//
// Flags:
//   --socket <path>    override default socket path
//   --no-socket        disable unix socket
//   --http <port>      enable HTTP on 127.0.0.1:<port>
//   --http-host <ip>   override HTTP bind host (default 127.0.0.1)
//   --no-stdin         don't read commands from stdin
#include "core/player.hpp"
#include "ipc/controller.hpp"
#include "ipc/http_server.hpp"
#include "ipc/socket_server.hpp"
#include "ipc/stdin_control.hpp"
#include "plugin/registry.hpp"

#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

struct Options {
	bool socket = true;
	std::string socket_path; // empty → default
	bool http = false;
	std::string http_host = "127.0.0.1";
	int http_port = 0;
	bool stdin_console = true;
};

void usage(const char *argv0) {
	std::fprintf(stderr,
	             "usage: %s [--socket PATH] [--no-socket]\n"
	             "            [--http PORT] [--http-host ADDR]\n"
	             "            [--no-stdin]\n",
	             argv0);
}

bool parse(int argc, char **argv, Options &o) {
	for(int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto take = [&](const char *flag, std::string &out) {
			if(i + 1 >= argc) {
				std::fprintf(stderr, "%s needs an argument\n", flag);
				return false;
			}
			out = argv[++i];
			return true;
		};
		if(a == "--socket") {
			if(!take("--socket", o.socket_path)) return false;
			o.socket = true;
		} else if(a == "--no-socket") {
			o.socket = false;
		} else if(a == "--http") {
			std::string v;
			if(!take("--http", v)) return false;
			o.http = true;
			o.http_port = std::atoi(v.c_str());
			if(o.http_port <= 0 || o.http_port > 65535) {
				std::fprintf(stderr, "--http: invalid port %s\n", v.c_str());
				return false;
			}
		} else if(a == "--http-host") {
			if(!take("--http-host", o.http_host)) return false;
		} else if(a == "--no-stdin") {
			o.stdin_console = false;
		} else if(a == "-h" || a == "--help") {
			usage(argv[0]);
			std::exit(0);
		} else {
			std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
			usage(argv[0]);
			return false;
		}
	}
	return true;
}

// Signal plumbing — let SIGINT/SIGTERM unblock the main wait loop.
std::mutex g_signal_mtx;
std::condition_variable g_signal_cv;
bool g_signal_fired = false;

void signal_handler(int) {
	std::lock_guard<std::mutex> g(g_signal_mtx);
	g_signal_fired = true;
	g_signal_cv.notify_all();
}

} // namespace

int main(int argc, char **argv) {
	Options opt;
	if(!parse(argc, argv, opt)) return 2;

	tuxedo::register_builtin_plugins();

	tuxedo::Player player;
	tuxedo::Controller ctl(player);

	std::unique_ptr<tuxedo::SocketServer> sock;
	if(opt.socket) {
		const std::string path = opt.socket_path.empty()
		    ? tuxedo::SocketServer::default_path() : opt.socket_path;
		sock = std::make_unique<tuxedo::SocketServer>(ctl, path);
		if(!sock->start()) {
			std::fprintf(stderr, "failed to start socket on %s\n", path.c_str());
			return 1;
		}
		std::fprintf(stderr, "socket: %s\n", path.c_str());
	}

	std::unique_ptr<tuxedo::HttpServer> http;
	if(opt.http) {
		http = std::make_unique<tuxedo::HttpServer>(ctl, opt.http_host, opt.http_port);
		if(!http->start()) {
			std::fprintf(stderr, "failed to start http on %s:%d\n",
			             opt.http_host.c_str(), opt.http_port);
			return 1;
		}
		std::fprintf(stderr, "http: http://%s:%d\n", opt.http_host.c_str(), opt.http_port);
	}

	std::puts("tuxedo ready");
	std::fflush(stdout);

	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);
	std::signal(SIGPIPE, SIG_IGN);

	if(opt.stdin_console) {
		// Blocks until stdin closes or `quit` is received.
		tuxedo::run_stdin_control(ctl);
	} else {
		// Block on signal.
		std::unique_lock<std::mutex> lk(g_signal_mtx);
		g_signal_cv.wait(lk, [] { return g_signal_fired; });
	}

	player.stop();
	if(http) http->stop();
	if(sock) sock->stop();
	return 0;
}
