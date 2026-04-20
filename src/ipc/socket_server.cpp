#include "ipc/socket_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace tuxedo {

namespace {

void write_line(int fd, const std::string &line) {
	std::string buf = line;
	buf.push_back('\n');
	const char *p = buf.data();
	size_t remaining = buf.size();
	while(remaining > 0) {
		ssize_t n = ::write(fd, p, remaining);
		if(n < 0) {
			if(errno == EINTR) continue;
			return; // client gone; caller will detect on next read
		}
		p += n;
		remaining -= static_cast<size_t>(n);
	}
}

} // namespace

SocketServer::SocketServer(Controller &c, std::string p)
: ctl_(c), path_(std::move(p)) {}

SocketServer::~SocketServer() { stop(); }

std::string SocketServer::default_path() {
	if(const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
		if(*xdg) return std::string(xdg) + "/tuxedo.sock";
	}
	return "/tmp/tuxedo-" + std::to_string(::getuid()) + ".sock";
}

bool SocketServer::start() {
	// Remove any stale socket file from a previous crashed run.
	::unlink(path_.c_str());

	listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if(listen_fd_ < 0) {
		std::perror("socket");
		return false;
	}

	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if(path_.size() >= sizeof(addr.sun_path)) {
		std::fprintf(stderr, "socket path too long: %s\n", path_.c_str());
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}
	std::strcpy(addr.sun_path, path_.c_str());

	// Bind with umask so the socket is 0600 (owner only).
	mode_t prev = ::umask(0077);
	int r = ::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	::umask(prev);
	if(r < 0) {
		std::perror("bind");
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}
	if(::listen(listen_fd_, 8) < 0) {
		std::perror("listen");
		::close(listen_fd_);
		::unlink(path_.c_str());
		listen_fd_ = -1;
		return false;
	}

	running_.store(true);
	accept_thread_ = std::thread([this] { accept_loop(); });
	return true;
}

void SocketServer::stop() {
	if(!running_.exchange(false)) return;

	if(listen_fd_ >= 0) {
		// Shutdown wakes accept() on macOS; close() on Linux. Do both for safety.
		::shutdown(listen_fd_, SHUT_RDWR);
		::close(listen_fd_);
		listen_fd_ = -1;
	}
	::unlink(path_.c_str());

	if(accept_thread_.joinable()) accept_thread_.join();
	close_all_clients();
}

void SocketServer::close_all_clients() {
	std::vector<std::thread> threads;
	{
		std::lock_guard<std::mutex> g(clients_mtx_);
		for(int fd : client_fds_) ::shutdown(fd, SHUT_RDWR);
		threads = std::move(client_threads_);
		client_threads_.clear();
	}
	for(auto &t : threads) {
		if(t.joinable()) t.join();
	}
}

void SocketServer::accept_loop() {
	while(running_.load()) {
		int fd = ::accept(listen_fd_, nullptr, nullptr);
		if(fd < 0) {
			if(errno == EINTR) continue;
			break; // listen_fd_ closed; loop exits
		}

		std::lock_guard<std::mutex> g(clients_mtx_);
		client_fds_.push_back(fd);
		client_threads_.emplace_back([this, fd] { client_loop(fd); });
	}
}

void SocketServer::client_loop(int fd) {
	// Subscribe this client to broadcast events.
	auto token = ctl_.subscribe([fd](const json &ev) {
		write_line(fd, ev.dump());
	});

	std::string rbuf;
	char chunk[4096];
	while(running_.load()) {
		ssize_t n = ::read(fd, chunk, sizeof(chunk));
		if(n == 0) break; // EOF
		if(n < 0) {
			if(errno == EINTR) continue;
			break;
		}
		rbuf.append(chunk, static_cast<size_t>(n));

		// Process complete lines; leave any partial in rbuf.
		size_t pos = 0;
		while(true) {
			size_t nl = rbuf.find('\n', pos);
			if(nl == std::string::npos) break;
			std::string line = rbuf.substr(pos, nl - pos);
			pos = nl + 1;

			if(line.empty()) continue;
			json req;
			try {
				req = json::parse(line);
			} catch(const std::exception &e) {
				json err;
				err["ok"] = false;
				err["error"] = std::string("invalid JSON: ") + e.what();
				write_line(fd, err.dump());
				continue;
			}
			json resp = ctl_.dispatch(req);
			write_line(fd, resp.dump());
		}
		rbuf.erase(0, pos);
	}

	ctl_.unsubscribe(token);
	::close(fd);

	// Remove self from clients vector so we don't leak the fd entry.
	std::lock_guard<std::mutex> g(clients_mtx_);
	auto it = std::find(client_fds_.begin(), client_fds_.end(), fd);
	if(it != client_fds_.end()) client_fds_.erase(it);
}

} // namespace tuxedo
