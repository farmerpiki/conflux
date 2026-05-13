module;
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
export module conflux.net.router;


import std;
import conflux.types;
import conflux.net.http.types;
import conflux.work;
import conflux.file_io;
import conflux.utils;
import conflux.net.config;
import conflux.socket_io;
export import conflux.net.http.server_types;
export import conflux.net.http.realtime;
export import conflux.net.http.static_files;
[[nodiscard]] S html_escape(
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
[[nodiscard]] S path_percent_encode(
	SV s) {
	S out;
	out.reserve(s.size());
	static constexpr char kHex[] = "0123456789ABCDEF";
	for (char const ch: s) {
		auto const c = static_cast<unsigned char>(ch);
		bool const safe = (c >= 'A' && c <= 'Z')
					   || (c >= 'a' && c <= 'z')
					   || (c >= '0' && c <= '9')
					   || c == '-'
					   || c == '_'
					   || c == '.'
					   || c == '~'
					   || c == '/';
		if (safe) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4U]);
			out.push_back(kHex[c & 0x0FU]);
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
		r.set_text_body(format("<html><body><h1>404 Not Found</h1><p>{}</p></body></html>", html_escape(path)));
		return r;
	}
	[[nodiscard]] static HttpResponse bad_request(
		SV detail = {}) {
		auto body = detail.empty() ?
						S{"<html><body><h1>400 Bad Request</h1></body></html>"} :
						format("<html><body><h1>400 Bad Request</h1><p>{}</p></body></html>", html_escape(detail));
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
						format("<html><body><h1>403 Forbidden</h1><p>{}</p></body></html>", html_escape(detail));
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
				format("<html><body><h1>422 Unprocessable Entity</h1><p>{}</p></body></html>", html_escape(detail));
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
				format("<html><body><h1>500 Internal Server Error</h1><p>{}</p></body></html>", html_escape(detail));
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
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

struct Segment {
	S value;
	bool is_param; // true → {name} single-segment capture
	bool is_wildcard; // true → {*name} greedy tail capture (must be last segment)
};
V<Segment> parse_pattern(
	SV pattern) {
	V<Segment> segs;
	SZ pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == SV::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({S{part.substr(2, part.size() - 3)}, false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({S{part.substr(1, part.size() - 2)}, true, false});
		} else {
			segs.push_back({S{part}, false, false});
		}

		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}
bool match_segments(
	V<Segment> const &pattern,
	SV path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		SZ pos = 0;
		HttpFieldsView tmp;
		for (SZ i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == SV::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(part));
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == SV::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		tmp.emplace_back_owned(S{pattern.back().value}, url_decode_path(path.substr(pos)));
		out_params = move(tmp);
		return true;
	}

	V<SV> parts;
	SZ pos = 0;
	while (true) {
		auto next = path.find('/', pos);
		parts.push_back((next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos));
		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}

	if (parts.size() != pattern.size()) {
		return false;
	}

	HttpFieldsView tmp;
	for (SZ i = 0; i < pattern.size(); ++i) {
		if (pattern[i].is_param) {
			tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(parts[i]));
		} else if (pattern[i].value != parts[i]) {
			return false;
		}
	}
	out_params = move(tmp);
	return true;
}
// Metadata for a single registered route, exposed by Router::route_infos().
export struct RouteInfo {
	S method;
	S path_pattern; // OpenAPI-style path e.g. /users/{id}
	V<S> path_params; // captured parameter names in order
};
// Reconstruct an OpenAPI path S from a parsed Segment V.
// The first segment is always an empty literal (artifact of the leading '/');
// skip it so the result starts with a single '/'.
inline S segments_to_pattern(
	V<Segment> const &segs) {
	S out;
	bool first = true;
	for (auto const &seg: segs) {
		if (first && seg.value.empty() && !seg.is_param && !seg.is_wildcard) {
			first = false;
			continue; // skip leading empty segment
		}
		first = false;
		out += '/';
		if (seg.is_wildcard || seg.is_param) {
			out += '{';
			out += seg.value;
			out += '}';
		} else {
			out += seg.value;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}
int contained_open(
	int root_fd,
	char const *relative,
	int flags,
	mode_t mode = 0) noexcept {
	open_how how{};
	how.flags = static_cast<__u64>(flags);
	how.mode = static_cast<__u64>(mode);
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
	return static_cast<int>(::syscall(SYS_openat2, root_fd, relative, &how, sizeof(how)));
}
struct RootDirFd {
	int fd{-1};
	explicit RootDirFd(
		char const *path)
		: fd(::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
	~RootDirFd() noexcept {
		if (fd >= 0) {
			::close(fd);
		}
	}
	RootDirFd(RootDirFd const &) = delete;
	RootDirFd &operator =(RootDirFd const &) = delete;
	RootDirFd(
		RootDirFd &&o) noexcept
		: fd(exchange(o.fd, -1)) {}
	RootDirFd &operator =(
		RootDirFd &&o) noexcept {
		if (fd >= 0) {
			::close(fd);
		}
		fd = exchange(o.fd, -1);
		return *this;
	}
};
export struct RequestContext {
	SocketTaskRing &ring;
};

export using NextHandler = CloneableFunction<HttpResponse(HttpRequestView const &)>;
export using MiddlewareFunction = CloneableFunction<HttpResponse(HttpRequestView const &, NextHandler const &)>;

export template<class R>
concept HandlerResult = same_as<R, HttpResponse> || same_as<R, conflux::work::root::Task<HttpResponse>>;

export template<class F>
concept ViewHandler = requires(std::decay_t<F> &fn, HttpRequestView const &req) {
	{ std::invoke(fn, req) } -> same_as<HttpResponse>;
};

export template<class F>
concept RequestHandler = requires(std::decay_t<F> &fn, HttpRequest const &req) {
	{ std::invoke(fn, req) } -> HandlerResult;
};

export template<class F>
concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

export template<class F>
concept ContextHandlerFunction = requires(std::decay_t<F> &fn, HttpRequest const &req, RequestContext const &ctx) {
	{ std::invoke(fn, req, ctx) } -> same_as<conflux::work::root::Task<HttpResponse>>;
};

export template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, HttpRequestView const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> same_as<HttpResponse>;
};

export template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, HttpRequest const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> same_as<HttpResponse>;
};

export template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F>;

export class Router {
public:
	using Handler = NextHandler;
	using ContextHandler =
		CloneableFunction<conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &)>;
	using ContextMiddleware = CloneableFunction<
		conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &, ContextHandler const &)>;
	using SseHandler = CloneableFunction<void(HttpRequestView const &, SP<SseChannel>)>;
	// next is the downstream handler (or next middleware); call it to continue the chain.
	using Middleware = MiddlewareFunction;
	using WsHandler = CloneableFunction<void(HttpRequestView const &, WsConn &)>;
	using ErrorHandler = CloneableFunction<HttpResponse(HttpRequestView const &, exception const &)>;
	Router();
	explicit Router(Config const &cfg);
	~Router();
	Router(Router const &) = delete;
	Router &operator =(Router const &) = delete;
	Router(Router &&o) noexcept;
	Router &operator =(Router &&o) noexcept;
	// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks): false positive on CloneableFunction ownership.
	template<typename F>
	Router &add(
		SV method,
		SV path,
		F &&handler) {
		add_prepared(method, path, make_handler(forward<F>(handler)));
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &add_context(
		SV method,
		SV path,
		F &&handler) {
		add_context_prepared(method, path, ContextHandler{forward<F>(handler)});
		return *this;
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
	[[nodiscard]] bool has_context_routes() const noexcept;
	template<typename F>
	Router &get(
		SV path,
		F &&handler) {
		return add("GET", path, forward<F>(handler));
	}
	template<typename F>
	Router &post(
		SV path,
		F &&handler) {
		return add("POST", path, forward<F>(handler));
	}
	template<typename F>
	Router &put(
		SV path,
		F &&handler) {
		return add("PUT", path, forward<F>(handler));
	}
	template<typename F>
	Router &patch(
		SV path,
		F &&handler) {
		return add("PATCH", path, forward<F>(handler));
	}
	template<typename F>
	Router &del(
		SV path,
		F &&handler) {
		return add("DELETE", path, forward<F>(handler));
	}
	template<typename F>
	Router &options(
		SV path,
		F &&handler) {
		return add("OPTIONS", path, forward<F>(handler));
	}
	template<typename F>
	Router &use(
		F &&mw) {
		use_prepared(make_middleware(forward<F>(mw)));
		return *this;
	}
	template<typename F>
	Router &on_not_found(
		F &&handler) {
		set_not_found_handler(make_handler(forward<F>(handler)));
		return *this;
	}
	template<typename F>
	Router &on_error(
		F &&handler) {
		set_error_handler(make_error_handler(forward<F>(handler)));
		return *this;
	}
	// Return metadata for all registered routes (regular routes only).
	[[nodiscard]] V<RouteInfo> route_infos() const;
	template<typename F>
	Router &sse(
		SV path,
		F &&handler) {
		sse_prepared(path, make_sse_handler(forward<F>(handler)));
		return *this;
	}
	Router &set_work_pool(SP<WorkPool> pool);
	[[nodiscard]] SP<WorkPool> work_pool() const;
	Router &set_static_file_cache(StaticFileCacheConfig cfg);
	// Register a WebSocket upgrade handler. GET requests with a valid Upgrade: websocket
	// handshake are upgraded to WebSocket; the handler runs on the router's work pool.
	template<typename F>
	Router &ws(
		SV path,
		F &&handler) {
		return ws_prepared(path, make_ws_handler(forward<F>(handler)));
	}
	// Route group: scopes a set of routes under a path prefix with Opt group-local middleware.
	// Group middleware wraps only the routes registered inside the group callback;
	// it does NOT affect routes registered outside. The group callback receives a Group&.
	class Group {
	public:
		template<typename F>
		Group &use(
			F &&mw) {
			middlewares_.push_back(Router::make_middleware(forward<F>(mw)));
			return *this;
		}
		template<typename F>
		Group &add(
			SV method,
			SV path,
			F &&handler) {
			router_.add(method, prefix_ + S{path}, wrap(Router::make_handler(forward<F>(handler))));
			return *this;
		}
		template<typename F>
		Group &get(
			SV path,
			F &&handler) {
			return add("GET", path, forward<F>(handler));
		}
		template<typename F>
		Group &post(
			SV path,
			F &&handler) {
			return add("POST", path, forward<F>(handler));
		}
		template<typename F>
		Group &put(
			SV path,
			F &&handler) {
			return add("PUT", path, forward<F>(handler));
		}
		template<typename F>
		Group &patch(
			SV path,
			F &&handler) {
			return add("PATCH", path, forward<F>(handler));
		}
		template<typename F>
		Group &del(
			SV path,
			F &&handler) {
			return add("DELETE", path, forward<F>(handler));
		}
		template<typename F>
		Group &options(
			SV path,
			F &&handler) {
			return add("OPTIONS", path, forward<F>(handler));
		}

	private:
		friend class Router;
		Group(
			Router &router,
			S prefix)
			: router_(router)
			, prefix_(move(prefix))
			, middlewares_{} {}
		// Apply group middlewares around h (innermost first, so first-registered is outermost).
		// Capture mw by value: the Group object is destroyed after router.group() returns,
		// so capturing by reference would dangle.
		[[nodiscard]] Handler wrap(
			Handler h) const {
			for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
				auto mw = middlewares_[static_cast<SZ>(i)]; // copy: Group is destroyed after group() returns
				h = [mw = move(mw), n = move(h)](HttpRequestView const &r) { return mw(r, n); };
			}
			return h;
		}
		Router &router_;
		S prefix_;
		V<Middleware> middlewares_;
	};
	template<typename F>
	Router &group(
		SV prefix,
		F &&fn) {
		Group g{*this, S{prefix}};
		forward<F>(fn)(g);
		return *this;
	}
	// Serve static files from root_dir for GET/HEAD requests under url_prefix.
	// url_prefix must not end with '/'. Files are served at url_prefix/{*file}.
	// Path traversal ("..") is rejected with 403.
	// ETag based on size+mtime; Range requests (206 Partial Content) supported.
	// Pre-compressed sidecar files (.gz, .br) served when client accepts them.
	Router &serve_static(
		SV url_prefix,
		S root_dir,
		StaticOptions const &sopts = {});
	[[nodiscard]] HttpResponse dispatch(HttpRequest const &req) const;
	[[nodiscard]] HttpResponse dispatch(HttpRequestView const &req) const;
	[[nodiscard]] Opt<HttpResponse> dispatch_async(
		HttpRequest const &req,
		RequestContext const &ctx) const;

private:
	struct Impl;
	UP<Impl> impl_;
	void add_prepared(SV method, SV path, Handler handler);
	void add_context_prepared(SV method, SV path, ContextHandler handler);
	void use_prepared(Middleware mw);
	void set_not_found_handler(Handler handler);
	void set_error_handler(ErrorHandler handler);
	void sse_prepared(SV path, SseHandler handler);
	Router &ws_prepared(SV path, WsHandler handler);
	static void launch_sse_handler(
		SP<WorkPool> const &pool,
		SseHandler handler,
		HttpRequest matched,
		SP<SseChannel> const &channel);
	[[nodiscard]] Handler wrap_middlewares(Handler h) const;
	[[nodiscard]] static HttpResponse run_async_http_task(
		conflux::work::root::Task<HttpResponse> task);
	template<class>
	static constexpr bool kDependentFalse = false;
	template<typename F>
	static Handler make_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequestView const &>;
			if constexpr (same_as<Ret, HttpResponse>) {
				return Handler{forward<F>(fn)};
			} else if constexpr (same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				static_assert(
					kDependentFalse<Fn>,
					"Async handlers must take HttpRequest const&, not HttpRequestView const& — "
					"the view can dangle after coroutine suspension");
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler taking HttpRequestView const& must return HttpResponse (sync only)");
			}
		} else if constexpr (std::invocable<Fn &, HttpRequest const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequest const &>;
			if constexpr (same_as<Ret, HttpResponse>) {
				return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned);
				}};
			} else if constexpr (same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return run_async_http_task(invoke(wrapped, owned));
				}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler returning HttpRequest const& must return HttpResponse or root::Task<HttpResponse>");
			}
		} else {
			static_assert(kDependentFalse<Fn>, "Handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static Middleware make_middleware(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, Handler const &>) {
			return Middleware{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, Handler const &>) {
			return Middleware{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, Handler const &next) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, next);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Middleware must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static SseHandler make_sse_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, SP<SseChannel>>) {
			return SseHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, SP<SseChannel>>) {
			return SseHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, SP<SseChannel> ch) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, move(ch));
			}};
		} else {
			static_assert(kDependentFalse<Fn>, "SSE handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static WsHandler make_ws_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, WsConn &>) {
			return WsHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, WsConn &>) {
			return WsHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, WsConn &ws) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, ws);
			}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"WebSocket handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static ErrorHandler make_error_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, exception const &>) {
			return ErrorHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, exception const &>) {
			return ErrorHandler{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, exception const &ex) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, ex);
				}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"Error handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
};
// Returns a middleware that formats each request as:
//   [ISO8601] METHOD path status bytes elapsed_ms
// and passes the formatted line to `sink`. Thread-safety of `sink` is
// the caller's responsibility.
export Router::Middleware make_access_log_middleware(
	Fn<void(S const &)> sink) {
	return [sink = move(sink)](HttpRequestView const &req, Router::Handler const &next) {
		auto const t0 = chrono::steady_clock::now();
		auto resp = next(req);
		auto const elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

		auto const now = chrono::system_clock::now();
		auto const tt = chrono::system_clock::to_time_t(now);
		A<char, 32> ts_buf{};
		SV ts{};
		if (strftime(ts_buf.data(), ts_buf.size(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt)) > 0) {
			ts = ts_buf.data();
		}

		sink(format("[{}] {} {} {} {} {}ms", ts, req.method, req.path, resp.status, resp.text_body().size(), elapsed));
		return resp;
	};
}
