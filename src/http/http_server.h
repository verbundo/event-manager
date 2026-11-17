#pragma once

#include <functional>
#include <memory>
#include <uv.h>

#include "http/http_config.h"

namespace http
{

// HTTP/1.1 + HTTP/2 server built on libh2o (libuv backend), running on an existing libuv loop.
class HttpServer
{
public:
	HttpServer(uv_loop_t* loop, HttpConfig config);
	~HttpServer();

	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

	// Configures TLS (if enabled) and starts listening. Throws std::runtime_error on failure.
	void start();

	// Stops accepting connections, asks open connections to finish, and releases all libuv handles
	// owned by the server (so uv_run can return). `onStopped` runs once that is done.
	void stop(std::function<void()> onStopped = {});

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace http
