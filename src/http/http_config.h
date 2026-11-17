#pragma once

#include <string>
#include <nlohmann/json_fwd.hpp>

namespace http
{

struct SslConfig
{
	bool enabled = false;
	std::string certificateFile;
	std::string keyFile;
	std::string minVersion = "TLSv1.2"; // "TLSv1.2" or "TLSv1.3"
	std::string cipherList;             // TLS 1.2 cipher list; empty = OpenSSL default
};

struct HttpConfig
{
	std::string host = "127.0.0.1";
	int port = 8080;
	bool http11 = true;
	bool http2 = true;
	bool http3 = false;
	SslConfig ssl;
};

// Parses the "http_server" section of config.json. Throws nlohmann::json::exception on bad input.
void from_json(const nlohmann::json& j, SslConfig& cfg);
void from_json(const nlohmann::json& j, HttpConfig& cfg);

} // namespace http
