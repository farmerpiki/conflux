module;
#include <cassert>
#include <time.h>

export module conflux.net.http.request;
import std;
import conflux.types;
import conflux.net.http.types;
export namespace conflux::http {

// ─── ClientRequest ──────────────────────────────────────────────────────────────

class ClientRequest {
public:
	class Builder;

	static Builder get(std::string_view url);
	static Builder post(std::string_view url);
	static Builder put(std::string_view url);
	static Builder patch(std::string_view url);
	static Builder del(std::string_view url);
	static Builder head(std::string_view url);
	static Builder method(std::string_view m, std::string_view url);
	[[nodiscard]] std::string_view method() const noexcept { return method_; }
	[[nodiscard]] Url const &url() const noexcept { return url_; }
	[[nodiscard]] HttpFields const &headers() const noexcept { return headers_; }
	[[nodiscard]] std::string const &body() const noexcept { return body_; }
	[[nodiscard]] HttpTimeouts timeouts() const noexcept { return timeouts_; }
	[[nodiscard]] bool verify_peer() const noexcept { return verify_peer_; }
	[[nodiscard]] std::string_view server_name() const noexcept { return server_name_; }
	[[nodiscard]] int max_redirects() const noexcept { return max_redirects_; }

private:
	friend class Builder;

	std::string method_{"GET"};
	Url url_{};
	HttpFields headers_{true}; // case-insensitive
	std::string body_{};
	HttpTimeouts timeouts_{};
	bool verify_peer_{true};
	std::string server_name_{};
	int max_redirects_{0};

	explicit ClientRequest() = default;
};
// ─── ClientRequest::Builder ─────────────────────────────────────────────────────

class ClientRequest::Builder {
	ClientRequest req_;
	bool body_set_{false};
	static Url parse_or_throw(
		std::string_view raw) {
		auto r = Url::parse(raw);
		if (!r) {
			throw std::invalid_argument(format("invalid URL: {}", r.error().message));
		}
		return move(*r);
	}
	void assert_single_body() {
#ifndef NDEBUG
		assert(!body_set_ && "body set twice without clear_body()");
#endif
		body_set_ = true;
	}

public:
	explicit Builder(
		std::string_view method_str,
		std::string_view url_raw) {
		req_.method_ = std::string{method_str};
		req_.url_ = parse_or_throw(url_raw);
	}
	explicit Builder(
		std::string_view method_str,
		Url url) {
		req_.method_ = std::string{method_str};
		req_.url_ = move(url);
	}
	// Implicit conversion: Builder&& → ClientRequest (no-copy).
	operator ClientRequest() && { return move(req_); } // NOLINT(google-explicit-constructor)
	[[nodiscard]] ClientRequest build() && { return move(req_); }
	// ── verbs / URL ──────────────────────────────────────────────────────────

	Builder &method(
		std::string_view m) & {
		req_.method_ = std::string{m};
		return *this;
	}
	Builder &url(
		std::string_view raw) & {
		req_.url_ = parse_or_throw(raw);
		return *this;
	}
	[[nodiscard]] expected<void, UrlError> try_url(
		std::string_view raw) & {
		auto parsed = Url::parse(raw);
		if (!parsed) {
			return unexpected{move(parsed.error())};
		}
		req_.url_ = move(*parsed);
		return {};
	}
	Builder &url(
		Url u) & {
		req_.url_ = move(u);
		return *this;
	}
	Builder &&method(
		std::string_view m) && {
		return move(method(m));
	}
	Builder &&url(
		std::string_view raw) && {
		return move(url(raw));
	}
	Builder &&url(
		Url u) && {
		return move(url(move(u)));
	}
	// ── query ─────────────────────────────────────────────────────────────────

	Builder &query(
		std::string_view name,
		std::string_view value) & {
		req_.url_.set_query_param(name, value);
		return *this;
	}
	Builder &query_params(
		HttpFields const &kv) & {
		for (auto const &[k, v]: kv) {
			req_.url_.set_query_param(k, v);
		}
		return *this;
	}
	Builder &&query(
		std::string_view name,
		std::string_view value) && {
		return move(query(name, value));
	}
	Builder &&query_params(
		HttpFields const &kv) && {
		return move(query_params(kv));
	}
	// ── headers ───────────────────────────────────────────────────────────────

