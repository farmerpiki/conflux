module;
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>
export module conflux.net.http.response;

import std;
import conflux.types;
import conflux.work;
import conflux.file_map.types;
export import conflux.utils;
import conflux.net.http.types;
import conflux.net.http.realtime;

export namespace conflux::http::detail {

void append_html_escaped(
	std::string &out,
	std::string_view value) {
	for (char const c: value) {
		switch (c) {
		case '&' : out += "&amp;"; break;
		case '<' : out += "&lt;"; break;
		case '>' : out += "&gt;"; break;
		case '"' : out += "&quot;"; break;
		case '\'': out += "&#39;"; break;
		default  : out += c; break;
		}
	}
}

[[nodiscard]] std::string html_escaped(
	std::string_view value) {
	std::string out;
	out.reserve(value.size());
	append_html_escaped(out, value);
	return out;
}

} // namespace conflux::http::detail

namespace conflux::http {

[[nodiscard]] std::string response_html_error_body(
	int status,
	std::string_view status_text,
	std::string_view detail = {}) {
	auto body = std::format("<html><body><h1>{} {}</h1>", status, status_text);
	if (!detail.empty()) {
		body += std::format("<p>{}</p>", conflux::http::detail::html_escaped(detail));
	}
	body += "</body></html>";
	return body;
}

} // namespace conflux::http

export namespace conflux::http {

using conflux::utils::eprintln;
using conflux::utils::kHttpBadGateway;
using conflux::utils::kHttpBadRequest;
using conflux::utils::kHttpCreated;
using conflux::utils::kHttpForbidden;
using conflux::utils::kHttpFound;
using conflux::utils::kHttpGatewayTimeout;
using conflux::utils::kHttpInternalServerError;
using conflux::utils::kHttpMethodNotAllowed;
using conflux::utils::kHttpMovedPermanently;
using conflux::utils::kHttpNoContent;
using conflux::utils::kHttpNotFound;
using conflux::utils::kHttpNotModified;
using conflux::utils::kHttpOk;
using conflux::utils::kHttpPartialContent;
using conflux::utils::kHttpPermanentRedirect;
using conflux::utils::kHttpRangeNotSatisfiable;
using conflux::utils::kHttpRequestEntityTooLarge;
using conflux::utils::kHttpRequestHeaderFieldsTooLarge;
using conflux::utils::kHttpTemporaryRedirect;
using conflux::utils::kHttpTooManyRequests;
using conflux::utils::kHttpUnauthorized;
using conflux::utils::kHttpUnprocessableEntity;
using conflux::utils::kHttpUriTooLong;

class DeferredResponse;

enum class StreamedFileResult : std::uint8_t {
	completed,
	failed,
};

enum class SameSite : std::uint8_t {
	Lax,
	Strict,
	None,
};

[[nodiscard]] constexpr std::string_view same_site_token(
	SameSite value) noexcept {
	switch (value) {
	case SameSite::Lax   : return "Lax";
	case SameSite::Strict: return "Strict";
	case SameSite::None  : return "None";
	}
	return "Lax";
}

struct StreamedFileHandle {
	std::shared_ptr<void> storage{};

	template<class Handle>
	[[nodiscard]] static StreamedFileHandle from(
		std::shared_ptr<Handle> handle) noexcept {
		return StreamedFileHandle{.storage = std::move(handle)};
	}

	template<class Handle>
	[[nodiscard]] Handle *get_if() const noexcept {
		return static_cast<Handle *>(storage.get());
	}

	[[nodiscard]] explicit operator bool() const noexcept { return storage != nullptr; }
};

struct CookieBuilder {
	std::string name;
	std::string value;
	std::string attributes;

	CookieBuilder(
		std::string_view cookie_name,
		std::string_view cookie_value)
		: name(cookie_name)
		, value(cookie_value) {}

	[[nodiscard]] auto &&attribute(
		this auto &&self,
		std::string_view attr) {
		if (attr.empty()) {
			return std::forward<decltype(self)>(self);
		}
		if (!self.attributes.empty()) {
			self.attributes += "; ";
		}
		self.attributes += attr;
		return std::forward<decltype(self)>(self);
	}

