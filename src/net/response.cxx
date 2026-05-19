module;
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>
export module conflux.net.http.response;

import std;
import conflux.types;
import conflux.work;
import conflux.file_map;
import conflux.uring.handle;
export import conflux.utils;
import conflux.net.http.types;
import conflux.net.http.realtime;

[[nodiscard]] std::string response_html_escape(
	std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (char const c: s) {
		switch (c) {
		case '&' : out += "&amp;"; break;
		case '<' : out += "&lt;"; break;
		case '>' : out += "&gt;"; break;
		case '"' : out += "&quot;"; break;
		case '\'': out += "&#39;"; break;
		default  : out += c; break;
		}
	}
	return out;
}

export class DeferredResponse; // defined after HttpResponse
export enum class StreamedFileResult : std::uint8_t {
	completed,
	failed,
};

// Carrier for a file about to be streamed through io_uring (splice on plain,
// fixed-buffer read on TLS). Owns a FileHandle — send dispatch consumes it and
// issues a close_async on the owning ring when the stream finishes.
export struct StreamedFile {
	std::shared_ptr<FileHandle> handle;
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
		} catch (...) {}
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
			} catch (...) {}
		}
	}

	std::mutex mtx_;
	bool completed_{};
	StreamedFileResult result_{StreamedFileResult::failed};
	std::vector<std::function<void(StreamedFileResult)>> callbacks_;
};

export struct HttpResponse {
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
		std::shared_ptr<SseChannel>,
		std::shared_ptr<WsUpgrade>,
		std::shared_ptr<MappedBody>,
		std::shared_ptr<StreamedFile>,
		std::shared_ptr<DeferredResponse>>;

