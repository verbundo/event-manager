#include <iostream>
#include <set>
#include <fstream>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <uv.h>

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

	fmt::print("pt1 == pt2: {}\n", pt1 == pt2);

	// Read config.json (copied next to the executable by CMake)
	std::ifstream file("config.json");
	if (!file)
	{
		fmt::print(stderr, "Could not open config.json\n");
		return 1;
	}

	try
	{
		json config = json::parse(file);

		auto appName = config.at("app_name").get<std::string>();
		auto version = config.at("version").get<std::string>();
		bool debug = config.value("debug", false);
		int maxConnections = config.value("max_connections", 1);
		auto host = config.at("server").at("host").get<std::string>();
		int port = config.at("server").at("port").get<int>();
		auto features = config.at("features").get<std::vector<std::string>>();

		fmt::print("app: {} v{}\n", appName, version);
		fmt::print("debug: {}, max_connections: {}\n", debug, maxConnections);
		fmt::print("server: {}:{}\n", host, port);
		fmt::print("features: {}\n", features);
	}
	catch (const json::exception& e)
	{
		fmt::print(stderr, "Config error: {}\n", e.what());
		return 1;
	}

	// Start the libuv event loop with a one-shot timer so it has something to do
	uv_loop_t* loop = uv_default_loop();

	uv_timer_t timer;
	uv_timer_init(loop, &timer);
	uv_timer_start(&timer, [](uv_timer_t* handle) {
		fmt::print("libuv timer fired, stopping loop\n");
		uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
	}, 30000, 0);

	fmt::print("libuv {} loop running...\n", uv_version_string());
	uv_run(loop, UV_RUN_DEFAULT);
	uv_loop_close(loop);
	fmt::print("loop finished\n");
}
