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

[[nodiscard]] S response_html_escape(
	SV s) {
	S out;
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
// Carrier for a file about to be streamed through io_uring (splice on plain,
// fixed-buffer read on TLS). Owns a FileHandle — send dispatch consumes it and
// issues a close_async on the owning ring when the stream finishes.
export struct StreamedFile {
	SP<FileHandle> handle;
	u64 send_offset{};
	u64 send_size{};
	// total file size — needed for Content-Range and range-validation paths.
	u64 total_size{};
};

export struct HttpResponse {
	enum class BodyKind : u8 {
		text,
		sse,
		ws_upgrade,
		mapped_file,
		streamed_file,
		deferred,
	};

	using BodyPayload =
		variant<S, SP<SseChannel>, SP<WsUpgrade>, SP<MappedBody>, SP<StreamedFile>, SP<DeferredResponse>>;

	int status = kHttpOk;
	S status_text = "OK";
	S content_type = "text/html; charset=utf-8";
	HttpFields headers = HttpFields(true); // extra response headers (added after Content-Type/Content-Length)
	V<S> set_cookies{}; // Set-Cookie headers (one per entry)
	HttpFields trailers = HttpFields(true); // HTTP/2 trailer headers sent after the DATA frames
	bool head_only = false; // true → send headers only, suppress body (HEAD requests)
	SZ content_length_hint{0}; // non-zero overrides content_length() (HEAD static file responses)
	BodyKind body_kind = BodyKind::text;
	BodyPayload body_payload{S{}};
	[[nodiscard]] bool is_text() const noexcept { return body_kind == BodyKind::text; }
	[[nodiscard]] bool is_sse() const noexcept { return body_kind == BodyKind::sse; }
	[[nodiscard]] bool is_ws_upgrade() const noexcept { return body_kind == BodyKind::ws_upgrade; }
	[[nodiscard]] bool is_mapped_file() const noexcept { return body_kind == BodyKind::mapped_file; }
	[[nodiscard]] bool is_streamed_file() const noexcept { return body_kind == BodyKind::streamed_file; }
	[[nodiscard]] bool is_deferred() const noexcept { return body_kind == BodyKind::deferred; }
	[[nodiscard]] SV text_body() const noexcept {
		if (auto const *text = get_if<S>(&body_payload)) {
			return *text;
		}
		return {};
	}
	[[nodiscard]] S &text_body_mut() {
		if (!is_text() || !holds_alternative<S>(body_payload)) {
			body_kind = BodyKind::text;
			body_payload = S{};
		}
		return get<S>(body_payload);
	}
	[[nodiscard]] S take_text_body() {
		if (!holds_alternative<S>(body_payload)) {
			return {};
		}
		return move(get<S>(body_payload));
	}
	[[nodiscard]] SP<SseChannel> const &sse_channel_ptr() const {
		static SP<SseChannel> const empty{};
		if (auto const *ch = get_if<SP<SseChannel>>(&body_payload)) {
			return *ch;
		}
		return empty;
	}
	[[nodiscard]] SP<WsUpgrade> const &ws_upgrade_ptr() const {
		static SP<WsUpgrade> const empty{};
		if (auto const *up = get_if<SP<WsUpgrade>>(&body_payload)) {
			return *up;
		}
		return empty;
	}
	[[nodiscard]] SP<MappedBody> const &mapped_file_ptr() const {
		static SP<MappedBody> const empty{};
		if (auto const *file = get_if<SP<MappedBody>>(&body_payload)) {
			return *file;
		}
		return empty;
	}
	[[nodiscard]] SP<StreamedFile> const &streamed_file_ptr() const {
		static SP<StreamedFile> const empty{};
		if (auto const *file = get_if<SP<StreamedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}
	[[nodiscard]] SP<DeferredResponse> const &deferred_response_ptr() const {
		static SP<DeferredResponse> const empty{};
		if (auto const *deferred = get_if<SP<DeferredResponse>>(&body_payload)) {
			return *deferred;
		}
		return empty;
	}
	[[nodiscard]] SP<SseChannel> take_sse_channel() {
		if (!holds_alternative<SP<SseChannel>>(body_payload)) {
			return {};
		}
		return move(get<SP<SseChannel>>(body_payload));
	}
	[[nodiscard]] SP<WsUpgrade> take_ws_upgrade() {
		if (!holds_alternative<SP<WsUpgrade>>(body_payload)) {
			return {};
		}
		return move(get<SP<WsUpgrade>>(body_payload));
	}
	[[nodiscard]] SP<MappedBody> take_mapped_file() {
		if (!holds_alternative<SP<MappedBody>>(body_payload)) {
			return {};
		}
		return move(get<SP<MappedBody>>(body_payload));
	}
	[[nodiscard]] SP<StreamedFile> take_streamed_file() {
		if (!holds_alternative<SP<StreamedFile>>(body_payload)) {
			return {};
		}
		return move(get<SP<StreamedFile>>(body_payload));
	}
	[[nodiscard]] SP<DeferredResponse> take_deferred_response() {
		if (!holds_alternative<SP<DeferredResponse>>(body_payload)) {
			return {};
		}
		return move(get<SP<DeferredResponse>>(body_payload));
	}
	void set_text_body(
		S text) {
		body_kind = BodyKind::text;
		body_payload = move(text);
	}
	void set_sse_channel(
		SP<SseChannel> ch) {
		body_kind = BodyKind::sse;
		body_payload = move(ch);
	}
	void set_ws_upgrade(
		SP<WsUpgrade> up) {
		body_kind = BodyKind::ws_upgrade;
		body_payload = move(up);
	}
	void set_mapped_file(
		SP<MappedBody> file) {
		body_kind = BodyKind::mapped_file;
		body_payload = move(file);
	}
	void set_streamed_file(
		SP<StreamedFile> file) {
		body_kind = BodyKind::streamed_file;
		body_payload = move(file);
	}
	void set_deferred_response(
		SP<DeferredResponse> deferred) {
		body_kind = BodyKind::deferred;
		body_payload = move(deferred);
	}
	[[nodiscard]] SZ content_length() const noexcept {
		if (content_length_hint != 0) {
			return content_length_hint;
		}
		if (is_mapped_file() && mapped_file_ptr()) {
			return static_cast<SZ>(mapped_file_ptr()->size);
		}
		if (is_streamed_file() && streamed_file_ptr()) {
			return static_cast<SZ>(streamed_file_ptr()->send_size);
		}
		return text_body().size();
	}
	[[nodiscard]] static HttpResponse html(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse html(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse json(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse json(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse text(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse text(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse redirect(
		SV location,
		int code = kHttpFound) {
		char const *status_text = "Found";
		switch (code) {
		case kHttpMovedPermanently : status_text = "Moved Permanently"; break;
		case kHttpTemporaryRedirect: status_text = "Temporary Redirect"; break;
		case kHttpPermanentRedirect: status_text = "Permanent Redirect"; break;
		default                    : break;
		}
		HttpResponse r{.status = code, .status_text = status_text, .content_type = "text/html; charset=utf-8"};
		r.headers["Location"] = S{location};
		return r;
	}
	[[nodiscard]] static HttpResponse not_found(
		SV path) {
		HttpResponse r;
		r.status = kHttpNotFound;
		r.status_text = "Not Found";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(format("<html><body><h1>404 Not Found</h1><p>{}</p></body></html>", response_html_escape(path)));
		return r;
	}
	[[nodiscard]] static HttpResponse bad_request(
		SV detail = {}) {
		auto body = detail.empty() ?
						S{"<html><body><h1>400 Bad Request</h1></body></html>"} :
						format("<html><body><h1>400 Bad Request</h1><p>{}</p></body></html>", response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpBadRequest;
		r.status_text = "Bad Request";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse unauthorized(
		SV www_authenticate = {}) {
		HttpResponse r;
		r.status = kHttpUnauthorized;
		r.status_text = "Unauthorized";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>401 Unauthorized</h1></body></html>");
		if (!www_authenticate.empty()) {
			r.headers["WWW-Authenticate"] = S{www_authenticate};
		}
		return r;
	}
	[[nodiscard]] static HttpResponse forbidden(
		SV detail = {}) {
		auto body = detail.empty() ?
						S{"<html><body><h1>403 Forbidden</h1></body></html>"} :
						format("<html><body><h1>403 Forbidden</h1><p>{}</p></body></html>", response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpForbidden;
		r.status_text = "Forbidden";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}
	[[nodiscard]] static HttpResponse method_not_allowed(
		std::initializer_list<SV> allowed = {}) {
		HttpResponse r;
		r.status = kHttpMethodNotAllowed;
		r.status_text = "Method Not Allowed";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>405 Method Not Allowed</h1></body></html>");
		if (allowed.size() > 0) {
			S allow;
			for (auto it = allowed.begin(); it != allowed.end(); ++it) {
				if (it != allowed.begin()) {
					allow += ", ";
				}
				allow += *it;
			}
			r.headers["Allow"] = move(allow);
		}
		return r;
	}
	[[nodiscard]] static HttpResponse unprocessable_entity(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>422 Unprocessable Entity</h1></body></html>"} :
				format("<html><body><h1>422 Unprocessable Entity</h1><p>{}</p></body></html>", response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpUnprocessableEntity;
		r.status_text = "Unprocessable Entity";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
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
		SV detail = {}) {
		HttpResponse r;
		r.status = kHttpBadGateway;
		r.status_text = "Bad Gateway";
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(detail.empty() ? "Bad Gateway" : S{detail});
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
		SP<SseChannel> ch) {
		HttpResponse r{.status = kHttpOk, .status_text = "OK", .content_type = "text/event-stream"};
		r.set_sse_channel(move(ch));
		return r;
	}
	[[nodiscard]] static HttpResponse deferred(
		SP<DeferredResponse> response) {
		HttpResponse r;
		r.set_deferred_response(move(response));
		return r;
	}
	[[nodiscard]] static HttpResponse internal_error(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>500 Internal Server Error</h1></body></html>"} :
				format("<html><body><h1>500 Internal Server Error</h1><p>{}</p></body></html>", response_html_escape(detail));
		HttpResponse r;
		r.status = kHttpInternalServerError;
		r.status_text = "Internal Server Error";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
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
		SV name,
		SV cookie_value,
		SV attributes = {}) {
		if (attributes.empty()) {
			set_cookies.push_back(format("{}={}", name, cookie_value));
		} else {
			set_cookies.push_back(format("{}={}; {}", name, cookie_value, attributes));
		}
		return *this;
	}
	void append_vary(
		SV token) {
		auto const current = SV{headers["Vary"]};
		if (current.empty()) {
			headers["Vary"] = S{token};
			return;
		}
		auto is_ws = [](char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		auto trim_sv = [&](SV s) noexcept -> SV {
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
			auto const part = trim_sv((comma == SV::npos) ? vary : vary.substr(0, comma));
			if (conflux::http::ascii_iequals(part, token)) {
				return;
			}
			if (comma == SV::npos) {
				break;
			}
			vary.remove_prefix(comma + 1);
		}
		headers["Vary"] = format("{}, {}", current, token);
	}
};
export class DeferredResponse {
	int efd_{-1};
	mutable mutex mtx_{};
	UP<HttpResponse> ready_{};
	chrono::steady_clock::time_point deadline_{};
	conflux::work::root::TaskControl cancel_ctl_{};

public:
	static constexpr chrono::milliseconds kDefaultTimeout{30000};

	explicit DeferredResponse(chrono::milliseconds timeout = kDefaultTimeout);
	~DeferredResponse() noexcept;
	DeferredResponse(DeferredResponse const &) = delete;
	DeferredResponse &operator =(DeferredResponse const &) = delete;
	DeferredResponse(DeferredResponse &&) = delete;
	DeferredResponse &operator =(DeferredResponse &&) = delete;

	[[nodiscard]] int eventfd_fd() const noexcept;
	void complete(HttpResponse response);
	[[nodiscard]] bool is_ready() const;
		[[nodiscard]] Opt<HttpResponse> take_ready();
	[[nodiscard]] chrono::steady_clock::time_point deadline() const;
	void set_deadline(chrono::steady_clock::time_point deadline);
	void attach_cancel(conflux::work::root::TaskControl ctl) noexcept;
	// Force-complete with 504 if the deadline has passed and no response is ready.
	// Returns true if this call expired the response (i.e. the caller should expect
	// the ready signal to fire).
	bool expire_if_past_deadline(chrono::steady_clock::time_point now);
};
DeferredResponse::DeferredResponse(
	chrono::milliseconds timeout)
	: efd_{::eventfd(0, EFD_CLOEXEC)}
	, deadline_{chrono::steady_clock::now() + timeout} {
	if (efd_ < 0) {
		throw SE{errno, system_category(), "eventfd"};
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
		SL const lk{mtx_};
		if (ready_) {
			return;
		}
		ready_ = make_unique<HttpResponse>(move(response));
	}
	u64 wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		eprintln(format("DeferredResponse::complete: eventfd write: {}", strerror(errno)));
	}
}
bool DeferredResponse::is_ready() const {
	SL const lk{mtx_};
	return ready_ != nullptr;
}
Opt<HttpResponse> DeferredResponse::take_ready() {
	SL const lk{mtx_};
	if (!ready_) {
		return nullopt;
	}
	auto response = move(*ready_);
	ready_.reset();
	return response;
}
chrono::steady_clock::time_point DeferredResponse::deadline() const {
	SL const lk{mtx_};
	return deadline_;
}
void DeferredResponse::set_deadline(
	chrono::steady_clock::time_point deadline) {
	SL const lk{mtx_};
	deadline_ = deadline;
}
void DeferredResponse::attach_cancel(
	conflux::work::root::TaskControl ctl) noexcept {
	SL const lk{mtx_};
	cancel_ctl_ = move(ctl);
}
bool DeferredResponse::expire_if_past_deadline(
	chrono::steady_clock::time_point now) {
	conflux::work::root::TaskControl to_cancel;
	{
		SL const lk{mtx_};
		if (ready_) {
			return false;
		}
		if (now < deadline_) {
			return false;
		}
		ready_ = make_unique<HttpResponse>(HttpResponse::gateway_timeout());
		to_cancel = move(cancel_ctl_);
	}
	auto _ = to_cancel.request_cancel();
	u64 wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		eprintln(format("DeferredResponse::expire_if_past_deadline: eventfd write: {}", strerror(errno)));
	}
	return true;
}
