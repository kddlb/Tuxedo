// HTTP REST wrapper around Controller. Opt-in: only starts if a port
// was requested. Binds 127.0.0.1 by default — no exposure to the LAN
// without an explicit reverse proxy.
#pragma once

#include "ipc/controller.hpp"

#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace tuxedo {

class HttpServer {
public:
	HttpServer(Controller &controller, std::string host, int port);
	~HttpServer();

	bool start();
	void stop();

	int port() const { return port_; }

private:
	Controller &ctl_;
	std::string host_;
	int port_;
	std::unique_ptr<httplib::Server> srv_;
	std::thread thread_;
};

} // namespace tuxedo
