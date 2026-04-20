#include "ipc/http_server.hpp"

#include "ipc/event_mailbox.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <memory>

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
	srv_->Get("/metadata", [this](const httplib::Request &, httplib::Response &res) {
		json req{{"op", "metadata"}};
		reply(res, ctl_.dispatch(req));
	});
	srv_->Post("/play", wrap_op("play"));
	srv_->Post("/pause", wrap_op("pause"));
	srv_->Post("/resume", wrap_op("resume"));
	srv_->Post("/stop", wrap_op("stop"));
	srv_->Post("/seek", wrap_op("seek"));
	srv_->Post("/volume", wrap_op("volume"));
	srv_->Post("/queue", wrap_op("queue"));
	srv_->Post("/queue_clear", wrap_op("queue_clear"));
	srv_->Post("/skip", wrap_op("skip"));
	srv_->Get("/queue", [this](const httplib::Request &, httplib::Response &res) {
		json req{{"op", "queue_list"}};
		reply(res, ctl_.dispatch(req));
	});

	// Server-Sent Events: stream player events to HTTP subscribers.
	srv_->Get("/events", [this](const httplib::Request &, httplib::Response &res) {
		auto box = std::make_shared<EventMailbox>(64);
		Controller::Token token = ctl_.subscribe(
		    [box](const json &ev) { box->push(ev.dump()); });

		res.set_header("Cache-Control", "no-cache");
		res.set_header("X-Accel-Buffering", "no");

		res.set_chunked_content_provider(
		    "text/event-stream",
		    [box](size_t /*offset*/, httplib::DataSink &sink) -> bool {
			    std::string msg;
			    if(!box->pop_for(msg, std::chrono::seconds(15))) {
				    // Heartbeat comment — keeps intermediaries from
				    // dropping the connection during idle periods.
				    static const char kBeat[] = ":\n\n";
				    return sink.write(kBeat, sizeof(kBeat) - 1);
			    }
			    const std::string framed = "data: " + msg + "\n\n";
			    return sink.write(framed.data(), framed.size());
		    },
		    [this, token, box](bool /*success*/) {
			    ctl_.unsubscribe(token);
			    box->close();
		    });
	});

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
