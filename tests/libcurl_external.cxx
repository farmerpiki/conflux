// libcurl external compatibility and stress tests.
// Plain TU — libcurl is C-header heavy, and test files avoid module-interface
// TU-local leakage.  See TRICKS.md #4.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <curl/curl.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.tests.external_support;

namespace {

struct CurlGlobal {
	CurlGlobal() {
		CURLcode const rc = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (rc != CURLE_OK) {
			throw std::runtime_error{format("curl_global_init failed: {}", curl_easy_strerror(rc))};
		}
	}
	~CurlGlobal() { curl_global_cleanup(); }
	CurlGlobal(CurlGlobal const &) = delete;
	CurlGlobal &operator =(CurlGlobal const &) = delete;
};

CurlGlobal const &curl_global() {
	static CurlGlobal const global;
	return global;
}

struct CurlSlist {
	curl_slist *ptr = nullptr;
	~CurlSlist() {
		if (ptr != nullptr) {
			curl_slist_free_all(ptr);
		}
	}
	CurlSlist(CurlSlist const &) = delete;
	CurlSlist &operator =(CurlSlist const &) = delete;
	CurlSlist() = default;
	void append(
		std::string const &value) {
		curl_slist *next = curl_slist_append(ptr, value.c_str());
		if (next == nullptr) {
			throw std::runtime_error{"curl_slist_append failed"};
		}
		ptr = next;
	}
};

struct CurlRequest {
	std::string url;
	std::string method{"GET"};
	std::string body;
	long http_version = CURL_HTTP_VERSION_NONE;
	long ssl_version = CURL_SSLVERSION_DEFAULT;
	bool insecure_tls = true;
	bool fresh_connect = false;
	bool forbid_reuse = false;
	std::optional<std::string> resolve;
};

struct CurlResponse {
	CURLcode code = CURLE_FAILED_INIT;
	long status = 0;
	std::string body;
	long http_version = 0;
	long ssl_verify_result = 0;
	long num_connects = 0;
	double total_time = 0.0;
};

[[nodiscard]] size_t curl_write_cb(
	char *data,
	size_t size,
	size_t nmemb,
	void *userdata) {
	auto *body = static_cast<std::string *>(userdata);
	std::size_t const n = size * nmemb;
	body->append(data, n);
	return n;
}

[[nodiscard]] std::string curl_error(
	CURLcode code) {
	return curl_easy_strerror(code);
}

void setopt(
	CURL *easy,
	CURLoption option,
	long value) {
	CURLcode const rc = curl_easy_setopt(easy, option, value);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt failed: {}", curl_error(rc))};
	}
}

void setopt(
	CURL *easy,
	CURLoption option,
	char const *value) {
	CURLcode const rc = curl_easy_setopt(easy, option, value);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt failed: {}", curl_error(rc))};
	}
}

void setopt_off(
	CURL *easy,
	CURLoption option,
	curl_off_t value) {
	CURLcode const rc = curl_easy_setopt(easy, option, value);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt failed: {}", curl_error(rc))};
	}
}

void setopt_write_cb(
	CURL *easy) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt write callback failed: {}", curl_error(rc))};
	}
}

void setopt_write_data(
	CURL *easy,
	std::string *body) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_WRITEDATA, body);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt write data failed: {}", curl_error(rc))};
	}
}

void setopt_resolve(
	CURL *easy,
	curl_slist *resolve) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_RESOLVE, resolve);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt resolve failed: {}", curl_error(rc))};
	}
}

[[maybe_unused]] void setopt_private(
	CURL *easy,
	void *value) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_PRIVATE, value);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt private failed: {}", curl_error(rc))};
	}
}

class CurlEasy {
	CURL *easy_ = nullptr;

public:
	CurlEasy() {
		(void)curl_global();
		easy_ = curl_easy_init();
		if (easy_ == nullptr) {
			throw std::runtime_error{"curl_easy_init failed"};
		}
	}
	~CurlEasy() {
		if (easy_ != nullptr) {
			curl_easy_cleanup(easy_);
		}
	}
	CurlEasy(CurlEasy const &) = delete;
	CurlEasy &operator =(CurlEasy const &) = delete;
	[[gnu::pure]] [[nodiscard]] CURL *get() const noexcept { return easy_; }

