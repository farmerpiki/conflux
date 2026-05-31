module;

export module conflux.net.http.server_types;

import std;
import conflux.types;
import conflux.net.http.types;

// ---------------------------------------------------------------------------
// HTTP server run/telemetry types shared across the primary module and its
// partitions.
// ---------------------------------------------------------------------------

export namespace conflux::http {

class RequestRingRef {
	void *ptr_{};

public:
	RequestRingRef() noexcept = default;
	template<class Ring>
	explicit RequestRingRef(
		Ring &ring) noexcept
		: ptr_{std::addressof(ring)} {}
	template<class Ring>
	[[nodiscard]] Ring &as() const noexcept {
		return *static_cast<Ring *>(ptr_);
	}
	template<class Ring>
	[[nodiscard]] operator Ring &() const noexcept {
		return as<Ring>();
	}
	[[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

struct RequestContext {
	RequestRingRef ring;
};

} // namespace conflux::http

export namespace conflux::http {

// NOLINTNEXTLINE(performance-enum-size)
enum class RunStatus : std::uint8_t {
	stopped_normally,
	fatal_cq_overflow,
	fatal_cq_overflow_no_nodrop,
	fatal_submit_wait_ebadr,
	fatal_internal_exception,
};

} // namespace conflux::http

export namespace conflux::http {

struct SendZcMetrics {
	std::uint64_t attempts{};
	std::uint64_t plain_attempts{};
	std::uint64_t mapped_attempts{};
	std::uint64_t bytes_requested{};
	std::uint64_t bytes_sent{};
	std::uint64_t notifications{};
	std::uint64_t copied_notifications{};
	std::uint64_t sends_without_notification{};
	std::uint64_t errors_enomem{};
	std::uint64_t errors_other{};
	std::uint64_t fallback_regular_send{};
	std::uint64_t tls_bypass{};
	std::uint64_t tls_bypass_bytes{};
	std::uint64_t adaptive_disable_count{};
};

} // namespace conflux::http

export namespace conflux::http {

enum class HttpRejectReason : std::uint8_t {
	none,
	malformed_request,
	request_line_too_large,
	header_line_too_large,
	header_block_too_large,
	too_many_headers,
	missing_host,
	duplicate_host,
	malformed_content_length,
	duplicate_content_length,
	content_length_with_transfer_encoding,
	unsupported_transfer_encoding,
	invalid_transfer_encoding,
	invalid_chunk,
	body_too_large,
	expectation_failed,
	header_timeout,
	body_timeout,
};

[[nodiscard]] constexpr std::string_view reject_reason_code(
	HttpRejectReason reason) noexcept {
	switch (reason) {
	case HttpRejectReason::none                                 : return "none";
	case HttpRejectReason::malformed_request                    : return "malformed_request";
	case HttpRejectReason::request_line_too_large               : return "request_line_too_large";
	case HttpRejectReason::header_line_too_large                : return "header_line_too_large";
	case HttpRejectReason::header_block_too_large               : return "header_block_too_large";
	case HttpRejectReason::too_many_headers                     : return "too_many_headers";
	case HttpRejectReason::missing_host                         : return "missing_host";
	case HttpRejectReason::duplicate_host                       : return "duplicate_host";
	case HttpRejectReason::malformed_content_length             : return "malformed_content_length";
	case HttpRejectReason::duplicate_content_length             : return "duplicate_content_length";
	case HttpRejectReason::content_length_with_transfer_encoding: return "content_length_with_transfer_encoding";
	case HttpRejectReason::unsupported_transfer_encoding        : return "unsupported_transfer_encoding";
	case HttpRejectReason::invalid_transfer_encoding            : return "invalid_transfer_encoding";
	case HttpRejectReason::invalid_chunk                        : return "invalid_chunk";
	case HttpRejectReason::body_too_large                       : return "body_too_large";
	case HttpRejectReason::expectation_failed                   : return "expectation_failed";
	case HttpRejectReason::header_timeout                       : return "header_timeout";
	case HttpRejectReason::body_timeout                         : return "body_timeout";
	}
	return "malformed_request";
}

[[nodiscard]] constexpr std::string_view reject_reason_diagnostic_code(
	HttpRejectReason reason) noexcept {
	switch (reason) {
	case HttpRejectReason::header_block_too_large: return "http.reject.header_block_too_large";
	case HttpRejectReason::content_length_with_transfer_encoding:
		return "http.reject.content_length_with_transfer_encoding";
	default: return reject_reason_code(reason);
	}
}

[[nodiscard]] constexpr int reject_reason_status(
	HttpRejectReason reason) noexcept {
	switch (reason) {
	case HttpRejectReason::request_line_too_large: return 414;
	case HttpRejectReason::header_line_too_large :
	case HttpRejectReason::header_block_too_large:
	case HttpRejectReason::too_many_headers      : return 431;
	case HttpRejectReason::body_too_large        : return 413;
	case HttpRejectReason::expectation_failed    : return 417;
	case HttpRejectReason::header_timeout        :
	case HttpRejectReason::body_timeout          : return 408;
	case HttpRejectReason::none                  : return 200;
	default                                      : return 400;
	}
}

[[nodiscard]] constexpr std::string_view reject_reason_detail(
	HttpRejectReason reason) noexcept {
	switch (reason) {
	case HttpRejectReason::malformed_request       : return "request syntax is invalid";
	case HttpRejectReason::request_line_too_large  : return "request line exceeds the configured limit";
	case HttpRejectReason::header_line_too_large   : return "a header line exceeds the configured limit";
	case HttpRejectReason::header_block_too_large  : return "request headers exceed the configured aggregate limit";
	case HttpRejectReason::too_many_headers        : return "request contains too many headers";
	case HttpRejectReason::missing_host            : return "HTTP/1.1 request is missing a Host header";
	case HttpRejectReason::duplicate_host          : return "HTTP/1.1 request contains more than one Host header";
	case HttpRejectReason::malformed_content_length: return "Content-Length is not a valid decimal length";
	case HttpRejectReason::duplicate_content_length: return "request contains more than one Content-Length header";
	case HttpRejectReason::content_length_with_transfer_encoding:
		return "request contains both Content-Length and Transfer-Encoding";
	case HttpRejectReason::unsupported_transfer_encoding: return "Transfer-Encoding is not supported for this request";
	case HttpRejectReason::invalid_transfer_encoding    : return "Transfer-Encoding header is invalid";
	case HttpRejectReason::invalid_chunk                : return "chunked request body framing is invalid";
	case HttpRejectReason::body_too_large               : return "request body exceeds the configured limit";
	case HttpRejectReason::expectation_failed           : return "request Expect header cannot be satisfied";
	case HttpRejectReason::header_timeout               : return "request headers were not received before the timeout";
	case HttpRejectReason::body_timeout                 : return "request body was not received before the timeout";
	case HttpRejectReason::none                         : return "";
	}
	return "request syntax is invalid";
}

struct HttpServerObservabilityHooks {
	std::function<void(HttpRejectReason, int)> rejection = {};
};

struct HttpRejectionMetrics {
	std::uint64_t malformed_request{};
	std::uint64_t request_line_too_large{};
	std::uint64_t header_line_too_large{};
	std::uint64_t header_block_too_large{};
	std::uint64_t too_many_headers{};
	std::uint64_t missing_host{};
	std::uint64_t duplicate_host{};
	std::uint64_t malformed_content_length{};
	std::uint64_t duplicate_content_length{};
	std::uint64_t content_length_with_transfer_encoding{};
	std::uint64_t unsupported_transfer_encoding{};
	std::uint64_t invalid_transfer_encoding{};
	std::uint64_t invalid_chunk{};
	std::uint64_t body_too_large{};
	std::uint64_t expectation_failed{};
	std::uint64_t header_timeout{};
	std::uint64_t body_timeout{};
};

} // namespace conflux::http

export namespace conflux::http {

enum class SendZcPendingAction : std::uint8_t {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
};

enum class SendZcCqeAction : std::uint8_t {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
	close_after_notification,
};

struct SendZcCqeState {
	bool waiting_notification{};
	bool close_after_notification{};
	SendZcPendingAction after_notification{SendZcPendingAction::none};
};

struct SendZcCqeInput {
	int result{};
	bool notification{};
	bool more{};
	bool copied{};
	bool enomem_error{};
	std::size_t written_before{};
	std::size_t response_total{};
};

struct SendZcCqeOutcome {
	SendZcCqeAction action{SendZcCqeAction::none};
	std::size_t bytes_sent{};
	bool adaptive_disabled{};
};

[[nodiscard]] SendZcCqeOutcome observe_send_zc_cqe(
	SendZcCqeState &state,
	conflux::http::SendZcMetrics &metrics,
	SendZcCqeInput input,
	bool &send_zc_enabled) noexcept {
	SendZcCqeOutcome out{};
	if (input.notification) {
		++metrics.notifications;
		if (input.copied) {
			++metrics.copied_notifications;
			if (send_zc_enabled
				&& metrics.attempts >= 1024
				&& metrics.bytes_requested >= std::size_t{16} * 1024 * 1024
				&& metrics.copied_notifications * 10 > metrics.notifications * 9) {
				send_zc_enabled = false;
				++metrics.adaptive_disable_count;
				out.adaptive_disabled = true;
			}
		}
		state.waiting_notification = false;
		if (state.close_after_notification) {
			state.close_after_notification = false;
			state.after_notification = SendZcPendingAction::none;
			out.action = SendZcCqeAction::close_after_notification;
			return out;
		}
		auto const action = state.after_notification;
		state.after_notification = SendZcPendingAction::none;
		switch (action) {
		case SendZcPendingAction::complete_response: out.action = SendZcCqeAction::complete_response; break;
		case SendZcPendingAction::resubmit_response: out.action = SendZcCqeAction::resubmit_response; break;
		case SendZcPendingAction::close_after_error: out.action = SendZcCqeAction::close_after_error; break;
		default                                    : break;
		}
		return out;
	}

	auto const note_error = [&] {
		if (input.enomem_error) {
			++metrics.errors_enomem;
		} else {
			++metrics.errors_other;
		}
	};

	if (input.more) {
		state.waiting_notification = true;
		if (input.result < 0) {
			note_error();
			state.after_notification = SendZcPendingAction::close_after_error;
			return out;
		}
		out.bytes_sent = static_cast<std::size_t>(input.result);
		metrics.bytes_sent += out.bytes_sent;
		state.after_notification = input.written_before + out.bytes_sent >= input.response_total ?
									   SendZcPendingAction::complete_response :
									   SendZcPendingAction::resubmit_response;
		return out;
	}

	++metrics.sends_without_notification;
	if (input.result < 0) {
		note_error();
		out.action = SendZcCqeAction::close_after_error;
		return out;
	}
	out.bytes_sent = static_cast<std::size_t>(input.result);
	metrics.bytes_sent += out.bytes_sent;
	out.action = input.written_before + out.bytes_sent < input.response_total ? SendZcCqeAction::resubmit_response :
																				SendZcCqeAction::complete_response;
	return out;
}

} // namespace conflux::http

export namespace conflux::http {

struct HttpServerMetrics {
	std::uint64_t sq_dropped{};
	std::uint64_t cq_overflow{};
	std::uint64_t accepted_direct_failures{};
	std::uint64_t zc_notifications_pending{};
	std::uint64_t zc_capable_rings{};
	std::uint64_t zc_enabled_rings{};
	std::uint64_t recv_bundle_cqes{};
	std::uint64_t recv_bundle_slices{};
	std::uint64_t recv_bundle_bytes{};
	SendZcMetrics send_zc{};
	HttpRejectionMetrics rejections{};
	struct StaticFileMetrics {
		std::uint64_t mapped_responses{};
		std::uint64_t streamed_responses{};
		std::uint64_t splice_submits{};
		std::uint64_t tls_read_fixed_submits{};
		std::uint64_t tls_mapped_plaintext_chunks{};
	} static_files{};
	struct HttpPressureMetrics {
		std::uint64_t accept_rejected{};
		std::uint64_t connections_closed_for_pressure{};
		std::uint64_t response_backpressure_events{};
		std::uint64_t sse_dropped_newest{};
		std::uint64_t sse_dropped_oldest{};
		std::uint64_t sse_disconnected_for_pressure{};
		std::uint64_t websocket_closed_for_pressure{};
		std::uint64_t drain_started{};
		std::uint64_t drain_deadline_hit{};
		std::uint64_t drain_forced_close{};
	} pressure{};
};

using HttpPressureMetrics = HttpServerMetrics::HttpPressureMetrics;

enum class OverflowPolicy : std::uint8_t {
	reject,
	drop_oldest,
	drop_newest,
	close_connection,
	backpressure,
};

enum class DrainStreamPolicy : std::uint8_t {
	close,
	close_with_reason,
	close_with_retry,
	leave_open,
};

struct DrainOptions {
	std::chrono::milliseconds deadline{30000};
	bool stop_accepting = true;
	bool close_idle = true;
	bool finish_requests = true;
	bool finish_streams = false;
	DrainStreamPolicy websocket_policy = DrainStreamPolicy::close;
	DrainStreamPolicy sse_policy = DrainStreamPolicy::close;
};

struct DrainReport {
	std::uint64_t accepted_before_stop = 0;
	std::uint64_t idle_closed = 0;
	std::uint64_t requests_finished = 0;
	std::uint64_t streams_closed = 0;
	std::uint64_t forced_closed = 0;
	bool deadline_hit = false;
};

} // namespace conflux::http

export namespace conflux::http {

struct UploadedFile {
	std::string_view name;
	std::string_view filename;
	std::string_view content_type;
	std::string_view data;
	std::string owned_name;
	std::string owned_filename;
	std::string owned_content_type;
	std::string owned_data;
	bool owns_metadata = false;
	bool owns_data = false;

	UploadedFile() = default;
	UploadedFile(
		std::string_view name_,
		std::string_view filename_,
		std::string_view content_type_,
		std::string_view data_)
		: name(name_)
		, filename(filename_)
		, content_type(content_type_)
		, data(data_) {}
	UploadedFile(
		UploadedFile const &other)
		: owned_name(other.owns_metadata ? other.owned_name : std::string{})
		, owned_filename(other.owns_metadata ? other.owned_filename : std::string{})
		, owned_content_type(other.owns_metadata ? other.owned_content_type : std::string{})
		, owned_data(other.owns_data ? other.owned_data : std::string{})
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		name = owns_metadata ? std::string_view{owned_name} : other.name;
		filename = owns_metadata ? std::string_view{owned_filename} : other.filename;
		content_type = owns_metadata ? std::string_view{owned_content_type} : other.content_type;
		data = owns_data ? std::string_view{owned_data} : other.data;
	}
	UploadedFile(
		UploadedFile &&other) noexcept
		: name(other.name)
		, filename(other.filename)
		, content_type(other.content_type)
		, data(other.data)
		, owned_name(std::move(other.owned_name))
		, owned_filename(std::move(other.owned_filename))
		, owned_content_type(std::move(other.owned_content_type))
		, owned_data(std::move(other.owned_data))
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
	}
	UploadedFile &operator =(
		UploadedFile const &other) {
		if (this == &other) {
			return *this;
		}
		owned_name = other.owns_metadata ? other.owned_name : std::string{};
		owned_filename = other.owns_metadata ? other.owned_filename : std::string{};
		owned_content_type = other.owns_metadata ? other.owned_content_type : std::string{};
		owned_data = other.owns_data ? other.owned_data : std::string{};
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		name = owns_metadata ? std::string_view{owned_name} : other.name;
		filename = owns_metadata ? std::string_view{owned_filename} : other.filename;
		content_type = owns_metadata ? std::string_view{owned_content_type} : other.content_type;
		data = owns_data ? std::string_view{owned_data} : other.data;
		return *this;
	}
	UploadedFile &operator =(
		UploadedFile &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		name = other.name;
		filename = other.filename;
		content_type = other.content_type;
		data = other.data;
		owned_name = std::move(other.owned_name);
		owned_filename = std::move(other.owned_filename);
		owned_content_type = std::move(other.owned_content_type);
		owned_data = std::move(other.owned_data);
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
		return *this;
	}
	[[nodiscard]] static UploadedFile borrowed(
		std::string_view name_,
		std::string_view filename_,
		std::string_view content_type_,
		std::string_view data_) {
		return UploadedFile{name_, filename_, content_type_, data_};
	}
	[[nodiscard]] static UploadedFile owned(
		std::string name_,
		std::string filename_,
		std::string content_type_,
		std::string data_) {
		UploadedFile file{std::string_view{}, std::string_view{}, std::string_view{}, std::string_view{}};
		file.owned_name = std::move(name_);
		file.owned_filename = std::move(filename_);
		file.owned_content_type = std::move(content_type_);
		file.owned_data = std::move(data_);
		file.name = file.owned_name;
		file.filename = file.owned_filename;
		file.content_type = file.owned_content_type;
		file.data = file.owned_data;
		file.owns_metadata = true;
		file.owns_data = true;
		return file;
	}
	[[nodiscard]] UploadedFile to_owned() const {
		if (owns_metadata && owns_data) {
			return *this;
		}
		return UploadedFile::owned(
			std::string{name},
			std::string{filename},
			std::string{content_type},
			std::string{data});
	}
};

enum class HttpFieldSource : std::uint8_t {
	params,
	headers,
	query,
	form,
	cookies,
};

enum class HttpFieldErrorKind : std::uint8_t {
	missing,
	empty,
	invalid,
	out_of_range,
};

struct HttpFieldError {
	HttpFieldErrorKind kind{HttpFieldErrorKind::invalid};
	HttpFieldSource source{HttpFieldSource::query};
	std::string name{};
	std::string value{};
	std::string message{};
};

[[nodiscard]] std::string_view http_field_source_name(
	HttpFieldSource source) noexcept {
	switch (source) {
	case HttpFieldSource::params : return "params";
	case HttpFieldSource::headers: return "headers";
	case HttpFieldSource::query  : return "query";
	case HttpFieldSource::form   : return "form";
	case HttpFieldSource::cookies: return "cookies";
	}
	return "field";
}

[[nodiscard]] inline bool http_field_token_eq_ci(
	std::string_view a,
	std::string_view b) noexcept {
	if (a.size() != b.size()) {
		return false;
	}
	auto fold = [](unsigned char c) noexcept {
		return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
	};
	return std::ranges::equal(a, b, [fold](unsigned char x, unsigned char y) { return fold(x) == fold(y); });
}

[[nodiscard]] inline HttpFieldError http_field_error(
	HttpFieldErrorKind kind,
	HttpFieldSource source,
	std::string_view name,
	std::string_view value,
	std::string_view reason) {
	return HttpFieldError{
		.kind = kind,
		.source = source,
		.name = std::string{name},
		.value = std::string{value},
		.message = std::format("{} field '{}' {}", http_field_source_name(source), name, reason),
	};
}

template<typename>
inline constexpr bool kHttpFieldDependentFalse = false;

template<typename T>
[[nodiscard]] std::expected<T, HttpFieldError> parse_http_field_value(
	std::string_view value,
	HttpFieldSource source,
	std::string_view name) {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::same_as<U, std::string_view>) {
		return value;
	} else if constexpr (std::same_as<U, std::string>) {
		return std::string{value};
	} else if constexpr (std::same_as<U, bool>) {
		if (value.empty()) {
			return std::unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		if (http_field_token_eq_ci(value, "true")
			|| value == "1"
			|| http_field_token_eq_ci(value, "yes")
			|| http_field_token_eq_ci(value, "on")) {
			return true;
		}
		if (http_field_token_eq_ci(value, "false")
			|| value == "0"
			|| http_field_token_eq_ci(value, "no")
			|| http_field_token_eq_ci(value, "off")) {
			return false;
		}
		return std::unexpected{http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not a boolean")};
	} else if constexpr (std::integral<U>) {
		if (value.empty()) {
			return std::unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		U parsed{};
		auto const *first = value.data();
		auto const *last = value.data() + value.size();
		auto const [ptr, ec] = std::from_chars(first, last, parsed);
		if (ec == std::errc::result_out_of_range) {
			return std::unexpected{
				http_field_error(HttpFieldErrorKind::out_of_range, source, name, value, "is out of range")};
		}
		if (ec != std::errc{} || ptr != last) {
			return std::unexpected{
				http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not an integer")};
		}
		return parsed;
	} else if constexpr (std::floating_point<U>) {
		if (value.empty()) {
			return std::unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		U parsed{};
		auto const *first = value.data();
		auto const *last = value.data() + value.size();
		auto const [ptr, ec] = std::from_chars(first, last, parsed, std::chars_format::general);
		if (ec == std::errc::result_out_of_range) {
			return std::unexpected{
				http_field_error(HttpFieldErrorKind::out_of_range, source, name, value, "is out of range")};
		}
		if (ec != std::errc{} || ptr != last || !std::isfinite(parsed)) {
			return std::unexpected{
				http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not a finite number")};
		}
		return parsed;
	} else {
		static_assert(kHttpFieldDependentFalse<U>, "Unsupported HTTP field target type");
	}
}

template<typename Fields, typename T>
[[nodiscard]] std::expected<T, HttpFieldError> http_field_as_impl(
	Fields const &fields,
	HttpFieldSource source,
	std::string_view name) {
	auto value = fields.get(name);
	if (!value) {
		return std::unexpected{http_field_error(HttpFieldErrorKind::missing, source, name, {}, "is missing")};
	}
	return parse_http_field_value<T>(*value, source, name);
}

template<typename Fields, typename T>
[[nodiscard]] std::expected<std::optional<T>, HttpFieldError> http_field_optional_as_impl(
	Fields const &fields,
	HttpFieldSource source,
	std::string_view name) {
	auto value = fields.get(name);
	if (!value) {
		return std::optional<T>{};
	}
	auto parsed = parse_http_field_value<T>(*value, source, name);
	if (!parsed) {
		return std::unexpected{std::move(parsed.error())};
	}
	return std::optional<T>{std::move(*parsed)};
}

template<typename T>
[[nodiscard]] std::expected<T, HttpFieldError> http_field_as(
	HttpFields const &fields,
	HttpFieldSource source,
	std::string_view name) {
	return http_field_as_impl<HttpFields, T>(fields, source, name);
}

template<typename T>
[[nodiscard]] std::expected<T, HttpFieldError> http_field_as(
	HttpFieldsView const &fields,
	HttpFieldSource source,
	std::string_view name) {
	return http_field_as_impl<HttpFieldsView, T>(fields, source, name);
}

template<typename T>
[[nodiscard]] std::expected<std::optional<T>, HttpFieldError> http_field_optional_as(
	HttpFields const &fields,
	HttpFieldSource source,
	std::string_view name) {
	return http_field_optional_as_impl<HttpFields, T>(fields, source, name);
}

template<typename T>
[[nodiscard]] std::expected<std::optional<T>, HttpFieldError> http_field_optional_as(
	HttpFieldsView const &fields,
	HttpFieldSource source,
	std::string_view name) {
	return http_field_optional_as_impl<HttpFieldsView, T>(fields, source, name);
}

} // namespace conflux::http

struct HttpRequestFieldAccessors {
	template<typename Self>
	[[nodiscard]] std::string_view param(
		this Self const &self,
		std::string_view name) noexcept {
		return self.params[name];
	}
	template<typename Self>
	[[nodiscard]] std::string_view header(
		this Self const &self,
		std::string_view name) noexcept {
		return self.headers[name];
	}
	template<typename Self>
	[[nodiscard]] std::string_view query_value(
		this Self const &self,
		std::string_view name) noexcept {
		return self.query[name];
	}
	template<typename Self>
	[[nodiscard]] std::string_view form_value(
		this Self const &self,
		std::string_view name) noexcept {
		return self.form[name];
	}
	template<typename Self>
	[[nodiscard]] std::string_view cookie(
		this Self const &self,
		std::string_view name) noexcept {
		return self.cookies[name];
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<T, conflux::http::HttpFieldError> param_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_as<T>(self.params, conflux::http::HttpFieldSource::params, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<T, conflux::http::HttpFieldError> header_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_as<T>(self.headers, conflux::http::HttpFieldSource::headers, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<T, conflux::http::HttpFieldError> query_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_as<T>(self.query, conflux::http::HttpFieldSource::query, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<T, conflux::http::HttpFieldError> form_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_as<T>(self.form, conflux::http::HttpFieldSource::form, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<T, conflux::http::HttpFieldError> cookie_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_as<T>(self.cookies, conflux::http::HttpFieldSource::cookies, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<std::optional<T>, conflux::http::HttpFieldError> optional_param_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_optional_as<T>(self.params, conflux::http::HttpFieldSource::params, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<std::optional<T>, conflux::http::HttpFieldError> optional_header_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_optional_as<T>(self.headers, conflux::http::HttpFieldSource::headers, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<std::optional<T>, conflux::http::HttpFieldError> optional_query_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_optional_as<T>(self.query, conflux::http::HttpFieldSource::query, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<std::optional<T>, conflux::http::HttpFieldError> optional_form_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_optional_as<T>(self.form, conflux::http::HttpFieldSource::form, name);
	}
	template<typename T, typename Self>
	[[nodiscard]] std::expected<std::optional<T>, conflux::http::HttpFieldError> optional_cookie_as(
		this Self const &self,
		std::string_view name) {
		return conflux::http::http_field_optional_as<T>(self.cookies, conflux::http::HttpFieldSource::cookies, name);
	}
};

export namespace conflux::http {

struct OwnedRequest : ::HttpRequestFieldAccessors {
	std::string method;
	std::string path; // path only, no query std::string
	std::string version;
	std::string remote_addr; // peer IP address (best-effort with multishot accept)
	bool is_tls = false; // true when request arrived over a TLS connection
	::HttpFields params; // {name} captures
	::HttpFields headers = ::HttpFields(true); // case-insensitive lookup
	::HttpFields query; // parsed from URL ?k=v&...
	::HttpFields form; // parsed from application/x-www-form-urlencoded body or multipart text fields
	::HttpFields cookies; // parsed from Cookie: header
	std::vector<conflux::http::UploadedFile> files; // parsed from multipart/form-data body
	std::string body;
	[[nodiscard]] OwnedRequest to_owned() const { return *this; }
};
struct RequestView : ::HttpRequestFieldAccessors {
	std::string_view method;
	std::string_view path;
	std::string_view version;
	std::string_view remote_addr;
	bool is_tls = false;
	::HttpFieldsView params;
	::HttpFieldsView headers;
	::HttpFieldsView query;
	::HttpFieldsView form;
	::HttpFieldsView cookies;
	std::span<conflux::http::UploadedFile const> files;
	std::string_view body;
	RequestView(
		std::string_view method_,
		std::string_view path_,
		std::string_view version_,
		std::string_view remote_addr_,
		bool is_tls_,
		::HttpFieldsView params_,
		::HttpFieldsView headers_,
		::HttpFieldsView query_,
		::HttpFieldsView form_,
		::HttpFieldsView cookies_,
		std::span<conflux::http::UploadedFile const> files_,
		std::string_view body_)
		: method(method_)
		, path(path_)
		, version(version_)
		, remote_addr(remote_addr_)
		, is_tls(is_tls_)
		, params(std::move(params_))
		, headers(std::move(headers_))
		, query(std::move(query_))
		, form(std::move(form_))
		, cookies(std::move(cookies_))
		, files(files_)
		, body(body_) {}
	RequestView(
		OwnedRequest const &req)
		: method(req.method)
		, path(req.path)
		, version(req.version)
		, remote_addr(req.remote_addr)
		, is_tls(req.is_tls)
		, params(req.params)
		, headers(req.headers)
		, query(req.query)
		, form(req.form)
		, cookies(req.cookies)
		, files(req.files)
		, body(req.body) {}
	[[nodiscard]] OwnedRequest to_owned() const {
		OwnedRequest owned;
		owned.method = std::string{method};
		owned.path = std::string{path};
		owned.version = std::string{version};
		owned.remote_addr = std::string{remote_addr};
		owned.is_tls = is_tls;
		owned.params = params.to_owned();
		owned.headers = headers.to_owned();
		owned.query = query.to_owned();
		owned.form = form.to_owned();
		owned.cookies = cookies.to_owned();
		owned.files.reserve(files.size());
		std::ranges::transform(files, std::back_inserter(owned.files), [](auto const &file) {
			return file.to_owned();
		});
		owned.body = std::string{body};
		return owned;
	}
};

using Request = RequestView;

} // namespace conflux::http

export namespace conflux::http {

template<typename>
class CloneableFunction;
template<typename R, typename... Args>
class CloneableFunction<R(Args...)> {
	static constexpr std::size_t kInlineBytes = 32;

	struct Concept {
		virtual ~Concept() = default;
		virtual R invoke(Args... args) = 0;
		[[nodiscard]] virtual std::unique_ptr<Concept> clone() const = 0;
	};
	template<typename F>
	struct FunctionModel : Concept {
		F fn;
		explicit FunctionModel(
			F f)
			: fn(std::move(f)) {}
		R invoke(
			Args... args) override {
			return std::invoke(fn, std::forward<Args>(args)...);
		}
		[[nodiscard]] std::unique_ptr<Concept> clone() const override { return std::make_unique<FunctionModel>(fn); }
	};
	struct InlineConcept : Concept {
		virtual void clone_into(void *dst) const = 0;
		virtual void move_into(void *dst) = 0;
	};
	template<typename F>
	struct InlineModel final : InlineConcept {
		F fn;
		static void *operator new(
			std::size_t,
			void *ptr) noexcept {
			return ptr;
		}
		static void operator delete(
			void *,
			void *) noexcept {}
		static void operator delete(
			void *) noexcept {}
		explicit InlineModel(
			F f)
			: fn(std::move(f)) {}
		R invoke(
			Args... args) override {
			return std::invoke(fn, std::forward<Args>(args)...);
		}
		[[nodiscard]] std::unique_ptr<Concept> clone() const override { return std::make_unique<FunctionModel<F>>(fn); }
		void clone_into(
			void *dst) const override {
			new (dst) InlineModel(fn);
		}
		void move_into(
			void *dst) override {
			new (dst) InlineModel(std::move(fn));
		}
	};
	std::unique_ptr<Concept> fn_{};
	alignas(std::max_align_t) std::byte inline_storage_[kInlineBytes]{};
	InlineConcept *inline_fn_{};

	[[nodiscard]] Concept *target() const noexcept { return inline_fn_ != nullptr ? inline_fn_ : fn_.get(); }

	void reset() noexcept {
		if (inline_fn_ != nullptr) {
			inline_fn_->~Concept();
			inline_fn_ = nullptr;
		}
		fn_.reset();
	}

	void move_from(
		CloneableFunction &&other) {
		if (other.inline_fn_ != nullptr) {
			other.inline_fn_->move_into(inline_storage_);
			inline_fn_ = reinterpret_cast<InlineConcept *>(inline_storage_);
			other.inline_fn_->~Concept();
			other.inline_fn_ = nullptr;
			return;
		}
		fn_ = std::move(other.fn_);
	}

public:
	CloneableFunction() = default;
	CloneableFunction(
		std::nullptr_t) {}
	template<typename F>
		requires(!std::same_as<std::remove_cvref_t<F>, CloneableFunction>)
	CloneableFunction(
		F &&f) {
		using ModelT = FunctionModel<std::remove_cvref_t<F>>;
		using InlineModelT = InlineModel<std::remove_cvref_t<F>>;
		if constexpr (sizeof(InlineModelT) <= kInlineBytes && alignof(InlineModelT) <= alignof(std::max_align_t)) {
			new (inline_storage_) InlineModelT(std::forward<F>(f));
			inline_fn_ = reinterpret_cast<InlineConcept *>(inline_storage_);
		} else {
			fn_ = std::make_unique<ModelT>(std::forward<F>(f));
		}
	}
	CloneableFunction(
		CloneableFunction const &o)
		: fn_(o.fn_ ? o.fn_->clone() : nullptr) {
		if (o.inline_fn_ != nullptr) {
			o.inline_fn_->clone_into(inline_storage_);
			inline_fn_ = reinterpret_cast<InlineConcept *>(inline_storage_);
		}
	}
	CloneableFunction &operator =(
		CloneableFunction const &o) {
		if (this != &o) {
			reset();
			fn_ = o.fn_ ? o.fn_->clone() : nullptr;
			if (o.inline_fn_ != nullptr) {
				o.inline_fn_->clone_into(inline_storage_);
				inline_fn_ = reinterpret_cast<InlineConcept *>(inline_storage_);
			}
		}
		return *this;
	}
	CloneableFunction(
		CloneableFunction &&o) {
		move_from(std::move(o));
	}
	CloneableFunction &operator =(
		CloneableFunction &&o) {
		if (this != &o) {
			reset();
			move_from(std::move(o));
		}
		return *this;
	}
	~CloneableFunction() { reset(); }
	[[nodiscard]] explicit operator bool() const noexcept { return target() != nullptr; }
	R operator ()(
		Args... args) const {
		return target()->invoke(std::forward<Args>(args)...);
	}
};

} // namespace conflux::http