	int status = kHttpOk;
	std::string status_text = "OK";
	std::string content_type = "text/html; charset=utf-8";
	HttpFields headers = HttpFields(true); // extra response headers (added after Content-Type/Content-Length)
	std::vector<std::string> set_cookies{}; // Set-Cookie headers (one per entry)
	HttpFields trailers = HttpFields(true); // HTTP/2 trailer headers sent after the DATA frames
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
	[[nodiscard]] std::shared_ptr<SseChannel> const &sse_channel_ptr() const {
		static std::shared_ptr<SseChannel> const empty{};
		if (auto const *ch = get_if<std::shared_ptr<SseChannel>>(&body_payload)) {
			return *ch;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<WsUpgrade> const &ws_upgrade_ptr() const {
		static std::shared_ptr<WsUpgrade> const empty{};
		if (auto const *up = get_if<std::shared_ptr<WsUpgrade>>(&body_payload)) {
			return *up;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<MappedBody> const &mapped_file_ptr() const {
		static std::shared_ptr<MappedBody> const empty{};
		if (auto const *file = get_if<std::shared_ptr<MappedBody>>(&body_payload)) {
			return *file;
		}
		return empty;
	}
	[[nodiscard]] std::shared_ptr<StreamedFile> const &streamed_file_ptr() const {
		static std::shared_ptr<StreamedFile> const empty{};
		if (auto const *file = get_if<std::shared_ptr<StreamedFile>>(&body_payload)) {
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
	[[nodiscard]] std::shared_ptr<SseChannel> take_sse_channel() {
		if (!holds_alternative<std::shared_ptr<SseChannel>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<SseChannel>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<WsUpgrade> take_ws_upgrade() {
		if (!holds_alternative<std::shared_ptr<WsUpgrade>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<WsUpgrade>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<MappedBody> take_mapped_file() {
		if (!holds_alternative<std::shared_ptr<MappedBody>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<MappedBody>>(body_payload));
	}
	[[nodiscard]] std::shared_ptr<StreamedFile> take_streamed_file() {
		if (!holds_alternative<std::shared_ptr<StreamedFile>>(body_payload)) {
			return {};
		}
		return std::move(get<std::shared_ptr<StreamedFile>>(body_payload));
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
		std::shared_ptr<SseChannel> ch) {
		body_kind = BodyKind::sse;
		body_payload = std::move(ch);
	}
	void set_ws_upgrade(
		std::shared_ptr<WsUpgrade> up) {
		body_kind = BodyKind::ws_upgrade;
		body_payload = std::move(up);
	}
	void set_mapped_file(
		std::shared_ptr<MappedBody> file) {
		body_kind = BodyKind::mapped_file;
		body_payload = std::move(file);
	}
	void set_streamed_file(
		std::shared_ptr<StreamedFile> file) {
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
		case kHttpRequestEntityTooLarge      : return "Content Too Large";
		case kHttpUriTooLong                 : return "URI Too Long";
		case kHttpRangeNotSatisfiable        : return "Range Not Satisfiable";
		case kHttpUnprocessableEntity        : return "Unprocessable Entity";
		case kHttpRequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
		case kHttpInternalServerError        : return "Internal Server Error";
		case kHttpBadGateway                 : return "Bad Gateway";
		case kHttpGatewayTimeout             : return "Gateway Timeout";
		default                              : return {};
		}
	}
	[[nodiscard]] static HttpResponse with_body(
		std::string body,
		std::string content_type) {
		return with_body(std::move(body), std::move(content_type), kHttpOk);
	}
	[[nodiscard]] static HttpResponse with_body(
		std::string body,
		std::string content_type,
		int status) {
		return with_body(std::move(body), std::move(content_type), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static HttpResponse with_body(
		std::string body,
		std::string content_type,
		int status,
		std::string status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = std::move(status_text);
		r.content_type = std::move(content_type);
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse html(
		std::string body) {
		return html(std::move(body), kHttpOk);
	}
	[[nodiscard]] static HttpResponse html(
		std::string body,
		int status) {
		return html(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static HttpResponse html(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), "text/html; charset=utf-8", status, std::move(status_text));
	}
	[[nodiscard]] static HttpResponse json(
		std::string body) {
		return json(std::move(body), kHttpOk);
	}
	[[nodiscard]] static HttpResponse json(
		std::string body,
		int status) {
		return json(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static HttpResponse json(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), "application/json", status, std::move(status_text));
	}
	[[nodiscard]] static HttpResponse text(
		std::string body) {
		return text(std::move(body), kHttpOk);
	}
	[[nodiscard]] static HttpResponse text(
		std::string body,
		int status) {
		return text(std::move(body), status, std::string{status_text_for(status)});
	}
	[[nodiscard]] static HttpResponse text(
		std::string body,
		int status,
		std::string status_text) {
		return with_body(std::move(body), "text/plain; charset=utf-8", status, std::move(status_text));
	}
	[[nodiscard]] static HttpResponse redirect(
		std::string_view location,
		int code = kHttpFound) {
		char const *status_text = "Found";
		switch (code) {
		case kHttpMovedPermanently : status_text = "Moved Permanently"; break;
		case kHttpTemporaryRedirect: status_text = "Temporary Redirect"; break;
		case kHttpPermanentRedirect: status_text = "Permanent Redirect"; break;
		default                    : break;
		}
		HttpResponse r{.status = code, .status_text = status_text, .content_type = "text/html; charset=utf-8"};
		r.headers["Location"] = std::string{location};
		return r;
	}
	[[nodiscard]] static HttpResponse not_found(
		std::string_view path) {
		HttpResponse r;
		r.status = kHttpNotFound;
		r.status_text = "Not Found";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(
			std::format("<html><body><h1>404 Not Found</h1><p>{}</p></body></html>", response_html_escape(path)));
		return r;
	}
	[[nodiscard]] static HttpResponse bad_request(
		std::string_view detail = {}) {
		auto body = detail.empty() ? std::string{"<html><body><h1>400 Bad Request</h1></body></html>"} :
									 std::format(
										 "<html><body><h1>400 Bad Request</h1><p>{}</p></body></html>",
										 response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpBadRequest;
		r.status_text = "Bad Request";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse unauthorized(
		std::string_view www_authenticate = {}) {
		HttpResponse r;
		r.status = kHttpUnauthorized;
		r.status_text = "Unauthorized";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>401 Unauthorized</h1></body></html>");
		if (!www_authenticate.empty()) {
			r.headers["WWW-Authenticate"] = std::string{www_authenticate};
		}
		return r;
	}
	[[nodiscard]] static HttpResponse forbidden(
		std::string_view detail = {}) {
		auto body =
			detail.empty() ?
				std::string{"<html><body><h1>403 Forbidden</h1></body></html>"} :
				std::format("<html><body><h1>403 Forbidden</h1><p>{}</p></body></html>", response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpForbidden;
		r.status_text = "Forbidden";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse method_not_allowed(
		std::initializer_list<std::string_view> allowed = {}) {
		HttpResponse r;
		r.status = kHttpMethodNotAllowed;
		r.status_text = "Method Not Allowed";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>405 Method Not Allowed</h1></body></html>");
		if (allowed.size() > 0) {
			std::string allow;
			for (auto it = allowed.begin(); it != allowed.end(); ++it) {
				if (it != allowed.begin()) {
					allow += ", ";
				}
				allow += *it;
			}
			r.headers["Allow"] = std::move(allow);
		}
		return r;
	}
	[[nodiscard]] static HttpResponse unprocessable_entity(
		std::string_view detail = {}) {
		auto body = detail.empty() ? std::string{"<html><body><h1>422 Unprocessable Entity</h1></body></html>"} :
									 std::format(
										 "<html><body><h1>422 Unprocessable Entity</h1><p>{}</p></body></html>",
										 response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpUnprocessableEntity;
		r.status_text = "Unprocessable Entity";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse uri_too_long() {
		HttpResponse r;
		r.status = kHttpUriTooLong;
		r.status_text = "URI Too Long";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>414 URI Too Long</h1></body></html>");
		return r;
	}
	[[nodiscard]] static HttpResponse header_fields_too_large() {
		HttpResponse r;
		r.status = kHttpRequestHeaderFieldsTooLarge;
		r.status_text = "Request Header Fields Too Large";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
		return r;
	}
	[[nodiscard]] static HttpResponse bad_gateway(
		std::string_view detail = {}) {
		HttpResponse r;
		r.status = kHttpBadGateway;
		r.status_text = "Bad Gateway";
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(detail.empty() ? "Bad Gateway" : std::string{detail});
		return r;
	}
	[[nodiscard]] static HttpResponse gateway_timeout() {
		HttpResponse r;
		r.status = kHttpGatewayTimeout;
		r.status_text = "Gateway Timeout";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>504 Gateway Timeout</h1></body></html>");
		return r;
	}
	[[nodiscard]] static HttpResponse sse(
		std::shared_ptr<SseChannel> ch) {
		HttpResponse r{.status = kHttpOk, .status_text = "OK", .content_type = "text/event-stream"};
		r.set_sse_channel(std::move(ch));
		return r;
	}
	[[nodiscard]] static HttpResponse deferred(
		std::shared_ptr<DeferredResponse> response) {
		HttpResponse r;
		r.set_deferred_response(std::move(response));
		return r;
	}
	[[nodiscard]] static HttpResponse internal_error(
		std::string_view detail = {}) {
		auto body = detail.empty() ? std::string{"<html><body><h1>500 Internal Server Error</h1></body></html>"} :
									 std::format(
										 "<html><body><h1>500 Internal Server Error</h1><p>{}</p></body></html>",
										 response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpInternalServerError;
		r.status_text = "Internal Server Error";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(std::move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse no_content() { return {.status = kHttpNoContent, .status_text = "No Content"}; }
	[[nodiscard]] static HttpResponse content_too_large() {
		HttpResponse r;
		r.status = kHttpRequestEntityTooLarge;
		r.status_text = "Content Too Large";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>413 Content Too Large</h1></body></html>");
		return r;
	}
	// Append a Set-Cookie header. Attributes are Opt; pass empty strings to omit.
	// Example: resp.set_cookie("session", "abc123", "Path=/; HttpOnly; SameSite=Lax")
	HttpResponse &set_cookie(
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
	void append_vary(
		std::string_view token) {
		auto const current = std::string_view{headers["Vary"]};
		if (current.empty()) {
			headers["Vary"] = std::string{token};
			return;
		}
		auto is_ws = [](char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		auto trim_sv = [&](std::string_view s) noexcept -> std::string_view {
			while (!s.empty() && is_ws(s.front())) {
				s.remove_prefix(1);
			}
			while (!s.empty() && is_ws(s.back())) {
				s.remove_suffix(1);
			}
			return s;
		};
		if (trim_sv(current) == "*") {
			return;
		}
		auto vary = current;
		while (!vary.empty()) {
			auto const comma = vary.find(',');
			auto const part = trim_sv((comma == std::string_view::npos) ? vary : vary.substr(0, comma));
			if (conflux::http::ascii_iequals(part, token)) {
				return;
			}
			if (comma == std::string_view::npos) {
				break;
			}
			vary.remove_prefix(comma + 1);
		}
		headers["Vary"] = std::format("{}, {}", current, token);
	}
};

export namespace conflux::http {

using Response = ::HttpResponse;

} // namespace conflux::http

export class DeferredResponse {
	int efd_{-1};
	mutable std::mutex mtx_{};
	std::unique_ptr<HttpResponse> ready_{};
	std::chrono::steady_clock::time_point deadline_{};
	conflux::work::root::TaskControl cancel_ctl_{};

public:
	static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

	explicit DeferredResponse(std::chrono::milliseconds timeout = kDefaultTimeout);
	~DeferredResponse() noexcept;
	DeferredResponse(DeferredResponse const &) = delete;
	DeferredResponse &operator =(DeferredResponse const &) = delete;
	DeferredResponse(DeferredResponse &&) = delete;
	DeferredResponse &operator =(DeferredResponse &&) = delete;

	[[nodiscard]] int eventfd_fd() const noexcept;
	void complete(HttpResponse response);
	[[nodiscard]] bool is_ready() const;
	[[nodiscard]] std::optional<HttpResponse> take_ready();
	[[nodiscard]] std::chrono::steady_clock::time_point deadline() const;
	void set_deadline(std::chrono::steady_clock::time_point deadline);
	void attach_cancel(conflux::work::root::TaskControl ctl) noexcept;
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
	HttpResponse response) {
	{
		std::scoped_lock const lk{mtx_};
		if (ready_) {
			return;
		}
		ready_ = std::make_unique<HttpResponse>(std::move(response));
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
std::optional<HttpResponse> DeferredResponse::take_ready() {
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
		ready_ = std::make_unique<HttpResponse>(HttpResponse::gateway_timeout());
		to_cancel = std::move(cancel_ctl_);
	}
	auto _ = to_cancel.request_cancel();
	std::uint64_t wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		eprintln(std::format("DeferredResponse::expire_if_past_deadline: eventfd write: {}", strerror(errno)));
	}
	return true;
}