	[[nodiscard]] CurlResponse perform(
		CurlRequest const &req) {
		curl_easy_reset(easy_);
		CurlResponse resp;
		CurlSlist resolve;
		if (req.resolve.has_value()) {
			resolve.append(*req.resolve);
			setopt_resolve(easy_, resolve.ptr);
		}

		setopt(easy_, CURLOPT_URL, req.url.c_str());
		setopt(easy_, CURLOPT_NOSIGNAL, 1L);
		setopt(easy_, CURLOPT_TIMEOUT_MS, 5000L);
		setopt(easy_, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
		setopt(easy_, CURLOPT_HTTP_VERSION, req.http_version);
		setopt(easy_, CURLOPT_SSLVERSION, req.ssl_version);
		setopt(easy_, CURLOPT_FRESH_CONNECT, req.fresh_connect ? 1L : 0L);
		setopt(easy_, CURLOPT_FORBID_REUSE, req.forbid_reuse ? 1L : 0L);
		if (req.insecure_tls) {
			setopt(easy_, CURLOPT_SSL_VERIFYPEER, 0L);
			setopt(easy_, CURLOPT_SSL_VERIFYHOST, 0L);
		}
		if (req.method == "POST") {
			setopt(easy_, CURLOPT_POST, 1L);
			setopt(easy_, CURLOPT_POSTFIELDS, req.body.c_str());
			setopt_off(easy_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
		}
		setopt_write_cb(easy_);
		setopt_write_data(easy_, &resp.body);

		resp.code = curl_easy_perform(easy_);
		(void)curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &resp.status);
		(void)curl_easy_getinfo(easy_, CURLINFO_HTTP_VERSION, &resp.http_version);
		(void)curl_easy_getinfo(easy_, CURLINFO_SSL_VERIFYRESULT, &resp.ssl_verify_result);
		(void)curl_easy_getinfo(easy_, CURLINFO_NUM_CONNECTS, &resp.num_connects);
		(void)curl_easy_getinfo(easy_, CURLINFO_TOTAL_TIME, &resp.total_time);
		return resp;
	}
};

class CurlMulti {
	CURLM *multi_ = nullptr;

public:
	CurlMulti() {
		(void)curl_global();
		multi_ = curl_multi_init();
		if (multi_ == nullptr) {
			throw std::runtime_error{"curl_multi_init failed"};
		}
	}
	~CurlMulti() {
		if (multi_ != nullptr) {
			curl_multi_cleanup(multi_);
		}
	}
	CurlMulti(CurlMulti const &) = delete;
	CurlMulti &operator =(CurlMulti const &) = delete;
	[[gnu::pure]] [[nodiscard]] CURLM *get() const noexcept { return multi_; }
};

[[nodiscard]] bool curl_feature(
	int bit) {
	auto const *info = curl_version_info(CURLVERSION_NOW);
	return info != nullptr && (info->features & bit) != 0;
}

[[nodiscard]] bool curl_has_http2() {
#ifdef CURL_VERSION_HTTP2
	return curl_feature(CURL_VERSION_HTTP2);
#else
	return false;
#endif
}

[[nodiscard]] bool curl_has_http3() {
#ifdef CURL_VERSION_HTTP3
	return curl_feature(CURL_VERSION_HTTP3);
#else
	return false;
#endif
}

[[nodiscard]] std::string http_url(
	std::uint16_t port,
	std::string_view path) {
	return format("http://127.0.0.1:{}{}", port, path);
}

[[nodiscard]] std::string https_url(
	std::uint16_t port,
	std::string_view path) {
	return format("https://127.0.0.1:{}{}", port, path);
}

[[nodiscard]] std::string h3_url(
	std::uint16_t port,
	std::string_view path) {
	return format("https://localhost:{}{}", port, path);
}

[[nodiscard]] std::string localhost_resolve(
	std::uint16_t port) {
	return format("localhost:{}:127.0.0.1", port);
}

void require_ok(
	CurlResponse const &resp,
	long status,
	std::string_view body) {
	INFO(format(
		"curl={} {} status={} http_version={} verify={} connects={} time={} body_size={} body={}",
		static_cast<int>(resp.code),
		curl_error(resp.code),
		resp.status,
		resp.http_version,
		resp.ssl_verify_result,
		resp.num_connects,
		resp.total_time,
		resp.body.size(),
		resp.body));
	REQUIRE(resp.code == CURLE_OK);
	REQUIRE(resp.status == status);
	REQUIRE(resp.body == body);
}

[[maybe_unused]] void require_contains(
	CurlResponse const &resp,
	long status,
	std::string_view needle) {
	INFO(format(
		"curl={} {} status={} http_version={} body_size={} body={}",
		static_cast<int>(resp.code),
		curl_error(resp.code),
		resp.status,
		resp.http_version,
		resp.body.size(),
		resp.body));
	REQUIRE(resp.code == CURLE_OK);
	REQUIRE(resp.status == status);
	REQUIRE(resp.body.find(needle) != std::string::npos);
}

[[maybe_unused]] void require_forced_http_version(
	CurlResponse const &resp,
	long expected) {
	INFO(format("effective http_version={} expected={}", resp.http_version, expected));
	REQUIRE(resp.http_version == expected);
}

[[nodiscard]] Router make_matrix_router() {
	Router r = conflux::tests::make_external_test_router();
	r.sse("/events", [](HttpRequest const &, std::shared_ptr<SseChannel> const &ch) {
		auto _ = ch->send("data: alpha\n\n");
		auto _ = ch->send("data: beta\n\n");
		ch->close();
	});
	return r;
}

[[maybe_unused]] void run_basic_case_matrix(
	CurlEasy &curl,
	std::uint16_t port,
	long http_version,
	bool tls) {
	auto url = [&](std::string_view path) { return tls ? https_url(port, path) : http_url(port, path); };
	CurlRequest req;
	req.http_version = http_version;
	req.url = url("/ping");
	auto resp = curl.perform(req);
	require_ok(resp, 200, R"({"ok":true})");

	req.url = url("/hello/libcurl");
	resp = curl.perform(req);
	require_ok(resp, 200, "hello libcurl");

	req.url = url("/echo");
	req.method = "POST";
	req.body = "hello libcurl";
	resp = curl.perform(req);
	require_ok(resp, 200, "hello libcurl");

	req.method = "GET";
	req.body.clear();
	req.url = url("/does-not-exist");
	resp = curl.perform(req);
	INFO(format(
		"curl={} {} status={} body_size={} body={}",
		static_cast<int>(resp.code),
		curl_error(resp.code),
		resp.status,
		resp.body.size(),
		resp.body));
	REQUIRE(resp.code == CURLE_OK);
	REQUIRE(resp.status == 404);
}

#ifndef CONFLUX_LIBCURL_STRESS_ONLY

TEST_CASE(
	"ext/libcurl: HTTP/1.0 plain compatibility matrix") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	run_basic_case_matrix(curl, fx.port(), CURL_HTTP_VERSION_1_0, false);
}