	Builder &header(
		std::string_view name,
		std::string_view value) & {
		req_.headers_.set(std::string{name}, std::string{value});
		return *this;
	}
	Builder &headers(
		HttpFields const &h) & {
		for (auto const &[k, v]: h) {
			req_.headers_.set(k, v);
		}
		return *this;
	}
	Builder &bearer(
		std::string_view token) & {
		return header("Authorization", format("Bearer {}", token));
	}
	Builder &basic(
		std::string_view user,
		std::string_view pass) & {
		// Base64-encode user:pass.
		auto const creds = format("{}:{}", user, pass);
		// Simple base64 without external lib.
		static constexpr std::string_view kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string b64;
		b64.reserve(((creds.size() + 2) / 3) * 4);
		for (std::size_t i = 0; i < creds.size(); i += 3) {
			auto const a = static_cast<unsigned char>(creds[i]);
			auto const b = (i + 1 < creds.size()) ? static_cast<unsigned char>(creds[i + 1]) : 0u;
			auto const c = (i + 2 < creds.size()) ? static_cast<unsigned char>(creds[i + 2]) : 0u;
			b64 += kAlpha[(static_cast<unsigned>(a) >> 2) & 0x3Fu];
			b64 += kAlpha[((static_cast<unsigned>(a) << 4) | (b >> 4)) & 0x3Fu];
			b64 += (i + 1 < creds.size()) ? kAlpha[((b << 2) | (c >> 6)) & 0x3Fu] : '=';
			b64 += (i + 2 < creds.size()) ? kAlpha[c & 0x3Fu] : '=';
		}
		return header("Authorization", format("Basic {}", b64));
	}
	Builder &user_agent(
		std::string_view ua) & {
		return header("User-Agent", ua);
	}
	Builder &accept(
		std::string_view mime) & {
		return header("Accept", mime);
	}
	Builder &accept_json() & { return accept("application/json"); }
	Builder &content_type(
		std::string_view ct) & {
		return header("Content-Type", ct);
	}
	Builder &if_match(
		std::string_view etag) & {
		return header("If-Match", etag);
	}
	Builder &if_none_match(
		std::string_view etag) & {
		return header("If-None-Match", etag);
	}
	Builder &if_modified_since(
		std::chrono::system_clock::time_point tp) & {
		// RFC 9110 HTTP-date format.
		auto const tt = std::chrono::system_clock::to_time_t(tp);
		tm gmt{};
		gmtime_r(&tt, &gmt);
		std::array<char, 32> buf{};
		strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
		return header("If-Modified-Since", buf.data());
	}
	Builder &if_unmodified_since(
		std::chrono::system_clock::time_point tp) & {
		auto const tt = std::chrono::system_clock::to_time_t(tp);
		tm gmt{};
		gmtime_r(&tt, &gmt);
		std::array<char, 32> buf{};
		strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
		return header("If-Unmodified-Since", buf.data());
	}
	Builder &&header(
		std::string_view name,
		std::string_view value) && {
		return move(header(name, value));
	}
	Builder &&headers(
		HttpFields h) && {
		return move(headers(move(h)));
	}
	Builder &&bearer(
		std::string_view token) && {
		return move(bearer(token));
	}
	Builder &&basic(
		std::string_view user,
		std::string_view pass) && {
		return move(basic(user, pass));
	}
	Builder &&user_agent(
		std::string_view ua) && {
		return move(user_agent(ua));
	}
	Builder &&accept(
		std::string_view mime) && {
		return move(accept(mime));
	}
	Builder &&accept_json() && { return move(accept_json()); }
	Builder &&content_type(
		std::string_view ct) && {
		return move(content_type(ct));
	}
	Builder &&if_match(
		std::string_view etag) && {
		return move(if_match(etag));
	}
	Builder &&if_none_match(
		std::string_view etag) && {
		return move(if_none_match(etag));
	}
	Builder &&if_modified_since(
		std::chrono::system_clock::time_point tp) && {
		return move(if_modified_since(tp));
	}
	Builder &&if_unmodified_since(
		std::chrono::system_clock::time_point tp) && {
		return move(if_unmodified_since(tp));
	}
	// ── body ──────────────────────────────────────────────────────────────────
	// Each body_* method asserts in debug that no prior body was set.
	// Release builds: last-wins + header overwrite.

