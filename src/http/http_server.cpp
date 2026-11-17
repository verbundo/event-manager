#include "http/http_server.h"

#include <array>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>

namespace http
{

namespace
{

constexpr unsigned kShutdownPollMs = 100;
constexpr unsigned kShutdownTimeoutMs = 5000;

std::string opensslError()
{
	std::array<char, 256> buf{};
	ERR_error_string_n(ERR_get_error(), buf.data(), buf.size());
	return buf.data();
}

h2o_iovec_t iov(const char* s)
{
	return h2o_iovec_init(s, std::strlen(s));
}

int onRoot(h2o_handler_t*, h2o_req_t* req)
{
	// Only exact "/" — anything else falls through to h2o's 404
	if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) ||
		!h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT("/")))
		return -1;

	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, nullptr, H2O_STRLIT("application/json"));
	h2o_send_inline(req, H2O_STRLIT("{\"status\":\"ok\"}\n"));
	return 0;
}

h2o_pathconf_t* registerHandler(h2o_hostconf_t* hostconf, const char* path, int (*onReq)(h2o_handler_t*, h2o_req_t*))
{
	h2o_pathconf_t* pathconf = h2o_config_register_path(hostconf, path, 0);
	h2o_handler_t* handler = h2o_create_handler(pathconf, sizeof(*handler));
	handler->on_req = onReq;
	return pathconf;
}

} // namespace

struct HttpServer::Impl
{
	uv_loop_t* loop;
	HttpConfig config;

	h2o_globalconf_t globalconf{};
	h2o_context_t ctx{};
	h2o_accept_ctx_t acceptCtx{};
	uv_tcp_t listener{};
	uv_timer_t shutdownTimer{};
	SSL_CTX* sslCtx = nullptr;
	std::vector<h2o_iovec_t> alpn; // null-terminated list handed to h2o, must outlive sslCtx

	bool configInited = false;
	bool contextInited = false;
	bool listening = false;
	bool stopping = false;
	uint64_t shutdownStartedAt = 0;
	std::function<void()> onStopped;

	Impl(uv_loop_t* l, HttpConfig c) : loop(l), config(std::move(c))
	{
		listener.data = this;
		shutdownTimer.data = this;
	}

	~Impl()
	{
		if (contextInited)
			h2o_context_dispose(&ctx);
		if (configInited)
			h2o_config_dispose(&globalconf);
		if (sslCtx)
			SSL_CTX_free(sslCtx);
	}

	void setupHosts()
	{
		h2o_config_init(&globalconf);
		configInited = true;
		// HTTP/2 over cleartext (prior knowledge, "PRI * HTTP/2") only when http2 is enabled
		globalconf.http1.upgrade_to_http2 = config.http2 ? 1 : 0;

		h2o_hostconf_t* hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
		registerHandler(hostconf, "/", onRoot);
	}

	void setupSsl()
	{
		const SslConfig& ssl = config.ssl;

		sslCtx = SSL_CTX_new(TLS_server_method());
		if (!sslCtx)
			throw std::runtime_error("SSL_CTX_new failed: " + opensslError());

		int minVersion = TLS1_2_VERSION;
		if (ssl.minVersion == "TLSv1.3")
			minVersion = TLS1_3_VERSION;
		else if (ssl.minVersion != "TLSv1.2")
			throw std::runtime_error("ssl.min_version must be \"TLSv1.2\" or \"TLSv1.3\", got: " + ssl.minVersion);
		SSL_CTX_set_min_proto_version(sslCtx, minVersion);
		SSL_CTX_set_options(sslCtx, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);

		if (SSL_CTX_use_certificate_chain_file(sslCtx, ssl.certificateFile.c_str()) != 1)
			throw std::runtime_error("cannot load certificate " + ssl.certificateFile + ": " + opensslError());
		if (SSL_CTX_use_PrivateKey_file(sslCtx, ssl.keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
			throw std::runtime_error("cannot load private key " + ssl.keyFile + ": " + opensslError());
		if (SSL_CTX_check_private_key(sslCtx) != 1)
			throw std::runtime_error("private key does not match certificate: " + opensslError());
		if (!ssl.cipherList.empty() && SSL_CTX_set_cipher_list(sslCtx, ssl.cipherList.c_str()) != 1)
			throw std::runtime_error("invalid ssl.cipher_list: " + ssl.cipherList);

		// Advertise only the enabled protocols via ALPN (h2 preferred)
		if (config.http2)
			alpn.push_back(iov("h2"));
		if (config.http11)
			alpn.push_back(iov("http/1.1"));
		alpn.push_back(h2o_iovec_t{nullptr, 0});
		h2o_ssl_register_alpn_protocols(sslCtx, alpn.data());

		if (!config.http11)
		{
			// h2o falls back to HTTP/1.1 whenever ALPN does not select h2 (no ALPN extension, or no overlap).
			// With HTTP/1.1 disabled, fail those handshakes with a no_application_protocol alert instead.
			SSL_CTX_set_client_hello_cb(sslCtx, &Impl::onClientHelloRequireAlpn, nullptr);
			SSL_CTX_set_alpn_select_cb(sslCtx, &Impl::onAlpnSelectH2Only, nullptr);
		}

		acceptCtx.ssl_ctx = sslCtx;
	}

	// Rejects ClientHellos that carry no ALPN extension (such clients would be served HTTP/1.1)
	static int onClientHelloRequireAlpn(SSL* ssl, int* alert, void*)
	{
		const unsigned char* ext = nullptr;
		size_t extLen = 0;
		if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, &ext, &extLen) == 1)
			return SSL_CLIENT_HELLO_SUCCESS;
		*alert = SSL_AD_NO_APPLICATION_PROTOCOL;
		return SSL_CLIENT_HELLO_ERROR;
	}