TEST_CASE(
	"ext/libcurl: HTTP/1.1 plain compatibility matrix") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	run_basic_case_matrix(curl, fx.port(), CURL_HTTP_VERSION_1_1, false);
}

TEST_CASE(
	"ext/libcurl: TLS default ALPN compatibility matrix") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	run_basic_case_matrix(curl, fx.port(), CURL_HTTP_VERSION_NONE, true);
}

TEST_CASE(
	"ext/libcurl: TLS forced HTTP/1.1 compatibility matrix") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	run_basic_case_matrix(curl, fx.port(), CURL_HTTP_VERSION_1_1, true);
	auto resp = curl.perform(CurlRequest{.url = https_url(fx.port(), "/ping"), .http_version = CURL_HTTP_VERSION_1_1});
	require_ok(resp, 200, R"({"ok":true})");
	require_forced_http_version(resp, CURL_HTTP_VERSION_1_1);
}

	#if CONFLUX_HAS_HTTP2
TEST_CASE(
	"ext/libcurl: TLS forced HTTP/2 compatibility matrix") {
	if (!curl_has_http2()) {
		WARN("installed libcurl lacks HTTP/2 support");
		return;
	}
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	run_basic_case_matrix(curl, fx.port(), CURL_HTTP_VERSION_2_0, true);
	auto resp = curl.perform(CurlRequest{.url = https_url(fx.port(), "/ping"), .http_version = CURL_HTTP_VERSION_2_0});
	require_ok(resp, 200, R"({"ok":true})");
	require_forced_http_version(resp, CURL_HTTP_VERSION_2_0);
}
	#endif

	#if CONFLUX_HAS_HTTP3 && defined(CURL_HTTP_VERSION_3ONLY)