	Builder &body(
		std::string s) & {
		assert_single_body();
		req_.body_ = move(s);
		return *this;
	}
	Builder &body_view(
		std::string_view sv) & {
		assert_single_body();
		req_.body_ = std::string{sv};
		return *this;
	}
	Builder &body_json_raw(
		std::string already_serialized) & {
		assert_single_body();
		req_.body_ = move(already_serialized);
		return content_type("application/json");
	}
	Builder &body_form(
		HttpFields const &fields) & {
		assert_single_body();
		static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
			'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
		std::size_t reserve_n = fields.empty() ? 0 : fields.size() - 1;
		for (auto const &[k, v]: fields) {
			reserve_n += k.size() * 3 + 1 + v.size() * 3;
		}
		std::string encoded;
		encoded.reserve(reserve_n);
		auto append_encoded = [&](std::string_view s) {
			for (auto const raw_c: s) {
				unsigned char const c = static_cast<unsigned char>(raw_c);
				if ((c >= 'A' && c <= 'Z')
					|| (c >= 'a' && c <= 'z')
					|| (c >= '0' && c <= '9')
					|| c == '-'
					|| c == '_'
					|| c == '.'
					|| c == '~') {
					encoded += static_cast<char>(c);
				} else if (c == ' ') {
					encoded += '+';
				} else {
					encoded += '%';
					encoded += kHex[c >> 4U];
					encoded += kHex[c & 0x0FU];
				}
			}
		};
		for (auto const &[k, v]: fields) {
			if (!encoded.empty()) {
				encoded += '&';
			}
			append_encoded(k);
			encoded += '=';
			append_encoded(v);
		}
		req_.body_ = move(encoded);
		return content_type("application/x-www-form-urlencoded");
	}
	Builder &clear_body() & {
		req_.body_.clear();
		body_set_ = false;
		return *this;
	}
	Builder &&body(
		std::string s) && {
		return move(body(move(s)));
	}
	Builder &&body_view(
		std::string_view sv) && {
		return move(body_view(sv));
	}
	Builder &&body_json_raw(
		std::string s) && {
		return move(body_json_raw(move(s)));
	}
	Builder &&body_form(
		HttpFields f) && {
		return move(body_form(move(f)));
	}
	Builder &&clear_body() && { return move(clear_body()); }
	// ── execution policy ──────────────────────────────────────────────────────

	Builder &timeouts(
		HttpTimeouts t) & {
		req_.timeouts_ = t;
		return *this;
	}
	Builder &follow_redirects(
		int max = 10) & {
		req_.max_redirects_ = max;
		return *this;
	}
	Builder &disable_redirects() & {
		req_.max_redirects_ = 0;
		return *this;
	}
	Builder &verify_peer(
		bool v) & {
		req_.verify_peer_ = v;
		return *this;
	}
	Builder &server_name(
		std::string_view sni) & {
		req_.server_name_ = std::string{sni};
		return *this;
	}
	Builder &&timeouts(
		HttpTimeouts t) && {
		return move(timeouts(t));
	}
	Builder &&follow_redirects(
		int max) && {
		return move(follow_redirects(max));
	}
	Builder &&disable_redirects() && { return move(disable_redirects()); }
	Builder &&verify_peer(
		bool v) && {
		return move(verify_peer(v));
	}
	Builder &&server_name(
		std::string_view s) && {
		return move(server_name(s));
	}
};
// ─── Static factory implementations ──────────────────────────────────────────

ClientRequest::Builder ClientRequest::get(
	std::string_view url) {
	return Builder{"GET", url};
}
ClientRequest::Builder ClientRequest::post(
	std::string_view url) {
	return Builder{"POST", url};
}
ClientRequest::Builder ClientRequest::put(
	std::string_view url) {
	return Builder{"PUT", url};
}
ClientRequest::Builder ClientRequest::patch(
	std::string_view url) {
	return Builder{"PATCH", url};
}
ClientRequest::Builder ClientRequest::del(
	std::string_view url) {
	return Builder{"DELETE", url};
}
ClientRequest::Builder ClientRequest::head(
	std::string_view url) {
	return Builder{"HEAD", url};
}
ClientRequest::Builder ClientRequest::method(
	std::string_view m,
	std::string_view url) {
	return Builder{m, url};
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_client_request_builder(
	std::string_view method,
	std::string_view url) {
	auto parsed = Url::parse(url);
	if (!parsed) {
		return unexpected{move(parsed.error())};
	}
	return ClientRequest::Builder{method, move(*parsed)};
}
[[nodiscard]] expected<ClientRequest, UrlError> try_client_request(
	std::string_view method,
	std::string_view url) {
	auto builder = try_client_request_builder(method, url);
	if (!builder) {
		return unexpected{move(builder.error())};
	}
	return move(*builder).build();
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_get_client_request(
	std::string_view url) {
	return try_client_request_builder("GET", url);
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_post_client_request(
	std::string_view url) {
	return try_client_request_builder("POST", url);
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_put_client_request(
	std::string_view url) {
	return try_client_request_builder("PUT", url);
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_patch_client_request(
	std::string_view url) {
	return try_client_request_builder("PATCH", url);
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_del_client_request(
	std::string_view url) {
	return try_client_request_builder("DELETE", url);
}
[[nodiscard]] expected<ClientRequest::Builder, UrlError> try_head_client_request(
	std::string_view url) {
	return try_client_request_builder("HEAD", url);
}

} // namespace conflux::http
