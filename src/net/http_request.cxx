module;
#include <cassert>
#include <time.h>

export module conflux.net.http.request;
import std;
import conflux.crypto;
import conflux.types;
import conflux.utils;
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
	[[nodiscard]] bool follows_redirects() const noexcept { return follow_redirects_; }

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
	bool follow_redirects_{false};

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
			throw std::invalid_argument(std::format("invalid URL: {}", r.error().message));
		}
		return std::move(*r);
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
		req_.url_ = std::move(url);
	}
	// Implicit conversion: Builder&& → ClientRequest (no-copy).
	operator ClientRequest() && { return std::move(req_); } // NOLINT(google-explicit-constructor)
	[[nodiscard]] ClientRequest build() && { return std::move(req_); }
	// ── verbs / URL ──────────────────────────────────────────────────────────

	template<class Self>
	Self &&method(
		this Self &&self,
		std::string_view m) {
		self.req_.method_ = std::string{m};
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&url(
		this Self &&self,
		std::string_view raw) {
		self.req_.url_ = parse_or_throw(raw);
		return std::forward<Self>(self);
	}
	[[nodiscard]] std::expected<void, UrlError> try_url(
		std::string_view raw) & {
		auto parsed = Url::parse(raw);
		if (!parsed) {
			return std::unexpected{std::move(parsed.error())};
		}
		req_.url_ = std::move(*parsed);
		return {};
	}
	template<class Self>
	Self &&url(
		this Self &&self,
		Url u) {
		self.req_.url_ = std::move(u);
		return std::forward<Self>(self);
	}
	// ── query ─────────────────────────────────────────────────────────────────

	template<class Self>
	Self &&query(
		this Self &&self,
		std::string_view name,
		std::string_view value) {
		self.req_.url_.set_query_param(name, value);
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&query_params(
		this Self &&self,
		HttpFields const &kv) {
		for (auto const &[k, v]: kv) {
			self.req_.url_.set_query_param(k, v);
		}
		return std::forward<Self>(self);
	}
	// ── headers ───────────────────────────────────────────────────────────────

	template<class Self>
	Self &&header(
		this Self &&self,
		std::string_view name,
		std::string_view value) {
		self.req_.headers_.set(std::string{name}, std::string{value});
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&headers(
		this Self &&self,
		HttpFields const &h) {
		for (auto const &[k, v]: h) {
			self.req_.headers_.set(k, v);
		}
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&bearer(
		this Self &&self,
		std::string_view token) {
		return std::forward<Self>(self).header("Authorization", std::format("Bearer {}", token));
	}
	template<class Self>
	Self &&basic(
		this Self &&self,
		std::string_view user,
		std::string_view pass) {
		auto const creds = std::format("{}:{}", user, pass);
		auto const encoded = base64_encode(to_unsigned_span(creds));
		return std::forward<Self>(self).header("Authorization", std::format("Basic {}", encoded));
	}
	template<class Self>
	Self &&user_agent(
		this Self &&self,
		std::string_view ua) {
		return std::forward<Self>(self).header("User-Agent", ua);
	}
	template<class Self>
	Self &&accept(
		this Self &&self,
		std::string_view mime) {
		return std::forward<Self>(self).header("Accept", mime);
	}
	template<class Self>
	Self &&accept_json(
		this Self &&self) {
		return std::forward<Self>(self).accept("application/json");
	}
	template<class Self>
	Self &&content_type(
		this Self &&self,
		std::string_view ct) {
		return std::forward<Self>(self).header("Content-Type", ct);
	}
	template<class Self>
	Self &&if_match(
		this Self &&self,
		std::string_view etag) {
		return std::forward<Self>(self).header("If-Match", etag);
	}
	template<class Self>
	Self &&if_none_match(
		this Self &&self,
		std::string_view etag) {
		return std::forward<Self>(self).header("If-None-Match", etag);
	}
	template<class Self>
	Self &&if_modified_since(
		this Self &&self,
		std::chrono::system_clock::time_point tp) {
		return std::forward<Self>(self).header("If-Modified-Since", http_date(tp));
	}
	template<class Self>
	Self &&if_unmodified_since(
		this Self &&self,
		std::chrono::system_clock::time_point tp) {
		return std::forward<Self>(self).header("If-Unmodified-Since", http_date(tp));
	}
	// ── body ──────────────────────────────────────────────────────────────────
	// Each body_* method asserts in debug that no prior body was set.
	// Release builds: last-wins + header overwrite.

	template<class Self>
	Self &&body(
		this Self &&self,
		std::string s) {
		self.assert_single_body();
		self.req_.body_ = std::move(s);
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&body_view(
		this Self &&self,
		std::string_view sv) {
		self.assert_single_body();
		self.req_.body_ = std::string{sv};
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&body_json_raw(
		this Self &&self,
		std::string already_serialized) {
		self.assert_single_body();
		self.req_.body_ = std::move(already_serialized);
		return std::forward<Self>(self).content_type("application/json");
	}
	template<class Self>
	Self &&body_form(
		this Self &&self,
		HttpFields const &fields) {
		self.assert_single_body();
		std::size_t reserve_n = fields.empty() ? 0 : fields.size() - 1;
		for (auto const &[k, v]: fields) {
			reserve_n += url_form_encoded_size(k) + 1 + url_form_encoded_size(v);
		}
		std::string encoded;
		encoded.reserve(reserve_n);
		for (auto const &[k, v]: fields) {
			if (!encoded.empty()) {
				encoded += '&';
			}
			append_url_form_encoded(encoded, k);
			encoded += '=';
			append_url_form_encoded(encoded, v);
		}
		self.req_.body_ = std::move(encoded);
		return std::forward<Self>(self).content_type("application/x-www-form-urlencoded");
	}
	template<class Self>
	Self &&clear_body(
		this Self &&self) {
		self.req_.body_.clear();
		self.body_set_ = false;
		return std::forward<Self>(self);
	}
	// ── execution policy ──────────────────────────────────────────────────────

	template<class Self>
	Self &&timeouts(
		this Self &&self,
		HttpTimeouts t) {
		self.req_.timeouts_ = t;
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&follow_redirects(
		this Self &&self,
		int max_redirects = 10) {
		self.req_.max_redirects_ = max_redirects;
		self.req_.follow_redirects_ = true;
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&disable_redirects(
		this Self &&self) {
		self.req_.max_redirects_ = 0;
		self.req_.follow_redirects_ = false;
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&verify_peer(
		this Self &&self,
		bool v) {
		self.req_.verify_peer_ = v;
		return std::forward<Self>(self);
	}
	template<class Self>
	Self &&server_name(
		this Self &&self,
		std::string_view sni) {
		self.req_.server_name_ = std::string{sni};
		return std::forward<Self>(self);
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
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_method(
	std::string_view method,
	std::string_view url) {
	auto parsed = Url::parse(url);
	if (!parsed) {
		return std::unexpected{std::move(parsed.error())};
	}
	return ClientRequest::Builder{method, std::move(*parsed)};
}
[[nodiscard]] std::expected<ClientRequest, UrlError> try_request(
	std::string_view method,
	std::string_view url) {
	auto builder = try_method(method, url);
	if (!builder) {
		return std::unexpected{std::move(builder.error())};
	}
	return std::move(*builder).build();
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_get(
	std::string_view url) {
	return try_method("GET", url);
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_post(
	std::string_view url) {
	return try_method("POST", url);
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_put(
	std::string_view url) {
	return try_method("PUT", url);
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_patch(
	std::string_view url) {
	return try_method("PATCH", url);
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_del(
	std::string_view url) {
	return try_method("DELETE", url);
}
[[nodiscard]] std::expected<ClientRequest::Builder, UrlError> try_head(
	std::string_view url) {
	return try_method("HEAD", url);
}

} // namespace conflux::http