TEST_CASE(
	"ext/libcurl: H3 forced GET /ping") {
	if (!curl_has_http3()) {
		WARN("installed libcurl lacks HTTP/3 support");
		return;
	}
	conflux::tests::Http3ServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	CurlRequest req;
	req.url = h3_url(fx.port(), "/ping");
	req.http_version = CURL_HTTP_VERSION_3ONLY;
	req.resolve = localhost_resolve(fx.port());
	auto resp = curl.perform(req);
	require_ok(resp, 200, R"({"ok":true})");
		#if defined(CURL_HTTP_VERSION_3)
	require_forced_http_version(resp, CURL_HTTP_VERSION_3);
		#else
	INFO("installed libcurl headers do not expose CURL_HTTP_VERSION_3");
		#endif
}
	#endif

TEST_CASE(
	"ext/libcurl: TLS forced versions GET /ping") {
	#if !defined(CURL_SSLVERSION_MAX_TLSv1_2)
	WARN("installed libcurl headers do not expose TLS max-version controls");
	return;
	#else
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	CurlRequest req;
	req.url = https_url(fx.port(), "/ping");
	req.http_version = CURL_HTTP_VERSION_1_1;
	req.ssl_version = CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2;
	auto resp = curl.perform(req);
	require_ok(resp, 200, R"({"ok":true})");

		#if defined(CURL_SSLVERSION_TLSv1_3) && defined(CURL_SSLVERSION_MAX_TLSv1_3)
	req.ssl_version = CURL_SSLVERSION_TLSv1_3 | CURL_SSLVERSION_MAX_TLSv1_3;
	resp = curl.perform(req);
	require_ok(resp, 200, R"({"ok":true})");
		#else
	WARN("installed libcurl headers do not expose TLS 1.3 controls");
		#endif
	#endif
}

TEST_CASE(
	"ext/libcurl: HTTP and HTTPS same port") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	auto resp = curl.perform(CurlRequest{.url = http_url(fx.port(), "/ping"), .http_version = CURL_HTTP_VERSION_1_1});
	require_ok(resp, 200, R"({"ok":true})");
	resp = curl.perform(CurlRequest{.url = https_url(fx.port(), "/ping"), .http_version = CURL_HTTP_VERSION_1_1});
	require_ok(resp, 200, R"({"ok":true})");
}

TEST_CASE(
	"ext/libcurl: SSE short stream closes cleanly") {
	conflux::tests::HttpsServerFixture const fx{make_matrix_router()};
	CurlEasy curl;
	auto resp =
		curl.perform(CurlRequest{.url = https_url(fx.port(), "/events"), .http_version = CURL_HTTP_VERSION_1_1});
	require_contains(resp, 200, "data: alpha\n\n");
	REQUIRE(resp.body.find("data: beta\n\n") != std::string::npos);
}