	[[nodiscard]] auto &&path(
		this auto &&self,
		std::string_view path_value) {
		return std::forward<decltype(self)>(self).attribute(std::format("Path={}", path_value));
	}

	[[nodiscard]] auto &&domain(
		this auto &&self,
		std::string_view domain_value) {
		return std::forward<decltype(self)>(self).attribute(std::format("Domain={}", domain_value));
	}

	[[nodiscard]] auto &&http_only(
		this auto &&self) {
		return std::forward<decltype(self)>(self).attribute("HttpOnly");
	}

	[[nodiscard]] auto &&secure(
		this auto &&self) {
		return std::forward<decltype(self)>(self).attribute("Secure");
	}

	[[nodiscard]] auto &&same_site(
		this auto &&self,
		SameSite value) {
		return std::forward<decltype(self)>(self).attribute(std::format("SameSite={}", same_site_token(value)));
	}

	template<class Rep, class Period>
	[[nodiscard]] auto &&max_age(
		this auto &&self,
		std::chrono::duration<Rep, Period> age) {
		auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
		return std::forward<decltype(self)>(self).attribute(std::format("Max-Age={}", seconds));
	}

	[[nodiscard]] auto &&expires(
		this auto &&self,
		std::string_view http_date) {
		return std::forward<decltype(self)>(self).attribute(std::format("Expires={}", http_date));
	}
};

// Carrier for a file about to be streamed through io_uring (splice on plain,
// fixed-buffer read on TLS). Owns an internal file handle; send dispatch
// consumes it and issues a close_async on the owning ring when the stream
// finishes.
struct StreamedFile {
	StreamedFileHandle handle{};
	std::uint64_t send_offset{};
	std::uint64_t send_size{};
	// total file size — needed for Content-Range and range-validation paths.
	std::uint64_t total_size{};

	void on_complete(
		std::function<void(StreamedFileResult)> callback) {
		auto result = StreamedFileResult::failed;
		{
			std::scoped_lock const lk{mtx_};
			if (!completed_) {
				callbacks_.push_back(std::move(callback));
				return;
			}
			result = result_;
		}
		try {
			callback(result);
		} catch (...) {
		} // NOLINT(bugprone-empty-catch): completion observer exceptions cannot change completed stream state.
	}

	void notify_complete() { notify(StreamedFileResult::completed); }

	void notify_failed() { notify(StreamedFileResult::failed); }

	void notify(
		StreamedFileResult result) {
		std::vector<std::function<void(StreamedFileResult)>> callbacks;
		{
			std::scoped_lock const lk{mtx_};
			if (completed_) {
				return;
			}
			completed_ = true;
			result_ = result;
			callbacks = std::move(callbacks_);
		}
		for (auto &callback: callbacks) {
			try {
				callback(result);
			} catch (...) {} // NOLINT(bugprone-empty-catch): notify all observers even if one throws.
		}
	}

	std::mutex mtx_;
	bool completed_{};
	StreamedFileResult result_{StreamedFileResult::failed};
	std::vector<std::function<void(StreamedFileResult)>> callbacks_;
};

struct Response {
	static constexpr std::string_view kContentTypeHtmlUtf8 = "text/html; charset=utf-8";
	static constexpr std::string_view kContentTypeTextUtf8 = "text/plain; charset=utf-8";
	static constexpr std::string_view kContentTypeJson = "application/json";
	static constexpr std::string_view kContentTypeProblemJson = "application/problem+json";
	static constexpr std::string_view kContentTypePrometheus = "text/plain; version=0.0.4; charset=utf-8";
	static constexpr std::string_view kContentTypeEventStream = "text/event-stream";

	enum class BodyKind : std::uint8_t {
		text,
		sse,
		ws_upgrade,
		mapped_file,
		streamed_file,
		deferred,
	};

