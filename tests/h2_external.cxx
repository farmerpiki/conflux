// HTTP/2 end-to-end tests.
// Starts a real HTTPS/H2 server (TLS + ALPN "h2"), then exercises it with a
// minimal synchronous nghttp2 client over a blocking TLS socket.
// No external tools required — nghttp2 lib is used directly.
//
// Plain-TU intentionally (not a module unit).  See TRICKS.md #4.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <conflux/detail/discard.hxx>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.app;
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.http_server;
import conflux.net.router;
import conflux.net.tls;
import conflux.work;
import conflux.tests.external_support;

using conflux::work::WorkPool;
namespace {

// ---------------------------------------------------------------------------
// H2Response + H2Client
// ---------------------------------------------------------------------------

struct H2Response {
	int status = 0;
	std::string body;
	bool closed = false;
	std::uint32_t error_code = 0;
	bool connection_error = false;
	std::vector<std::pair<std::string, std::string>> trailers;
};
// Minimal synchronous nghttp2 client over a blocking TLS socket.
// Call get()/post() for serial requests; for concurrent streams use
// submit_get()/pump_all().
struct H2Client {
	// Internal body state for nghttp2 data provider.
	struct ReqBody {
		std::string data;
		std::size_t off{0};
		std::size_t frame_size{0};
	};
	// --- public API ---

	H2Client(H2Client const &) = delete;
	H2Client &operator =(H2Client const &) = delete;
	explicit H2Client(
		std::uint16_t port)
		: ctx_(SSL_CTX_new(TLS_client_method()))
		, fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
		SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);
		// Advertise "h2" in ALPN.
		static constexpr unsigned char kAlpn[] = "\x02h2";
		SSL_CTX_set_alpn_protos(ctx_.get(), kAlpn, sizeof(kAlpn) - 1);