TEST_CASE(
	"ext/libcurl: large static body over TLS") {
	char dir_template[] = "/tmp/conflux_libcurl_static_XXXXXX";
	char *const dir_ptr = ::mkdtemp(dir_template);
	REQUIRE(dir_ptr != nullptr);
	std::string const dir{dir_ptr};
	std::string const body(256UL * 1024, 'L');
	{
		std::ofstream out{dir + "/large.bin", std::ios::binary};
		out << body;
	}

	Router router;
	router.serve_static("/static", dir);
	conflux::tests::HttpsServerFixture const fx{move(router)};
	CurlEasy curl;
	auto resp = curl.perform(
		CurlRequest{.url = https_url(fx.port(), "/static/large.bin"), .http_version = CURL_HTTP_VERSION_1_1});
	std::filesystem::remove_all(dir);
	require_ok(resp, 200, body);
}

#else // CONFLUX_LIBCURL_STRESS_ONLY

enum class TortureVersion {
	Default, // CTest-stable default: HTTP/1.1; opt into H2/H3/mixed with env.
	Http11,
	Http2,
	Http3,
	Mixed,
};

[[nodiscard]] unsigned env_uint(
	char const *name,
	unsigned fallback) {
	char const *raw = std::getenv(name);
	if (raw == nullptr || *raw == '\0') {
		return fallback;
	}
	char *end = nullptr;
	unsigned long const parsed = std::strtoul(raw, &end, 10);
	if (end == raw || *end != '\0') {
		return fallback;
	}
	return static_cast<unsigned>(parsed);
}

[[nodiscard]] bool env_bool(
	char const *name,
	bool fallback) {
	char const *raw = std::getenv(name);
	if (raw == nullptr || *raw == '\0') {
		return fallback;
	}
	return std::string_view{raw} == "1" || std::string_view{raw} == "true" || std::string_view{raw} == "on";
}

[[nodiscard]] TortureVersion env_version() {
	char const *raw = std::getenv("CONFLUX_CURL_TORTURE_HTTP_VERSION");
	std::string_view const value = raw == nullptr ? std::string_view{"default"} : std::string_view{raw};
	if (value == "1.1") {
		return TortureVersion::Http11;
	}
	if (value == "2") {
		return TortureVersion::Http2;
	}
	if (value == "3") {
		return TortureVersion::Http3;
	}
	if (value == "mixed") {
		return TortureVersion::Mixed;
	}
	return TortureVersion::Default;
}

[[nodiscard]] Router make_stress_router() {
	Router r = make_matrix_router();
	auto large = make_shared<std::string>(128UL * 1024, 'S');
	r.get("/static/large.bin", [large](HttpRequest const &) { return HttpResponse::text(*large); });
	return r;
}

struct ExpectedCurlRequest {
	CurlRequest request;
	long status = 200;
	std::string expected_body;
	std::optional<std::string> expected_contains;
};

[[nodiscard]] long picked_http_version(
	TortureVersion mode,
	unsigned i) {
	switch (mode) {
	case TortureVersion::Http11: return CURL_HTTP_VERSION_1_1;
	case TortureVersion::Http2 : return CURL_HTTP_VERSION_2_0;
	case TortureVersion::Mixed:
		if (i % 3U == 0U) {
			return CURL_HTTP_VERSION_NONE;
		}
		if (i % 3U == 1U || !curl_has_http2()) {
			return CURL_HTTP_VERSION_1_1;
		}
		return CURL_HTTP_VERSION_2_0;
	case TortureVersion::Default: return CURL_HTTP_VERSION_1_1;
	case TortureVersion::Http3  : return CURL_HTTP_VERSION_NONE;
	}
	return CURL_HTTP_VERSION_NONE;
}