	// Selects "h2" or aborts the handshake; never lets the connection proceed without a protocol
	static int onAlpnSelectH2Only(SSL*, const unsigned char** out, unsigned char* outLen, const unsigned char* in,
								  unsigned int inLen, void*)
	{
		static const unsigned char kH2[] = {2, 'h', '2'};
		if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLen, kH2, sizeof(kH2), in, inLen) ==
			OPENSSL_NPN_NEGOTIATED)
			return SSL_TLSEXT_ERR_OK;
		return SSL_TLSEXT_ERR_ALERT_FATAL;
	}

	void listen()
	{
		sockaddr_storage addr{};
		int r = uv_ip4_addr(config.host.c_str(), config.port, reinterpret_cast<sockaddr_in*>(&addr));
		if (r != 0)
			r = uv_ip6_addr(config.host.c_str(), config.port, reinterpret_cast<sockaddr_in6*>(&addr));
		if (r != 0)
			throw std::runtime_error("invalid http_server.host: " + config.host);

		uv_tcp_init(loop, &listener);
		listening = true;
		if ((r = uv_tcp_bind(&listener, reinterpret_cast<const sockaddr*>(&addr), 0)) != 0)
			throw std::runtime_error(std::format("bind {}:{} failed: {}", config.host, config.port, uv_strerror(r)));
		if ((r = uv_listen(reinterpret_cast<uv_stream_t*>(&listener), SOMAXCONN, &Impl::onAccept)) != 0)
			throw std::runtime_error(std::format("listen {}:{} failed: {}", config.host, config.port, uv_strerror(r)));
	}

	static void onAccept(uv_stream_t* server, int status)
	{
		if (status != 0)
			return;
		auto* self = static_cast<Impl*>(server->data);

		auto* conn = static_cast<uv_tcp_t*>(h2o_mem_alloc(sizeof(uv_tcp_t)));
		uv_tcp_init(server->loop, conn);
		if (uv_accept(server, reinterpret_cast<uv_stream_t*>(conn)) != 0)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(conn), reinterpret_cast<uv_close_cb>(free));
			return;
		}

		h2o_socket_t* sock = h2o_uv_socket_create(reinterpret_cast<uv_handle_t*>(conn), reinterpret_cast<uv_close_cb>(free));
		h2o_accept(&self->acceptCtx, sock);
	}

	bool hasConnections()
	{
		return !h2o_linklist_is_empty(&ctx._conns.active) || !h2o_linklist_is_empty(&ctx._conns.idle) ||
			   !h2o_linklist_is_empty(&ctx._conns.shutdown);
	}

	void stop(std::function<void()> cb)
	{
		if (stopping)
			return;
		stopping = true;
		onStopped = std::move(cb);

		if (listening)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(&listener), nullptr);
			listening = false;
		}
		if (!contextInited)
		{
			finishStop();
			return;
		}

		// Graceful shutdown: ask connections to close, then poll until they are gone (or time out)
		h2o_context_request_shutdown(&ctx);
		shutdownStartedAt = uv_now(loop);
		uv_timer_init(loop, &shutdownTimer);
		uv_timer_start(&shutdownTimer, &Impl::onShutdownPoll, 0, kShutdownPollMs);
	}

	static void onShutdownPoll(uv_timer_t* timer)
	{
		auto* self = static_cast<Impl*>(timer->data);
		bool timedOut = uv_now(self->loop) - self->shutdownStartedAt >= kShutdownTimeoutMs;
		if (self->hasConnections() && !timedOut)
			return;
		if (timedOut)
			std::print(stderr, "http: shutdown timed out with open connections\n");

		uv_timer_stop(timer);
		uv_close(reinterpret_cast<uv_handle_t*>(timer), nullptr);
		// Disposing the context closes h2o's own libuv handles (e.g. its async queue)
		h2o_context_dispose(&self->ctx);
		self->contextInited = false;
		self->finishStop();
	}

	void finishStop()
	{
		if (onStopped)
			std::exchange(onStopped, nullptr)();
	}
};

HttpServer::HttpServer(uv_loop_t* loop, HttpConfig config)
	: impl_(std::make_unique<Impl>(loop, std::move(config)))
{
}

HttpServer::~HttpServer() = default;

void HttpServer::start()
{
	const HttpConfig& cfg = impl_->config;

	if (!cfg.http11 && !cfg.http2)
		throw std::runtime_error("http_server: at least one of http11/http2 must be enabled");
	if (cfg.http3)
		std::print(stderr, "http: HTTP/3 is not supported by libh2o's libuv backend; ignoring \"http3\": true\n");
	if (!cfg.http11 && !cfg.ssl.enabled)
		std::print(stderr, "http: without TLS, HTTP/1.1 cannot be disabled (h2c prior knowledge still starts as HTTP/1)\n");

	impl_->setupHosts();
	if (cfg.ssl.enabled)
		impl_->setupSsl();

	h2o_context_init(&impl_->ctx, impl_->loop, &impl_->globalconf);
	impl_->contextInited = true;
	impl_->acceptCtx.ctx = &impl_->ctx;
	impl_->acceptCtx.hosts = impl_->globalconf.hosts;

	impl_->listen();

	std::print("http: listening on {}://{}:{} (http/1.1: {}, h2: {})\n", cfg.ssl.enabled ? "https" : "http", cfg.host,
			   cfg.port, cfg.http11, cfg.http2);
}

void HttpServer::stop(std::function<void()> onStopped)
{
	impl_->stop(std::move(onStopped));
}

} // namespace http