		timeval tv{.tv_sec = 5, .tv_usec = 0};
		::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
			throw std::runtime_error{"H2Client: connect failed"};
		}

		ssl_.reset(SSL_new(ctx_.get()));
		SSL_set_fd(ssl_.get(), fd_);
		SSL_ctrl(
			ssl_.get(),
			SSL_CTRL_SET_TLSEXT_HOSTNAME,
			TLSEXT_NAMETYPE_host_name,
			const_cast<void *>(static_cast<void const *>("localhost")));
		if (SSL_connect(ssl_.get()) != 1) {
			throw std::runtime_error{"H2Client: TLS handshake failed"};
		}

		// Verify ALPN negotiated "h2".
		unsigned char const *proto = nullptr;
		unsigned int proto_len = 0;
		SSL_get0_alpn_selected(ssl_.get(), &proto, &proto_len);
		if (proto_len != 2 || proto[0] != 'h' || proto[1] != '2') {
			throw std::runtime_error{"H2Client: server did not negotiate h2"};
		}

		nghttp2_session_callbacks *cbs = nullptr;
		nghttp2_session_callbacks_new(&cbs);
		nghttp2_session_callbacks_set_send_callback(cbs, send_cb);
		nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
		nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_cb);
		nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_cb);
		nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_cb);
		nghttp2_option *opts = nullptr;
		nghttp2_option_new(&opts);
		nghttp2_option_set_no_http_messaging(opts, 1);
		nghttp2_session_client_new2(&session_, cbs, this, opts);
		nghttp2_option_del(opts);
		nghttp2_session_callbacks_del(cbs);

		// Send client connection preface (magic bytes + empty SETTINGS).
		nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
		nghttp2_session_send(session_);
	}
	~H2Client() {
		if (session_ != nullptr) {
			nghttp2_session_del(session_);
		}
		if (ssl_) {
			SSL_shutdown(ssl_.get());
			ssl_.reset();
		}
		ctx_.reset();
		if (fd_ >= 0) {
			::close(fd_);
		}
	}
	// Submit a GET and block until response received.
	H2Response get(
		std::string_view path) {
		std::int32_t const sid = submit_request("GET", path, nullptr);
		pump_until_closed(sid);
		return responses_[sid];
	}
	H2Response get_with_headers(
		std::string_view path,
		std::vector<std::pair<std::string, std::string>> extra_headers) {
		std::int32_t const sid = submit_request("GET", path, nullptr, std::move(extra_headers));
		pump_until_closed(sid);
		return responses_[sid];
	}
	H2Response raw_request(
		std::vector<std::pair<std::string, std::string>> headers) {
		std::int32_t const sid = submit_raw_headers(std::move(headers));
		pump_until_closed(sid);
		return responses_[sid];
	}
	// Submit a POST with body and block until response received.
	// ReqBody must outlive the pump — kept in req_bodies_ for stability.
	H2Response post(
		std::string_view path,
		std::string_view body_data) {
		return post_with_headers(path, body_data, {});
	}
	H2Response post_with_frame_size(
		std::string_view path,
		std::string_view body_data,
		std::size_t frame_size) {
		return post_with_headers_and_frame_size(path, body_data, {}, frame_size);
	}
	H2Response post_with_headers_and_frame_size(
		std::string_view path,
		std::string_view body_data,
		std::vector<std::pair<std::string, std::string>> extra_headers,
		std::size_t frame_size) {
		auto rb =
			std::make_unique<ReqBody>(ReqBody{.data = std::string{body_data}, .off = 0, .frame_size = frame_size});
		ReqBody *rb_ptr = rb.get();
		nghttp2_data_provider prd{};
		prd.read_callback = read_cb;
		prd.source.ptr = rb_ptr;

		std::int32_t const sid = submit_request("POST", path, &prd, std::move(extra_headers));
		req_bodies_.emplace(sid, std::move(rb));
		pump_until_closed(sid);
		return responses_[sid];
	}
	H2Response post_with_content_length(
		std::string_view path,
		std::string_view body_data,
		std::size_t content_length) {
		return post_with_headers(
			path,
			body_data,
			{
				{"content-length", std::to_string(content_length)}
        });
	}
	H2Response post_with_headers(
		std::string_view path,
		std::string_view body_data,
		std::vector<std::pair<std::string, std::string>> extra_headers) {
		auto rb = std::make_unique<ReqBody>(ReqBody{.data = std::string{body_data}, .off = 0});
		ReqBody *rb_ptr = rb.get();
		nghttp2_data_provider prd{};
		prd.read_callback = read_cb;
		prd.source.ptr = rb_ptr;

		std::int32_t const sid = submit_request("POST", path, &prd, std::move(extra_headers));
		req_bodies_.emplace(sid, std::move(rb)); // pointer still valid after move
		pump_until_closed(sid);
		return responses_[sid];
	}
	// Submit a GET without pumping (for concurrent-stream tests).
	std::int32_t submit_get(
		std::string_view path) {
		return submit_request("GET", path, nullptr);
	}
	// Pump until all listed streams are closed (or timeout).
	void pump_all(
		std::span<std::int32_t const> sids) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
		auto all_done = [&] { return std::ranges::all_of(sids, [&](std::int32_t s) { return responses_[s].closed; }); };
		while (!all_done() && std::chrono::steady_clock::now() < deadline) {
			pump_once();
		}
		REQUIRE(all_done());
	}
	std::map<std::int32_t, H2Response> responses_;