[[nodiscard]] ExpectedCurlRequest make_expected_request(
	std::uint16_t port,
	unsigned i,
	TortureVersion version,
	bool fresh) {
	ExpectedCurlRequest out;
	out.request.http_version = picked_http_version(version, i);
	out.request.fresh_connect = fresh;
	out.request.forbid_reuse = fresh;

	if (version == TortureVersion::Http3) {
	#ifdef CURL_HTTP_VERSION_3ONLY
		out.request.url = h3_url(port, "/ping");
		out.request.http_version = CURL_HTTP_VERSION_3ONLY;
		out.request.resolve = localhost_resolve(port);
		out.expected_body = R"({"ok":true})";
	#else
		out.request.url = https_url(port, "/ping");
		out.expected_body = R"({"ok":true})";
	#endif
		return out;
	}

	switch (i % 5U) {
	case 0:
		out.request.url = https_url(port, "/ping");
		out.expected_body = R"({"ok":true})";
		break;
	case 1:
		out.request.url = https_url(port, "/hello/stress");
		out.expected_body = "hello stress";
		break;
	case 2:
		out.request.url = https_url(port, "/echo");
		out.request.method = "POST";
		out.request.body = "stress echo";
		out.expected_body = "stress echo";
		break;
	case 3:
		out.request.url = https_url(port, "/static/large.bin");
		out.expected_body = std::string(128UL * 1024, 'S');
		break;
	default:
		out.request.url = https_url(port, "/events");
		// SSE deliberately closes the connection after the terminating chunk.
		// Tell libcurl not to pool that easy handle afterwards so the next
		// sequential stress iteration never races a peer-side TLS close_notify.
		out.request.forbid_reuse = true;
		out.expected_contains = "data: alpha\n\n";
		break;
	}
	return out;
}

void require_expected(
	CurlResponse const &resp,
	ExpectedCurlRequest const &expected) {
	INFO(format(
		"curl={} {} status={} expected_status={} http_version={} body_size={} url={}",
		static_cast<int>(resp.code),
		curl_error(resp.code),
		resp.status,
		expected.status,
		resp.http_version,
		resp.body.size(),
		expected.request.url));
	REQUIRE(resp.code == CURLE_OK);
	REQUIRE(resp.status == expected.status);
	if (expected.expected_contains.has_value()) {
		REQUIRE(resp.body.find(*expected.expected_contains) != std::string::npos);
	} else {
		REQUIRE(resp.body == expected.expected_body);
	}
}

struct AbortAfter {
	std::size_t limit = 0;
	std::size_t seen = 0;
};

[[nodiscard]] size_t abort_after_cb(
	char *,
	size_t size,
	size_t nmemb,
	void *userdata) {
	auto *state = static_cast<AbortAfter *>(userdata);
	std::size_t const n = size * nmemb;
	if (state->seen + n > state->limit) {
		return 0;
	}
	state->seen += n;
	return n;
}

void setopt_abort_cb(
	CURL *easy) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, abort_after_cb);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt abort callback failed: {}", curl_error(rc))};
	}
}

void setopt_abort_data(
	CURL *easy,
	AbortAfter *state) {
	CURLcode const rc = curl_easy_setopt(easy, CURLOPT_WRITEDATA, state);
	if (rc != CURLE_OK) {
		throw std::runtime_error{format("curl_easy_setopt abort data failed: {}", curl_error(rc))};
	}
}

[[nodiscard]] bool torture_supported(
	TortureVersion version) {
	if (version == TortureVersion::Http2 && !curl_has_http2()) {
		WARN("CONFLUX_CURL_TORTURE_HTTP_VERSION=2 requested, but libcurl lacks HTTP/2");
		return false;
	}
	if (version == TortureVersion::Http3) {
	#if !CONFLUX_HAS_HTTP3 || !defined(CURL_HTTP_VERSION_3ONLY)
		WARN("CONFLUX_CURL_TORTURE_HTTP_VERSION=3 requested, but HTTP/3 is unavailable in this build");
		return false;
	#else
		if (!curl_has_http3()) {
			WARN("CONFLUX_CURL_TORTURE_HTTP_VERSION=3 requested, but libcurl lacks HTTP/3");
			return false;
		}
	#endif
	}
	return true;
}

