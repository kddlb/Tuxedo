#include "ipc/http_server.hpp"

#include <httplib.h>

#include <cstdio>

namespace tuxedo {

namespace {

// Map Controller response ok/err into HTTP status + JSON body.
void reply(httplib::Response &res, const json &out) {
	const bool ok = out.value("ok", false);
	res.status = ok ? 200 : 400;
	res.set_content(out.dump(), "application/json");
}

json parse_body(const httplib::Request &req, json fallback = json::object()) {
	if(req.body.empty()) return fallback;
	try {
		return json::parse(req.body);
	} catch(const std::exception &) {
		return fallback;
	}
}

} // namespace

HttpServer::HttpServer(Controller &c, std::string host, int port)
: ctl_(c), host_(std::move(host)), port_(port),
  srv_(std::make_unique<httplib::Server>()) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
	auto wrap_op = [this](const std::string &op) {
		return [this, op](const httplib::Request &req, httplib::Response &res) {
			json body = parse_body(req);
			body["op"] = op;
			reply(res, ctl_.dispatch(body));
		};
	};

	srv_->Get("/status", [this](const httplib::Request &, httplib::Response &res) {
		json req{{"op", "status"}};
		reply(res, ctl_.dispatch(req));
	});
	srv_->Post("/play", wrap_op("play"));
	srv_->Post("/pause", wrap_op("pause"));
	srv_->Post("/resume", wrap_op("resume"));
	srv_->Post("/stop", wrap_op("stop"));
	srv_->Post("/seek", wrap_op("seek"));
	srv_->Post("/volume", wrap_op("volume"));

	// Generic dispatch for anything else — body should be a full request.
	srv_->Post("/rpc", [this](const httplib::Request &req, httplib::Response &res) {
		reply(res, ctl_.dispatch(parse_body(req)));
	});

	if(!srv_->bind_to_port(host_.c_str(), port_)) {
		std::fprintf(stderr, "http: failed to bind %s:%d\n", host_.c_str(), port_);
		return false;
	}
	thread_ = std::thread([this] { srv_->listen_after_bind(); });
	return true;
}

void HttpServer::stop() {
	if(srv_) srv_->stop();
	if(thread_.joinable()) thread_.join();
}

} // namespace tuxedo