private:
	conflux::net_tls::UniqueSslCtx ctx_;
	conflux::net_tls::UniqueSsl ssl_;
	int fd_ = -1;
	nghttp2_session *session_ = nullptr;
	std::map<std::int32_t, std::unique_ptr<ReqBody>> req_bodies_;
	bool goaway_received_ = false;
	std::uint32_t goaway_error_code_ = 0;
	std::int32_t submit_request(
		std::string_view method,
		std::string_view path,
		nghttp2_data_provider const *prd,
		std::vector<std::pair<std::string, std::string>> extra_headers = {}) {
		std::string ms{method};
		std::string ps{path};
		std::vector<std::pair<std::string, std::string>> nv_store;
		nv_store.emplace_back(":method", ms);
		nv_store.emplace_back(":path", ps);
		nv_store.emplace_back(":scheme", "https");
		nv_store.emplace_back(":authority", "localhost");
		for (auto &h: extra_headers) {
			nv_store.push_back(std::move(h));
		}

		std::vector<nghttp2_nv> nva;
		nva.reserve(nv_store.size());
		for (auto &[n, v]: nv_store) {
			nva.push_back(
				{reinterpret_cast<std::uint8_t *>(n.data()),
				 reinterpret_cast<std::uint8_t *>(v.data()),
				 n.size(),
				 v.size(),
				 NGHTTP2_NV_FLAG_NONE});
		}

		std::int32_t const sid = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(), prd, nullptr);
		if (sid < 0) {
			throw std::runtime_error{"nghttp2_submit_request failed"};
		}
		return sid;
	}
	std::int32_t submit_raw_request(
		std::vector<std::pair<std::string, std::string>> headers,
		nghttp2_data_provider const *prd) {
		std::vector<nghttp2_nv> nva;
		nva.reserve(headers.size());
		for (auto &[n, v]: headers) {
			nva.push_back(
				{reinterpret_cast<std::uint8_t *>(n.data()),
				 reinterpret_cast<std::uint8_t *>(v.data()),
				 n.size(),
				 v.size(),
				 NGHTTP2_NV_FLAG_NONE});
		}
		std::int32_t const sid = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(), prd, nullptr);
		if (sid < 0) {
			throw std::runtime_error{"nghttp2_submit_request failed"};
		}
		return sid;
	}
	std::int32_t submit_raw_headers(
		std::vector<std::pair<std::string, std::string>> headers) {
		std::vector<nghttp2_nv> nva;
		nva.reserve(headers.size());
		for (auto &[n, v]: headers) {
			nva.push_back(
				{reinterpret_cast<std::uint8_t *>(n.data()),
				 reinterpret_cast<std::uint8_t *>(v.data()),
				 n.size(),
				 v.size(),
				 NGHTTP2_NV_FLAG_NONE});
		}
		std::int32_t const sid =
			nghttp2_submit_headers(session_, NGHTTP2_FLAG_END_STREAM, -1, nullptr, nva.data(), nva.size(), nullptr);
		if (sid < 0) {
			throw std::runtime_error{"nghttp2_submit_headers failed"};
		}
		return sid;
	}
	void pump_once() {
		nghttp2_session_send(session_);

		std::array<char, 16384> buf{};
		int const n = SSL_read(ssl_.get(), buf.data(), static_cast<int>(buf.size()));
		if (n > 0) {
			nghttp2_session_mem_recv(
				session_,
				reinterpret_cast<std::uint8_t const *>(buf.data()),
				static_cast<std::size_t>(n));
		}
		// n <= 0: timeout or close — caller checks stream state
	}
	void pump_until_closed(
		std::int32_t sid) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
		while (!responses_[sid].closed && !goaway_received_ && std::chrono::steady_clock::now() < deadline) {
			pump_once();
		}
		if (goaway_received_ && !responses_[sid].closed) {
			responses_[sid].closed = true;
			responses_[sid].connection_error = true;
			responses_[sid].error_code = goaway_error_code_;
		}
		REQUIRE(responses_[sid].closed);
	}
	// --- nghttp2 static callbacks ---

	static ssize_t send_cb(
		nghttp2_session * /*unused*/,
		std::uint8_t const *data,
		std::size_t length,
		int /*unused*/,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		int const n = SSL_write(c->ssl_.get(), data, static_cast<int>(length));
		return n > 0 ? static_cast<ssize_t>(n) : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
	}
	static int on_header_cb(
		nghttp2_session * /*unused*/,
		nghttp2_frame const *frame,
		std::uint8_t const *name,
		std::size_t namelen,
		std::uint8_t const *value,
		std::size_t valuelen,
		std::uint8_t /*unused*/,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		std::string_view const n{reinterpret_cast<char const *>(name), namelen};
		std::string_view const v{reinterpret_cast<char const *>(value), valuelen};
		if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
			// Trailer HEADERS frame (follows DATA frames) — capture all fields.
			c->responses_[frame->hd.stream_id].trailers.emplace_back(std::string{n}, std::string{v});
		} else if (n == ":status") {
			int st = 0;
			std::from_chars(v.data(), v.data() + v.size(), st);
			c->responses_[frame->hd.stream_id].status = st;
		}
		return 0;
	}
	static int on_data_chunk_cb(
		nghttp2_session * /*unused*/,
		std::uint8_t /*unused*/,
		std::int32_t stream_id,
		std::uint8_t const *data,
		std::size_t len,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		c->responses_[stream_id].body.append(reinterpret_cast<char const *>(data), len);
		return 0;
	}
	static int on_stream_close_cb(
		nghttp2_session * /*unused*/,
		std::int32_t stream_id,
		std::uint32_t error_code,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		c->responses_[stream_id].closed = true;
		c->responses_[stream_id].error_code = error_code;
		return 0;
	}
	static int on_frame_recv_cb(
		nghttp2_session * /*unused*/,
		nghttp2_frame const *frame,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		if (frame->hd.type == NGHTTP2_GOAWAY) {
			c->goaway_received_ = true;
			c->goaway_error_code_ = frame->goaway.error_code;
		}
		return 0;
	}
	static ssize_t read_cb(
		nghttp2_session * /*unused*/,
		std::int32_t /*unused*/,
		std::uint8_t *buf,
		std::size_t length,
		std::uint32_t *data_flags,
		nghttp2_data_source *source,
		void * /*unused*/) {
		auto &rb = *static_cast<ReqBody *>(source->ptr);
		auto remaining = rb.data.size() - rb.off;
		auto to_copy = (remaining < length ? remaining : length);
		if (rb.frame_size != 0 && to_copy > rb.frame_size) {
			to_copy = rb.frame_size;
		}
		std::memcpy(
			buf,
			rb.data.data() + rb.off, // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			to_copy);
		rb.off += to_copy;
		if (rb.off >= rb.data.size()) {
			*data_flags |= NGHTTP2_DATA_FLAG_EOF;
		}
		return static_cast<ssize_t>(to_copy);
	}
};
conflux::http::Router make_router() {
	conflux::http::Router r = conflux::tests::make_external_test_router();
	return r;
}

} // namespace
// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"h2: ALPN negotiates h2 (connection setup succeeds)") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	// H2Client constructor throws if ALPN does not yield "h2".
	REQUIRE_NOTHROW(H2Client{fx.port()});
}
TEST_CASE(
	"h2: GET /ping returns 200 with JSON body") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.get("/ping");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == R"({"ok":true})");
}
TEST_CASE(
	"h2: GET with path param echoes name") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.get("/hello/conflux");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "hello conflux");
}
TEST_CASE(
	"h2: POST body is echoed") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.post("/echo", "hello h2 world");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "hello h2 world");
}
TEST_CASE(
	"h2: POST body split into tiny DATA frames is echoed") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	std::string body(4096, 'h');
	auto resp = client.post_with_frame_size("/echo", body, 1);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == body);
}
TEST_CASE(
	"h2: streaming upload body reads DATA frames without buffered request body") {
	conflux::http::Router router;
	router.add_upload_context_with_timeout(
		"POST",
		"/stream-upload",
		nullptr,
		[](conflux::http::RequestView req,
		   conflux::http::RequestContext const &ctx) -> conflux::work::Task<conflux::http::Response> {
			conflux::http::UploadBody body{ctx.upload_body};
			std::string payload;
			while (true) {
				auto read = co_await body.read();
				if (!read) {
					co_return conflux::http::upload_error_response(read.error());
				}
				if (!*read) {
					break;
				}
				payload += (*read)->text_view();
			}
			co_return conflux::http::Response::text(
				std::format("{}:{}:{}:{}", req.body.empty(), req.form.size(), req.files.size(), payload));
		});
	conflux::tests::HttpsServerFixture const fx{std::move(router)};
	H2Client client{fx.port()};
	auto resp = client.post_with_headers_and_frame_size(
		"/stream-upload",
		"a=b",
		{
			{"content-type", "application/x-www-form-urlencoded"}
    },
		1);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "true:0:0:a=b");
}
TEST_CASE(
	"h2: streaming upload early response cancels remaining DATA") {
	conflux::http::Router router;
	router.add_upload_context_with_timeout(
		"POST",
		"/stream-upload",
		nullptr,
		[](conflux::http::RequestView, conflux::http::RequestContext const &)
			-> conflux::work::Task<conflux::http::Response> { co_return conflux::http::Response::text("done"); });
	conflux::tests::HttpsServerFixture const fx{std::move(router)};
	H2Client client{fx.port()};
	std::string body(64 * 1024, 'x');
	auto resp = client.post_with_frame_size("/stream-upload", body, 1024);
	REQUIRE(resp.closed);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
}
TEST_CASE(
	"h2: streaming upload immediate handler error cancels remaining DATA") {
	conflux::http::Router router;
	router.add_upload_context_with_timeout(
		"POST",
		"/stream-upload",
		nullptr,
		[](conflux::http::RequestView,
		   conflux::http::RequestContext const &) -> conflux::work::Task<conflux::http::Response> {
			co_return conflux::http::Response::content_too_large();
		});
	conflux::tests::HttpsServerFixture const fx{std::move(router)};
	H2Client client{fx.port()};
	std::string body(64 * 1024, 'x');
	auto resp = client.post_with_frame_size("/stream-upload", body, 1024);
	REQUIRE(resp.closed);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
	auto const metrics = fx.metrics();
	CHECK(metrics.uploads.canceled_by_handler == 1);
}
TEST_CASE(
	"h2: route-local upload body limit increments body-too-large metric") {
	auto observed = std::make_shared<std::atomic<int>>(-1);
	auto app = conflux::http::App::default_server();
	app.post(
		   "/stream-upload",
		   [observed](conflux::http::UploadBody body) -> conflux::work::Task<conflux::http::Response> {
			   while (true) {
				   auto read = co_await body.read();
				   if (!read) {
					   observed->store(static_cast<int>(read.error().kind), std::memory_order_release);
					   co_return conflux::http::upload_error_response(read.error());
				   }
				   if (!*read) {
					   break;
				   }
			   }
			   co_return conflux::http::Response::text("unexpected");
		   })
		.max_body_size(3);
	REQUIRE(app.validate().ok());
	conflux::tests::HttpsServerFixture const fx{std::move(conflux::http::router(app))};
	H2Client client{fx.port()};
	auto resp = client.post_with_frame_size("/stream-upload", "abcdef", 2);
	REQUIRE(resp.closed);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
	CHECK(
		observed->load(std::memory_order_acquire) == static_cast<int>(conflux::http::UploadErrorKind::body_too_large));
	auto const metrics = fx.metrics();
	CHECK(metrics.uploads.body_too_large == 1);
	CHECK(metrics.uploads.queue_backpressure_events == 0);
}
TEST_CASE(
	"h2: route-local upload content-length limit rejects before handler") {
	auto handler_started = std::make_shared<std::atomic<bool>>(false);
	auto app = conflux::http::App::default_server();
	app.post(
		   "/stream-upload",
		   [handler_started](conflux::http::UploadBody body) -> conflux::work::Task<conflux::http::Response> {
			   handler_started->store(true, std::memory_order_release);
			   auto discarded = co_await body.discard();
			   if (!discarded) {
				   co_return conflux::http::upload_error_response(discarded.error());
			   }
			   co_return conflux::http::Response::text("unexpected");
		   })
		.max_body_size(3);
	REQUIRE(app.validate().ok());
	conflux::tests::HttpsServerFixture const fx{std::move(conflux::http::router(app))};
	H2Client client{fx.port()};
	auto resp = client.post_with_content_length("/stream-upload", "", 6);
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
	CHECK_FALSE(handler_started->load(std::memory_order_acquire));
	auto const metrics = fx.metrics();
	CHECK(metrics.uploads.canceled_by_handler == 0);
}
TEST_CASE(
	"h2: content-length over max body resets stream") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.max_body_size = 8;
	conflux::tests::HttpsServerFixture const fx{cfg, make_router()};
	H2Client client{fx.port()};
	auto resp = client.post_with_content_length("/echo", "", 9);
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
}
TEST_CASE(
	"h2: DATA over max body resets stream") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.max_body_size = 8;
	conflux::tests::HttpsServerFixture const fx{cfg, make_router()};
	H2Client client{fx.port()};
	auto resp = client.post("/echo", "012345678");
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_CANCEL);
}
TEST_CASE(
	"h2: header count over parser limit resets stream") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.parser_limits.max_headers = 16;
	conflux::tests::HttpsServerFixture const fx{cfg, make_router()};
	H2Client client{fx.port()};
	auto ok = client.get("/ping");
	REQUIRE(ok.status == 200);
	std::vector<std::pair<std::string, std::string>> headers;
	for (int i = 0; i < 20; ++i) {
		headers.emplace_back(std::format("x-extra-{}", i), "1");
	}
	auto resp = client.get_with_headers("/ping", std::move(headers));
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_ENHANCE_YOUR_CALM);
}
TEST_CASE(
	"h2: header list bytes over parser limit resets stream") {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.parser_limits.max_header_block_size = 256;
	conflux::tests::HttpsServerFixture const fx{cfg, make_router()};
	H2Client client{fx.port()};
	auto ok = client.get("/ping");
	REQUIRE(ok.status == 200);
	auto resp = client.get_with_headers(
		"/ping",
		{
			{"x-large", std::string(256, 'x')}
    });
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_ENHANCE_YOUR_CALM);
}
TEST_CASE(
	"h2: duplicate pseudo-header resets stream") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.raw_request({
		{   ":method",       "GET"},
		{     ":path",     "/ping"},
		{     ":path",    "/other"},
		{   ":scheme",     "https"},
		{":authority", "localhost"}
    });
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_PROTOCOL_ERROR);
}
TEST_CASE(
	"h2: pseudo-header after regular header resets stream") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.raw_request({
		{   ":method",       "GET"},
		{  "x-before",         "1"},
		{     ":path",     "/ping"},
		{   ":scheme",     "https"},
		{":authority", "localhost"}
    });
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_PROTOCOL_ERROR);
}
TEST_CASE(
	"h2: forbidden connection header resets stream") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.get_with_headers(
		"/ping",
		{
			{"connection", "keep-alive"}
    });
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_PROTOCOL_ERROR);
}
TEST_CASE(
	"h2: invalid TE header resets stream") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.get_with_headers(
		"/ping",
		{
			{"te", "gzip"}
    });
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_PROTOCOL_ERROR);
}
TEST_CASE(
	"h2: content-length mismatch resets stream") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.post_with_content_length("/echo", "abc", 2);
	REQUIRE(resp.closed);
	CHECK(resp.status == 0);
	CHECK(resp.error_code == NGHTTP2_PROTOCOL_ERROR);
}
TEST_CASE(
	"h2: unknown route returns 404") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};
	auto resp = client.get("/does-not-exist");
	REQUIRE(resp.status == 404);
}
TEST_CASE(
	"h2: multiple sequential requests on same connection") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};

	for (int i = 0; i < 5; ++i) {
		auto resp = client.get("/ping");
		REQUIRE(resp.status == 200);
		REQUIRE(resp.body == R"({"ok":true})");
	}
}
TEST_CASE(
	"h2: multiple concurrent streams") {
	conflux::tests::HttpsServerFixture const fx{make_router()};
	H2Client client{fx.port()};

	// Submit three requests without pumping between them.
	std::array<std::int32_t, 3> sids{
		client.submit_get("/ping"),
		client.submit_get("/hello/world"),
		client.submit_get("/ping"),
	};

	client.pump_all(sids);

	REQUIRE(client.responses_[sids[0]].status == 200);
	REQUIRE(client.responses_[sids[0]].body == R"({"ok":true})");
	REQUIRE(client.responses_[sids[1]].status == 200);
	REQUIRE(client.responses_[sids[1]].body == "hello world");
	REQUIRE(client.responses_[sids[2]].status == 200);
	REQUIRE(client.responses_[sids[2]].body == R"({"ok":true})");
}
TEST_CASE(
	"h2: deferred response completes over HTTP/2") {
	auto pool = std::make_shared<WorkPool>();
	conflux::http::Router router;
	router.get("/deferred", [pool](conflux::http::OwnedRequest const &) {
		auto deferred = std::make_shared<conflux::http::DeferredResponse>();
		auto queued = pool->enqueue([deferred] {
			auto resp = conflux::http::Response::text("deferred h2 ok");
			resp.headers["x-deferred"] = "yes";
			deferred->complete(std::move(resp));
		});
		if (!queued) {
			return conflux::http::Response::internal_error("pool enqueue failed");
		}
		return conflux::http::Response::deferred(std::move(deferred));
	});
	conflux::tests::HttpsServerFixture const fx{std::move(router)};
	H2Client client{fx.port()};
	auto resp = client.get("/deferred");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "deferred h2 ok");
}
TEST_CASE(
	"h2: sync middleware over async app route completes inner deferred response") {
	auto app = conflux::http::App::default_server();
	app.use(
		[](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) { return next(req); });
	app.get(
		"/async-through-sync",
		[](conflux::http::RequestView const &) -> conflux::work::Task<conflux::http::Response> {
			co_return conflux::http::Response::text("async through sync middleware");
		});
	conflux::tests::HttpsServerFixture const fx{std::move(conflux::http::router(app))};
	H2Client client{fx.port()};
	auto resp = client.get("/async-through-sync");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "async through sync middleware");
}
TEST_CASE(
	"h2: SSE delivers all events over HTTP/2 before channel close") {
	conflux::http::Router r;
	r.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"ok":true})"); });
	r.sse("/events", [](conflux::http::OwnedRequest const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
		auto _ = ch->send("data: alpha\n\n");
		CONFLUX_DISCARD(ch->send("data: beta\n\n"));
		CONFLUX_DISCARD(ch->send("data: gamma\n\n"));
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{std::move(r)};
	H2Client client{fx.port()};
	auto resp = client.get("/events");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "data: alpha\n\ndata: beta\n\ndata: gamma\n\n");
	REQUIRE(resp.closed);
}
TEST_CASE(
	"h2: SSE send_event delivers typed event") {
	conflux::http::Router r;
	r.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"ok":true})"); });
	r.sse("/typed", [](conflux::http::OwnedRequest const &, std::shared_ptr<conflux::http::SseChannel> const &ch) {
		auto _ = ch->send_event("update", "payload42");
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{std::move(r)};
	H2Client client{fx.port()};
	auto resp = client.get("/typed");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "event: update\ndata: payload42\n\n");
	REQUIRE(resp.closed);
}
TEST_CASE(
	"h2: response trailers arrive after body") {
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::json(R"({"ok":true})");
	});
	router.get("/with-trailers", [](conflux::http::OwnedRequest const &) {
		conflux::http::Response resp;
		resp.status = 200;
		resp.status_text = "OK";
		resp.content_type = "text/plain; charset=utf-8";
		resp.set_text_body("hello trailers");
		resp.trailers = {
			{"x-checksum", "crc32:deadbeef"},
			{  "x-server",        "conflux"}
        };
		return resp;
	});
	conflux::tests::HttpsServerFixture const fx{std::move(router)};
	H2Client client{fx.port()};
	auto resp = client.get("/with-trailers");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body == "hello trailers");
	auto has_trailer = [&](std::string_view key, std::string_view val) {
		return std::ranges::any_of(resp.trailers, [&](auto const &kv) { return kv.first == key && kv.second == val; });
	};
	REQUIRE(has_trailer("x-checksum", "crc32:deadbeef"));
	REQUIRE(has_trailer("x-server", "conflux"));
}
TEST_CASE(
	"h2: large body (>65535 bytes) is fully received via flow control") {
	// Default H2 initial window is 65535 bytes.  A 128 KiB response forces the
	// server to pause and the client to send WINDOW_UPDATE before delivery completes.
	static constexpr std::size_t kBodySize = 128 * 1024;
	std::string large_body(kBodySize, 'X');
	conflux::http::Router r;
	r.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"ok":true})"); });
	r.get("/big", [&large_body](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(large_body);
	});
	conflux::tests::HttpsServerFixture const fx{std::move(r)};
	H2Client client{fx.port()};
	auto resp = client.get("/big");
	REQUIRE(resp.status == 200);
	REQUIRE(resp.body.size() == kBodySize);
	REQUIRE(resp.body == large_body);
}
TEST_CASE(
	"h2: HTTP/1.1 client can still connect to h2-capable server") {
	conflux::tests::HttpsServerFixture fx{make_router()};
	// Force an HTTP/1.1 client against the h2-capable TLS listener; must still get a 200.
	auto [status, body] = fx.curl_https("/ping");
	REQUIRE(status == 0);
	REQUIRE(body == R"({"ok":true})");
}