TEST_CASE(
	"ext/libcurl/stress: sequential requests") {
	unsigned const iters = env_uint("CONFLUX_CURL_TORTURE_ITERS", 1000U);
	bool const fresh = env_bool("CONFLUX_CURL_TORTURE_FRESH_CONNECT", true);
	TortureVersion const version = env_version();
	if (!torture_supported(version)) {
		return;
	}

	Router router = make_stress_router();
	#if CONFLUX_HAS_HTTP3 && defined(CURL_HTTP_VERSION_3ONLY)
	if (version == TortureVersion::Http3) {
		conflux::tests::Http3ServerFixture const fx{move(router)};
		CurlEasy curl;
		for (unsigned i = 0; i < iters; ++i) {
			auto expected = make_expected_request(fx.port(), i, version, fresh);
			require_expected(curl.perform(expected.request), expected);
		}
		return;
	}
	#endif
	conflux::tests::HttpsServerFixture const fx{move(router)};
	CurlEasy curl;
	for (unsigned i = 0; i < iters; ++i) {
		auto expected = make_expected_request(fx.port(), i, version, fresh);
		require_expected(curl.perform(expected.request), expected);
	}
}

TEST_CASE(
	"ext/libcurl/stress: parallel multi-interface mixed routes") {
	unsigned const iters = env_uint("CONFLUX_CURL_TORTURE_ITERS", 1000U);
	unsigned const concurrency = max(1U, env_uint("CONFLUX_CURL_TORTURE_CONCURRENCY", 32U));
	// Keep the default stress target on request handling/protocol behavior, not
	// libcurl's TLS connection cache. Set the env var to 0 when explicitly
	// probing keep-alive reuse behavior.
	bool const fresh = env_bool("CONFLUX_CURL_TORTURE_FRESH_CONNECT", true);
	TortureVersion const version = env_version();
	if (!torture_supported(version)) {
		return;
	}
	if (version == TortureVersion::Http3) {
		WARN(
			"multi-interface HTTP/3 stress is intentionally left to curl CLI smoke until local libcurl H3 support is "
			"stable");
		return;
	}

	conflux::tests::HttpsServerFixture const fx{make_stress_router()};
	CurlMulti multi;
	struct Active {
		CURLM *multi = nullptr;
		CURL *easy = nullptr;
		bool added = false;
		std::unique_ptr<ExpectedCurlRequest> expected;
		std::string body;

		~Active() {
			if (easy == nullptr) {
				return;
			}
			if (added && multi != nullptr) {
				(void)curl_multi_remove_handle(multi, easy);
			}
			curl_easy_cleanup(easy);
		}
		Active() = default;
		Active(Active const &) = delete;
		Active &operator =(Active const &) = delete;
	};
	std::vector<std::unique_ptr<Active>> owned;
	owned.reserve(concurrency);
	auto add_one = [&](unsigned index) {
		auto active = make_unique<Active>();
		active->multi = multi.get();
		active->easy = curl_easy_init();
		REQUIRE(active->easy != nullptr);
		active->expected = make_unique<ExpectedCurlRequest>(make_expected_request(fx.port(), index, version, fresh));
		CurlRequest const &req = active->expected->request;
		setopt(active->easy, CURLOPT_URL, req.url.c_str());
		setopt(active->easy, CURLOPT_NOSIGNAL, 1L);
		setopt(active->easy, CURLOPT_TIMEOUT_MS, 5000L);
		setopt(active->easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
		setopt(active->easy, CURLOPT_HTTP_VERSION, req.http_version);
		setopt(active->easy, CURLOPT_SSL_VERIFYPEER, 0L);
		setopt(active->easy, CURLOPT_SSL_VERIFYHOST, 0L);
		setopt(active->easy, CURLOPT_FRESH_CONNECT, req.fresh_connect ? 1L : 0L);
		setopt(active->easy, CURLOPT_FORBID_REUSE, req.forbid_reuse ? 1L : 0L);
		if (req.method == "POST") {
			setopt(active->easy, CURLOPT_POST, 1L);
			setopt(active->easy, CURLOPT_POSTFIELDS, req.body.c_str());
			setopt_off(active->easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
		}
		setopt_write_cb(active->easy);
		setopt_write_data(active->easy, &active->body);
		setopt_private(active->easy, active.get());
		CURLMcode const add_rc = curl_multi_add_handle(multi.get(), active->easy);
		REQUIRE(add_rc == CURLM_OK);
		active->added = true;
		owned.push_back(move(active));
	};

	unsigned launched = 0;
	unsigned completed = 0;
	while (launched < iters && launched < concurrency) {
		add_one(launched++);
	}
	int running = 0;
	while (completed < iters) {
		CURLMcode const perform_rc = curl_multi_perform(multi.get(), &running);
		REQUIRE(perform_rc == CURLM_OK);
		int numfds = 0;
		CURLMcode const wait_rc = curl_multi_poll(multi.get(), nullptr, 0, 1000, &numfds);
		(void)numfds;
		REQUIRE(wait_rc == CURLM_OK);

		int queued = 0;
		while (CURLMsg *msg = curl_multi_info_read(multi.get(), &queued)) {
			if (msg->msg != CURLMSG_DONE) {
				continue;
			}
			char *private_raw = nullptr;
			CURLcode const info_rc = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &private_raw);
			REQUIRE(info_rc == CURLE_OK);
			auto *active = reinterpret_cast<Active *>(private_raw);
			REQUIRE(active != nullptr);
			CurlResponse resp;
			resp.code = msg->data.result;
			resp.body = active->body;
			(void)curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &resp.status);
			(void)curl_easy_getinfo(msg->easy_handle, CURLINFO_HTTP_VERSION, &resp.http_version);
			require_expected(resp, *active->expected);
			CURLMcode const remove_rc = curl_multi_remove_handle(multi.get(), msg->easy_handle);
			REQUIRE(remove_rc == CURLM_OK);
			active->added = false;
			curl_easy_cleanup(msg->easy_handle);
			active->easy = nullptr;
			auto const found = std::ranges::find_if(owned, [&](std::unique_ptr<Active> const &candidate) {
				return candidate.get() == active;
			});
			REQUIRE(found != owned.end());
			owned.erase(found);
			++completed;
			if (launched < iters) {
				add_one(launched++);
			}
		}
	}
}

