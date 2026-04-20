// Unix-domain-socket IPC server. One JSON object per line; each client
// can also receive async event lines.
#pragma once

#include "ipc/controller.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tuxedo {

class SocketServer {
public:
	SocketServer(Controller &controller, std::string socket_path);
	~SocketServer();

	bool start();
	void stop();

	const std::string &socket_path() const { return path_; }

	// Build the default socket path for this user. Tries
	// $XDG_RUNTIME_DIR/tuxedo.sock, falls back to /tmp/tuxedo-$UID.sock.
	static std::string default_path();

private:
	void accept_loop();
	void client_loop(int fd);
	void close_all_clients();

	Controller &ctl_;
	std::string path_;

	int listen_fd_ = -1;
	std::atomic<bool> running_{false};
	std::thread accept_thread_;

	std::mutex clients_mtx_;
	std::vector<int> client_fds_;
	std::vector<std::thread> client_threads_;
};

} // namespace tuxedo
