#include "http/http_config.h"

#include <nlohmann/json.hpp>

namespace http
{

void from_json(const nlohmann::json& j, SslConfig& cfg)
{
	cfg.enabled = j.value("enabled", false);
	cfg.certificateFile = j.value("certificate_file", std::string{});
	cfg.keyFile = j.value("key_file", std::string{});
	cfg.minVersion = j.value("min_version", cfg.minVersion);
	cfg.cipherList = j.value("cipher_list", std::string{});
}

void from_json(const nlohmann::json& j, HttpConfig& cfg)
{
	cfg.host = j.at("host").get<std::string>();
	cfg.port = j.at("port").get<int>();
	cfg.http11 = j.value("http11", cfg.http11);
	cfg.http2 = j.value("http2", cfg.http2);
	cfg.http3 = j.value("http3", cfg.http3);
	if (j.contains("ssl"))
		cfg.ssl = j.at("ssl").get<SslConfig>();
}

} // namespace http