TEST_CASE(
	"ext/libcurl/stress: early-close client abort does not poison listener") {
	TortureVersion const version = env_version();
	if (version == TortureVersion::Http3) {
		WARN("early-close stress uses TLS/TCP; HTTP/3 remains covered by curl --http3-only smoke");
		return;
	}
	if (!torture_supported(version)) {
		return;
	}

	conflux::tests::HttpsServerFixture const fx{make_stress_router()};
	CurlEasy abort_curl;
	CURL *easy = abort_curl.get();
	curl_easy_reset(easy);
	AbortAfter abort{.limit = 64, .seen = 0};
	std::string const url = https_url(fx.port(), "/static/large.bin");
	setopt(easy, CURLOPT_URL, url.c_str());
	setopt(easy, CURLOPT_NOSIGNAL, 1L);
	setopt(easy, CURLOPT_TIMEOUT_MS, 5000L);
	setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
	setopt(easy, CURLOPT_HTTP_VERSION, picked_http_version(version, 0));
	setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
	setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
	setopt_abort_cb(easy);
	setopt_abort_data(easy, &abort);
	CURLcode const rc = curl_easy_perform(easy);
	REQUIRE(rc == CURLE_WRITE_ERROR);

	CurlEasy curl;
	auto expected = make_expected_request(fx.port(), 0, version, false);
	require_expected(curl.perform(expected.request), expected);
}

#endif // CONFLUX_LIBCURL_STRESS_ONLY

} // namespace
