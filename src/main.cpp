#include <csignal>
#include <iostream>
#include <set>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <uv.h>

#include "http/http_config.h"
#include "http/http_server.h"

using json = nlohmann::json;

struct Point
{
	int x;
	int y;
	auto operator<=>(const Point&) const = default;
};

int main()
{
	Point pt1{.x = 1, .y = 1}, pt2{.x = 1, .y = 2};
	std::set<Point> s;
	s.insert(pt1);
	s.insert(pt2);

	std::cout << std::boolalpha
		<< (pt1 == pt2) << " "
		<< (pt1 != pt2) << " "
		<< (pt1 < pt2) << " "
		<< (pt1 <= pt2) << " "
		<< (pt1 > pt2) << " "
		<< (pt1 >= pt2) << "\n";

	std::print("pt1 == pt2: {}\n", pt1 == pt2);

	// Read config.json (copied next to the executable by CMake)
	std::ifstream file("config.json");
	if (!file)
	{
		std::print(stderr, "Could not open config.json\n");
		return 1;
	}

	http::HttpConfig httpConfig;
	try
	{
		json config = json::parse(file);

		auto appName = config.at("app_name").get<std::string>();
		auto version = config.at("version").get<std::string>();
		bool debug = config.value("debug", false);
		int maxConnections = config.value("max_connections", 1);
		httpConfig = config.at("http_server").get<http::HttpConfig>();
		auto features = config.at("features").get<std::vector<std::string>>();

		std::print("app: {} v{}\n", appName, version);
		std::print("debug: {}, max_connections: {}\n", debug, maxConnections);
		std::print("http_server: {}:{}\n", httpConfig.host, httpConfig.port);
		std::print("http11: {}, http2: {}, http3: {}\n", httpConfig.http11, httpConfig.http2, httpConfig.http3);
		std::print("ssl: enabled: {}, cert: {}, key: {}, min_version: {}\n", httpConfig.ssl.enabled,
				   httpConfig.ssl.certificateFile, httpConfig.ssl.keyFile, httpConfig.ssl.minVersion);
		std::print("features: {}\n", features);
	}
	catch (const json::exception& e)
	{
		std::print(stderr, "Config error: {}\n", e.what());
		return 1;
	}

	// Writes to a peer that closed the connection must not kill the process
	std::signal(SIGPIPE, SIG_IGN);

	uv_loop_t* loop = uv_default_loop();

	http::HttpServer server(loop, httpConfig);
	try
	{
		server.start();
	}
	catch (const std::exception& e)
	{
		std::print(stderr, "HTTP server error: {}\n", e.what());
		return 1;
	}

	// Stop gracefully on Ctrl+C / SIGTERM
	struct SignalCtx
	{
		http::HttpServer* server;
		uv_signal_t sigint;
		uv_signal_t sigterm;
	} sigCtx{.server = &server};

	auto onSignal = [](uv_signal_t* handle, int signum) {
		auto* ctx = static_cast<SignalCtx*>(handle->data);
		std::print("received signal {}, shutting down\n", signum);
		uv_close(reinterpret_cast<uv_handle_t*>(&ctx->sigint), nullptr);
		uv_close(reinterpret_cast<uv_handle_t*>(&ctx->sigterm), nullptr);
		ctx->server->stop([] { std::print("http server stopped\n"); });
	};
	for (auto [handle, signum] : {std::pair{&sigCtx.sigint, SIGINT}, std::pair{&sigCtx.sigterm, SIGTERM}})
	{
		uv_signal_init(loop, handle);
		handle->data = &sigCtx;
		uv_signal_start(handle, onSignal, signum);
	}

	std::print("libuv {} loop running...\n", uv_version_string());
	uv_run(loop, UV_RUN_DEFAULT);
	uv_loop_close(loop);
	std::print("loop finished\n");
}