	using BodyPayload = std::variant<
		std::string,
		std::shared_ptr<conflux::http::SseChannel>,
		std::shared_ptr<conflux::http::WsUpgrade>,
		std::shared_ptr<conflux::file_map::MappedBody>,
		std::shared_ptr<conflux::http::StreamedFile>,
		std::shared_ptr<DeferredResponse>>;

	int status = kHttpOk;
	std::string status_text = "OK";
	std::string content_type = std::string{kContentTypeHtmlUtf8};
	conflux::http::HttpFields headers =
		conflux::http::HttpFields(true); // extra response headers (added after Content-Type/Content-Length)
	std::vector<std::string> set_cookies{}; // Set-Cookie headers (one per entry)
	conflux::http::HttpFields trailers =
		conflux::http::HttpFields(true); // HTTP/2 trailer headers sent after the DATA frames
	bool head_only = false; // true → send headers only, suppress body (HEAD requests)
	std::size_t content_length_hint{0}; // non-zero overrides content_length() (HEAD static file responses)
	BodyKind body_kind = BodyKind::text;
	BodyPayload body_payload{std::string{}};
	[[nodiscard]] bool is_text() const noexcept { return body_kind == BodyKind::text; }
	[[nodiscard]] bool is_sse() const noexcept { return body_kind == BodyKind::sse; }
	[[nodiscard]] bool is_ws_upgrade() const noexcept { return body_kind == BodyKind::ws_upgrade; }
	[[nodiscard]] bool is_mapped_file() const noexcept { return body_kind == BodyKind::mapped_file; }
	[[nodiscard]] bool is_streamed_file() const noexcept { return body_kind == BodyKind::streamed_file; }
	[[nodiscard]] bool is_deferred() const noexcept { return body_kind == BodyKind::deferred; }
	[[nodiscard]] std::string_view text_body() const noexcept {
		if (auto const *text = get_if<std::string>(&body_payload)) {
			return *text;
		}
		return {};
	}
	[[nodiscard]] std::string &text_body_mut() {
		if (!is_text() || !holds_alternative<std::string>(body_payload)) {
			body_kind = BodyKind::text;
			body_payload = std::string{};
		}
		return get<std::string>(body_payload);
	}
	[[nodiscard]] std::string take_text_body() {
		if (!holds_alternative<std::string>(body_payload)) {
			return {};
		}
		return std::move(get<std::string>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<conflux::http::SseChannel> const &sse_channel_ptr() const {
		static std::shared_ptr<conflux::http::SseChannel> const empty{};
		if (auto const *ch = get_if<std::shared_ptr<conflux::http::SseChannel>>(&body_payload)) {
			return *ch;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<conflux::http::WsUpgrade> const &ws_upgrade_ptr() const {
		static std::shared_ptr<conflux::http::WsUpgrade> const empty{};
		if (auto const *up = get_if<std::shared_ptr<conflux::http::WsUpgrade>>(&body_payload)) {
			return *up;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<conflux::file_map::MappedBody> const &mapped_file_ptr() const {
		static std::shared_ptr<conflux::file_map::MappedBody> const empty{};
		if (auto const *file = get_if<std::shared_ptr<conflux::file_map::MappedBody>>(&body_payload)) {
			return *file;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<conflux::http::StreamedFile> const &streamed_file_ptr() const {
		static std::shared_ptr<conflux::http::StreamedFile> const empty{};
		if (auto const *file = get_if<std::shared_ptr<conflux::http::StreamedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<DeferredResponse> const &deferred_response_ptr() const {
		static std::shared_ptr<DeferredResponse> const empty{};
		if (auto const *deferred = get_if<std::shared_ptr<DeferredResponse>>(&body_payload)) {
			return *deferred;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<conflux::http::SseChannel> take_sse_channel() {
		if (!holds_alternative<std::shared_ptr<conflux::http::SseChannel>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<conflux::http::SseChannel>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<conflux::http::WsUpgrade> take_ws_upgrade() {
		if (!holds_alternative<std::shared_ptr<conflux::http::WsUpgrade>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<conflux::http::WsUpgrade>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<conflux::file_map::MappedBody> take_mapped_file() {
		if (!holds_alternative<std::shared_ptr<conflux::file_map::MappedBody>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<conflux::file_map::MappedBody>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<conflux::http::StreamedFile> take_streamed_file() {
		if (!holds_alternative<std::shared_ptr<conflux::http::StreamedFile>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<conflux::http::StreamedFile>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<DeferredResponse> take_deferred_response() {
		if (!holds_alternative<std::shared_ptr<DeferredResponse>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<DeferredResponse>>(body_payload));
	}
	void set_text_body(
		std::string text) {
		body_kind = BodyKind::text;
		body_payload = std::move(text);
	}
	void set_sse_channel(
		std::shared_ptr<conflux::http::SseChannel> ch) {
		body_kind = BodyKind::sse;
		body_payload = std::move(ch);
	}
	void set_ws_upgrade(
		std::shared_ptr<conflux::http::WsUpgrade> up) {
		body_kind = BodyKind::ws_upgrade;
		body_payload = std::move(up);
	}
	void set_mapped_file(
		std::shared_ptr<conflux::file_map::MappedBody> file) {
		body_kind = BodyKind::mapped_file;
		body_payload = std::move(file);
	}
	void set_streamed_file(
		std::shared_ptr<conflux::http::StreamedFile> file) {
		body_kind = BodyKind::streamed_file;
		body_payload = std::move(file);
	}
	void set_deferred_response(
		std::shared_ptr<DeferredResponse> deferred) {
		body_kind = BodyKind::deferred;
		body_payload = std::move(deferred);
	}
	[[nodiscard]] std::size_t content_length() const noexcept {
		if (content_length_hint != 0) {
			return content_length_hint;
		}
		if (is_mapped_file() && mapped_file_ptr()) {
			return static_cast<std::size_t>(mapped_file_ptr()->size);
		}
		if (is_streamed_file() && streamed_file_ptr()) {
			return static_cast<std::size_t>(streamed_file_ptr()->send_size);
		}
		return text_body().size();
	}
	[[nodiscard]] static std::string_view status_text_for(
		int status) noexcept {
		switch (status) {
		case kHttpOk                         : return "OK";
		case kHttpCreated                    : return "Created";
		case kHttpNoContent                  : return "No Content";
		case kHttpPartialContent             : return "Partial Content";
		case kHttpMovedPermanently           : return "Moved Permanently";
		case kHttpFound                      : return "Found";
		case kHttpNotModified                : return "Not Modified";
		case kHttpTemporaryRedirect          : return "Temporary Redirect";
		case kHttpPermanentRedirect          : return "Permanent Redirect";
		case kHttpBadRequest                 : return "Bad Request";
		case kHttpUnauthorized               : return "Unauthorized";
		case kHttpForbidden                  : return "Forbidden";
		case kHttpNotFound                   : return "Not Found";
		case kHttpMethodNotAllowed           : return "Method Not Allowed";
		case 408                             : return "Request Timeout";
		case kHttpRequestEntityTooLarge      : return "Content Too Large";
		case kHttpUriTooLong                 : return "URI Too Long";
		case 417                             : return "Expectation Failed";
		case kHttpRangeNotSatisfiable        : return "Range Not Satisfiable";
		case kHttpUnprocessableEntity        : return "Unprocessable Entity";
		case kHttpRequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
		case kHttpTooManyRequests            : return "Too Many Requests";
		case kHttpInternalServerError        : return "Internal Server Error";
		case kHttpBadGateway                 : return "Bad Gateway";
		case kHttpGatewayTimeout             : return "Gateway Timeout";
		default                              : return {};
		}
	}
	[[nodiscard]] static Response with_body(
		std::string body,
		std::string content_type) {
		return with_body(std::move(body), std::move(content_type), kHttpOk);
	}
	[[nodiscard]] static Response with_body(
		std::string body,
		std::string content_type,
		int status) {
		return with_body(std::move(body), std::move(content_type), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static Response with_body(
		std::string body,
		std::string content_type,
		int status,
		std::string status_text) {
		Response r;
		r.status = status;
		r.status_text = std::move(status_text);
		r.content_type = std::move(content_type);
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static Response html(
		std::string body) {
		return html(std::move(body), kHttpOk);
	}
	[[nodiscard]] static Response html(
		std::string body,
		int status) {
		return html(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static Response html(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), std::string{kContentTypeHtmlUtf8}, status, std::move(status_text));
	}
	[[nodiscard]] static Response json(
		std::string body) {
		return json(std::move(body), kHttpOk);
	}
	[[nodiscard]] static Response json(
		std::string body,
		int status) {
		return json(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static Response json(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), std::string{kContentTypeJson}, status, std::move(status_text));
	}
	[[nodiscard]] static Response problem_json(
		std::string body) {
		return problem_json(std::move(body), kHttpBadRequest);
	}
	[[nodiscard]] static Response problem_json(
		std::string body,
		int status) {
		return problem_json(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static Response problem_json(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), std::string{kContentTypeProblemJson}, status, std::move(status_text));
	}
	[[nodiscard]] static Response prometheus(
		std::string body) {
		return with_body(std::move(body), std::string{kContentTypePrometheus}, kHttpOk, "OK");
	}
	[[nodiscard]] static Response text(
		std::string body) {
		return text(std::move(body), kHttpOk);
	}
	[[nodiscard]] static Response text(
		std::string body,
		int status) {
		return text(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static Response text(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), std::string{kContentTypeTextUtf8}, status, std::move(status_text));
	}
	[[nodiscard]] static Response redirect(
		std::string_view location,
		int code = kHttpFound) {
		auto status_text = status_text_for(code);
		if (status_text.empty()) {
			status_text = "Found";
		}
		Response r{
			.status = code,
			.status_text = std::string{status_text},
			.content_type = std::string{kContentTypeHtmlUtf8}};
		r.headers["Location"] = std::string{location};
		return r;
	}
	[[nodiscard]] static Response not_found(
		std::string_view path) {
		return html(response_html_error_body(kHttpNotFound, "Not Found", path), kHttpNotFound);
	}
	[[nodiscard]] static Response bad_request(
		std::string_view detail = {}) {
		return html(response_html_error_body(kHttpBadRequest, "Bad Request", detail), kHttpBadRequest);
	}
	[[nodiscard]] static Response unauthorized(
		std::string_view www_authenticate = {}) {
		auto r = html(response_html_error_body(kHttpUnauthorized, "Unauthorized"), kHttpUnauthorized);
		if (!www_authenticate.empty()) {
			r.headers["WWW-Authenticate"] = std::string{www_authenticate};
		}
		return r;
	}
	[[nodiscard]] static Response forbidden(
		std::string_view detail = {}) {
		return html(response_html_error_body(kHttpForbidden, "Forbidden", detail), kHttpForbidden);
	}
	[[nodiscard]] static Response method_not_allowed(
		std::initializer_list<std::string_view> allowed = {}) {
		auto r = html(response_html_error_body(kHttpMethodNotAllowed, "Method Not Allowed"), kHttpMethodNotAllowed);
		if (allowed.size() > 0) {
			r.headers["Allow"] = conflux::http::join_header_tokens(allowed);
		}
		return r;
	}
	[[nodiscard]] static Response unprocessable_entity(
		std::string_view detail = {}) {
		return html(
			response_html_error_body(kHttpUnprocessableEntity, "Unprocessable Entity", detail),
			kHttpUnprocessableEntity);
	}
	[[nodiscard]] static Response uri_too_long() {
		return html(response_html_error_body(kHttpUriTooLong, "URI Too Long"), kHttpUriTooLong);
	}
	[[nodiscard]] static Response header_fields_too_large() {
		return html(
			response_html_error_body(kHttpRequestHeaderFieldsTooLarge, "Request Header Fields Too Large"),
			kHttpRequestHeaderFieldsTooLarge);
	}
	[[nodiscard]] static Response bad_gateway(
		std::string_view detail = {}) {
		Response r;
		r.status = kHttpBadGateway;
		r.status_text = "Bad Gateway";
		r.content_type = std::string{kContentTypeTextUtf8};
		r.set_text_body(detail.empty() ? "Bad Gateway" : std::string{detail});
		return r;
	}
	[[nodiscard]] static Response gateway_timeout() {
		return html(response_html_error_body(kHttpGatewayTimeout, "Gateway Timeout"), kHttpGatewayTimeout);
	}
	[[nodiscard]] static Response sse(
		std::shared_ptr<conflux::http::SseChannel> ch) {
		Response r{.status = kHttpOk, .status_text = "OK", .content_type = std::string{kContentTypeEventStream}};
		r.set_sse_channel(std::move(ch));
		return r;
	}
	[[nodiscard]] static Response deferred(
		std::shared_ptr<DeferredResponse> response) {
		Response r;
		r.set_deferred_response(std::move(response));
		return r;
	}
	[[nodiscard]] static Response internal_error(
		std::string_view detail = {}) {
		return html(
			response_html_error_body(kHttpInternalServerError, "Internal Server Error", detail),
			kHttpInternalServerError);
	}
	[[nodiscard]] static Response no_content() { return {.status = kHttpNoContent, .status_text = "No Content"}; }
	[[nodiscard]] static Response not_modified(
		std::string_view etag = {}) {
		Response r{.status = kHttpNotModified, .status_text = "Not Modified"};
		if (!etag.empty()) {
			r.headers["ETag"] = std::string{etag};
		}
		return r;
	}
	[[nodiscard]] static Response content_too_large() {
		return html(
			response_html_error_body(kHttpRequestEntityTooLarge, "Content Too Large"),
			kHttpRequestEntityTooLarge);
	}
	// Append a Set-Cookie header. Attributes are Opt; pass empty strings to omit.
	// Example: resp.set_cookie("session", "abc123", "Path=/; HttpOnly; SameSite=Lax")
	Response &set_cookie(
		std::string_view name,
		std::string_view cookie_value,
		std::string_view attributes = {}) {
		if (attributes.empty()) {
			set_cookies.push_back(std::format("{}={}", name, cookie_value));
		} else {
			set_cookies.push_back(std::format("{}={}; {}", name, cookie_value, attributes));
		}
		return *this;
	}
	Response &set_cookie(
		conflux::http::CookieBuilder cookie) {
		return set_cookie(cookie.name, cookie.value, cookie.attributes);
	}
	void append_vary(
		std::string_view token) {
		auto const current = std::string_view{headers["Vary"]};
		if (current.empty()) {
			headers["Vary"] = std::string{token};
			return;
		}
		if (conflux::utils::trim(current) == "*") {
			return;
		}
		bool const already_present =
			std::ranges::any_of(conflux::http::header_tokens(current), [&](std::string_view part) {
				return conflux::http::ascii_iequals(part, token);
			});
		if (already_present) {
			return;
		}
		headers["Vary"] = std::format("{}, {}", current, token);
	}
};

class DeferredResponse {
	int efd_{-1};
	mutable std::mutex mtx_{};
	std::unique_ptr<Response> ready_{};
	std::chrono::steady_clock::time_point deadline_{};
	conflux::work::root::TaskControl cancel_ctl_{};
	std::vector<std::shared_ptr<void const>> keep_alive_{};

public:
	static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

	explicit DeferredResponse(std::chrono::milliseconds timeout = kDefaultTimeout);
	~DeferredResponse() noexcept;
	DeferredResponse(DeferredResponse const &) = delete;
	DeferredResponse &operator =(DeferredResponse const &) = delete;
	DeferredResponse(DeferredResponse &&) = delete;
	DeferredResponse &operator =(DeferredResponse &&) = delete;

	[[nodiscard]] int eventfd_fd() const noexcept;
	void complete(Response response);
	[[nodiscard]] bool is_ready() const;
	[[nodiscard]] std::optional<Response> take_ready();
	[[nodiscard]] std::chrono::steady_clock::time_point deadline() const;
	void set_deadline(std::chrono::steady_clock::time_point deadline);
	void attach_cancel(conflux::work::root::TaskControl ctl) noexcept;
	void cancel(conflux::work::root::CancelReason reason) noexcept;
	void cancel_deadline() noexcept;
	void cancel_disconnect() noexcept;
	void cancel_shutdown() noexcept;
	void keep_alive(std::shared_ptr<void const> storage);
	// Force-complete with 504 if the deadline has passed and no response is ready.
	// Returns true if this call expired the response (i.e. the caller should expect
	// the ready signal to fire).
	bool expire_if_past_deadline(std::chrono::steady_clock::time_point now);
};
DeferredResponse::DeferredResponse(
	std::chrono::milliseconds timeout)
	: efd_{::eventfd(0, EFD_CLOEXEC)}
	, deadline_{std::chrono::steady_clock::now() + timeout} {
	if (efd_ < 0) {
		throw std::system_error{errno, std::system_category(), "eventfd"};
	}
}
DeferredResponse::~DeferredResponse() noexcept {
	if (efd_ >= 0) {
		::close(efd_);
	}
}
int DeferredResponse::eventfd_fd() const noexcept {
	return efd_;
}
void DeferredResponse::complete(
	Response response) {
	{
		std::scoped_lock const lk{mtx_};
		if (ready_) {
			return;
		}
		ready_ = std::make_unique<Response>(std::move(response));
	}
	std::uint64_t wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		eprintln(std::format("DeferredResponse::complete: eventfd write: {}", strerror(errno)));
	}
}
bool DeferredResponse::is_ready() const {
	std::scoped_lock const lk{mtx_};
	return ready_ != nullptr;
}
std::optional<Response> DeferredResponse::take_ready() {
	std::scoped_lock const lk{mtx_};
	if (!ready_) {
		return std::nullopt;
	}
	auto response = std::move(*ready_);
	ready_.reset();
	return response;
}
std::chrono::steady_clock::time_point DeferredResponse::deadline() const {
	std::scoped_lock const lk{mtx_};
	return deadline_;
}
void DeferredResponse::set_deadline(
	std::chrono::steady_clock::time_point deadline) {
	std::scoped_lock const lk{mtx_};
	deadline_ = deadline;
}
void DeferredResponse::attach_cancel(
	conflux::work::root::TaskControl ctl) noexcept {
	std::scoped_lock const lk{mtx_};
	cancel_ctl_ = std::move(ctl);
}
void DeferredResponse::cancel(
	conflux::work::root::CancelReason reason) noexcept {
	conflux::work::root::TaskControl to_cancel;
	{
		std::scoped_lock const lk{mtx_};
		to_cancel = cancel_ctl_;
	}
	(void)to_cancel.request_cancel(reason);
}
void DeferredResponse::cancel_deadline() noexcept {
	cancel(conflux::work::root::CancelReason::deadline);
}
void DeferredResponse::cancel_disconnect() noexcept {
	cancel(conflux::work::root::CancelReason::requested);
}
void DeferredResponse::cancel_shutdown() noexcept {
	cancel(conflux::work::root::CancelReason::shutdown);
}
void DeferredResponse::keep_alive(
	std::shared_ptr<void const> storage) {
	if (!storage) {
		return;
	}
	std::scoped_lock const lk{mtx_};
	keep_alive_.push_back(std::move(storage));
}
bool DeferredResponse::expire_if_past_deadline(
	std::chrono::steady_clock::time_point now) {
	conflux::work::root::TaskControl to_cancel;
	{
		std::scoped_lock const lk{mtx_};
		if (ready_) {
			return false;
		}
		if (now < deadline_) {
			return false;
		}
		ready_ = std::make_unique<Response>(Response::gateway_timeout());
		to_cancel = std::move(cancel_ctl_);
	}
	auto _ = to_cancel.request_cancel(conflux::work::root::CancelReason::deadline);
	std::uint64_t wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		eprintln(std::format("DeferredResponse::expire_if_past_deadline: eventfd write: {}", strerror(errno)));
	}
	return true;
}

} // namespace conflux::http
