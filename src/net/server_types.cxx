module;

export module conflux.net.http.server_types;

import std;
import conflux.types;
import conflux.net.http.types;

// ---------------------------------------------------------------------------
// HTTP server run/telemetry types shared across the primary module and its
// partitions.
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size)
export enum class RunStatus : u8 {
	stopped_normally,
	fatal_cq_overflow,
	fatal_cq_overflow_no_nodrop,
	fatal_submit_wait_ebadr,
	fatal_internal_exception,
};

export struct SendZcMetrics {
	u64 attempts{};
	u64 plain_attempts{};
	u64 mapped_attempts{};
	u64 bytes_requested{};
	u64 bytes_sent{};
	u64 notifications{};
	u64 copied_notifications{};
	u64 sends_without_notification{};
	u64 errors_enomem{};
	u64 errors_other{};
	u64 fallback_regular_send{};
	u64 tls_bypass{};
	u64 tls_bypass_bytes{};
	u64 adaptive_disable_count{};
};

export enum class SendZcPendingAction : u8 {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
};

export enum class SendZcCqeAction : u8 {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
	close_after_notification,
};

export struct SendZcCqeState {
	bool waiting_notification{};
	bool close_after_notification{};
	SendZcPendingAction after_notification{SendZcPendingAction::none};
};

export struct SendZcCqeInput {
	int result{};
	bool notification{};
	bool more{};
	bool copied{};
	bool enomem_error{};
	SZ written_before{};
	SZ response_total{};
};

export struct SendZcCqeOutcome {
	SendZcCqeAction action{SendZcCqeAction::none};
	SZ bytes_sent{};
	bool adaptive_disabled{};
};

export [[nodiscard]] SendZcCqeOutcome observe_send_zc_cqe(
	SendZcCqeState &state,
	SendZcMetrics &metrics,
	SendZcCqeInput input,
	bool &send_zc_enabled) noexcept {
	SendZcCqeOutcome out{};
	if (input.notification) {
		++metrics.notifications;
		if (input.copied) {
			++metrics.copied_notifications;
			if (send_zc_enabled
				&& metrics.attempts >= 1024
				&& metrics.bytes_requested >= SZ{16} * 1024 * 1024
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
		default: break;
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
		out.bytes_sent = static_cast<SZ>(input.result);
		metrics.bytes_sent += out.bytes_sent;
		state.after_notification = input.written_before + out.bytes_sent >= input.response_total
			? SendZcPendingAction::complete_response
			: SendZcPendingAction::resubmit_response;
		return out;
	}

	++metrics.sends_without_notification;
	if (input.result < 0) {
		note_error();
		out.action = SendZcCqeAction::close_after_error;
		return out;
	}
	out.bytes_sent = static_cast<SZ>(input.result);
	metrics.bytes_sent += out.bytes_sent;
	out.action = input.written_before + out.bytes_sent < input.response_total
		? SendZcCqeAction::resubmit_response
		: SendZcCqeAction::complete_response;
	return out;
}

export struct HttpServerMetrics {
	u64 sq_dropped{};
	u64 cq_overflow{};
	u64 accepted_direct_failures{};
	u64 zc_notifications_pending{};
	u64 zc_capable_rings{};
	u64 zc_enabled_rings{};
	u64 recv_bundle_cqes{};
	u64 recv_bundle_slices{};
	u64 recv_bundle_bytes{};
	SendZcMetrics send_zc{};
};

export struct UploadedFile {
	SV name;
	SV filename;
	SV content_type;
	SV data;
	S owned_name;
	S owned_filename;
	S owned_content_type;
	S owned_data;
	bool owns_metadata = false;
	bool owns_data = false;

	UploadedFile() = default;
	UploadedFile(
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_)
		: name(name_)
		, filename(filename_)
		, content_type(content_type_)
		, data(data_) {}
	UploadedFile(
		UploadedFile const &other)
		: owned_name(other.owns_metadata ? other.owned_name : S{})
		, owned_filename(other.owns_metadata ? other.owned_filename : S{})
		, owned_content_type(other.owns_metadata ? other.owned_content_type : S{})
		, owned_data(other.owns_data ? other.owned_data : S{})
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
	}
	UploadedFile(
		UploadedFile &&other) noexcept
		: name(other.name)
		, filename(other.filename)
		, content_type(other.content_type)
		, data(other.data)
		, owned_name(move(other.owned_name))
		, owned_filename(move(other.owned_filename))
		, owned_content_type(move(other.owned_content_type))
		, owned_data(move(other.owned_data))
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
		owned_name = other.owns_metadata ? other.owned_name : S{};
		owned_filename = other.owns_metadata ? other.owned_filename : S{};
		owned_content_type = other.owns_metadata ? other.owned_content_type : S{};
		owned_data = other.owns_data ? other.owned_data : S{};
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
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
		owned_name = move(other.owned_name);
		owned_filename = move(other.owned_filename);
		owned_content_type = move(other.owned_content_type);
		owned_data = move(other.owned_data);
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
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_) {
		return UploadedFile{name_, filename_, content_type_, data_};
	}
	[[nodiscard]] static UploadedFile owned(
		S name_,
		S filename_,
		S content_type_,
		S data_) {
		UploadedFile file{SV{}, SV{}, SV{}, SV{}};
		file.owned_name = move(name_);
		file.owned_filename = move(filename_);
		file.owned_content_type = move(content_type_);
		file.owned_data = move(data_);
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
		return UploadedFile::owned(S{name}, S{filename}, S{content_type}, S{data});
	}
};

export enum class HttpFieldSource : u8 {
	params,
	headers,
	query,
	form,
	cookies,
};

export enum class HttpFieldErrorKind : u8 {
	missing,
	empty,
	invalid,
	out_of_range,
};

export struct HttpFieldError {
	HttpFieldErrorKind kind{HttpFieldErrorKind::invalid};
	HttpFieldSource source{HttpFieldSource::query};
	S name{};
	S value{};
	S message{};
};

export [[nodiscard]] SV http_field_source_name(
	HttpFieldSource source) noexcept {
	switch (source) {
	case HttpFieldSource::params: return "params";
	case HttpFieldSource::headers: return "headers";
	case HttpFieldSource::query: return "query";
	case HttpFieldSource::form: return "form";
	case HttpFieldSource::cookies: return "cookies";
	}
	return "field";
}

[[nodiscard]] inline bool http_field_token_eq_ci(
	SV a,
	SV b) noexcept {
	if (a.size() != b.size()) {
		return false;
	}
	auto fold = [](unsigned char c) noexcept {
		return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
	};
	return ranges::equal(a, b, [fold](unsigned char x, unsigned char y) {
		return fold(x) == fold(y);
	});
}

[[nodiscard]] inline HttpFieldError http_field_error(
	HttpFieldErrorKind kind,
	HttpFieldSource source,
	SV name,
	SV value,
	SV reason) {
	return HttpFieldError{
		.kind = kind,
		.source = source,
		.name = S{name},
		.value = S{value},
		.message = format("{} field '{}' {}", http_field_source_name(source), name, reason),
	};
}

export template<typename>
inline constexpr bool kHttpFieldDependentFalse = false;

export template<typename T>
[[nodiscard]] expected<T, HttpFieldError> parse_http_field_value(
	SV value,
	HttpFieldSource source,
	SV name) {
	using U = std::remove_cvref_t<T>;
	if constexpr (same_as<U, SV>) {
		return value;
	} else if constexpr (same_as<U, S>) {
		return S{value};
	} else if constexpr (same_as<U, bool>) {
		if (value.empty()) {
			return unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		if (http_field_token_eq_ci(value, "true") || value == "1" || http_field_token_eq_ci(value, "yes")
			|| http_field_token_eq_ci(value, "on")) {
			return true;
		}
		if (http_field_token_eq_ci(value, "false") || value == "0" || http_field_token_eq_ci(value, "no")
			|| http_field_token_eq_ci(value, "off")) {
			return false;
		}
		return unexpected{http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not a boolean")};
	} else if constexpr (std::integral<U>) {
		if (value.empty()) {
			return unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		U parsed{};
		auto const *first = value.data();
		auto const *last = value.data() + value.size();
		auto const [ptr, ec] = from_chars(first, last, parsed);
		if (ec == errc::result_out_of_range) {
			return unexpected{http_field_error(HttpFieldErrorKind::out_of_range, source, name, value, "is out of range")};
		}
		if (ec != errc{} || ptr != last) {
			return unexpected{http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not an integer")};
		}
		return parsed;
	} else if constexpr (std::floating_point<U>) {
		if (value.empty()) {
			return unexpected{http_field_error(HttpFieldErrorKind::empty, source, name, value, "is empty")};
		}
		U parsed{};
		auto const *first = value.data();
		auto const *last = value.data() + value.size();
		auto const [ptr, ec] = from_chars(first, last, parsed, std::chars_format::general);
		if (ec == errc::result_out_of_range) {
			return unexpected{http_field_error(HttpFieldErrorKind::out_of_range, source, name, value, "is out of range")};
		}
		if (ec != errc{} || ptr != last || !isfinite(parsed)) {
			return unexpected{http_field_error(HttpFieldErrorKind::invalid, source, name, value, "is not a finite number")};
		}
		return parsed;
	} else {
		static_assert(kHttpFieldDependentFalse<U>, "Unsupported HTTP field target type");
	}
}

template<typename Fields, typename T>
[[nodiscard]] expected<T, HttpFieldError> http_field_as_impl(
	Fields const &fields,
	HttpFieldSource source,
	SV name) {
	auto value = fields.get(name);
	if (!value) {
		return unexpected{http_field_error(HttpFieldErrorKind::missing, source, name, {}, "is missing")};
	}
	return parse_http_field_value<T>(*value, source, name);
}

template<typename Fields, typename T>
[[nodiscard]] expected<Opt<T>, HttpFieldError> http_field_optional_as_impl(
	Fields const &fields,
	HttpFieldSource source,
	SV name) {
	auto value = fields.get(name);
	if (!value) {
		return Opt<T>{};
	}
	auto parsed = parse_http_field_value<T>(*value, source, name);
	if (!parsed) {
		return unexpected{move(parsed.error())};
	}
	return Opt<T>{move(*parsed)};
}

export template<typename T>
[[nodiscard]] expected<T, HttpFieldError> http_field_as(
	HttpFields const &fields,
	HttpFieldSource source,
	SV name) {
	return http_field_as_impl<HttpFields, T>(fields, source, name);
}

export template<typename T>
[[nodiscard]] expected<T, HttpFieldError> http_field_as(
	HttpFieldsView const &fields,
	HttpFieldSource source,
	SV name) {
	return http_field_as_impl<HttpFieldsView, T>(fields, source, name);
}

export template<typename T>
[[nodiscard]] expected<Opt<T>, HttpFieldError> http_field_optional_as(
	HttpFields const &fields,
	HttpFieldSource source,
	SV name) {
	return http_field_optional_as_impl<HttpFields, T>(fields, source, name);
}

export template<typename T>
[[nodiscard]] expected<Opt<T>, HttpFieldError> http_field_optional_as(
	HttpFieldsView const &fields,
	HttpFieldSource source,
	SV name) {
	return http_field_optional_as_impl<HttpFieldsView, T>(fields, source, name);
}

export struct HttpRequest {
	S method;
	S path; // path only, no query S
	S version;
	S remote_addr; // peer IP address (best-effort with multishot accept)
	bool is_tls = false; // true when request arrived over a TLS connection
	HttpFields params; // {name} captures
	HttpFields headers = HttpFields(true); // case-insensitive lookup
	HttpFields query; // parsed from URL ?k=v&...
	HttpFields form; // parsed from application/x-www-form-urlencoded body or multipart text fields
	HttpFields cookies; // parsed from Cookie: header
	V<UploadedFile> files; // parsed from multipart/form-data body
	S body;
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> param_as(
		SV name) const {
		return http_field_as<T>(params, HttpFieldSource::params, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> header_as(
		SV name) const {
		return http_field_as<T>(headers, HttpFieldSource::headers, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> query_as(
		SV name) const {
		return http_field_as<T>(query, HttpFieldSource::query, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> form_as(
		SV name) const {
		return http_field_as<T>(form, HttpFieldSource::form, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> cookie_as(
		SV name) const {
		return http_field_as<T>(cookies, HttpFieldSource::cookies, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_param_as(
		SV name) const {
		return http_field_optional_as<T>(params, HttpFieldSource::params, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_header_as(
		SV name) const {
		return http_field_optional_as<T>(headers, HttpFieldSource::headers, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_query_as(
		SV name) const {
		return http_field_optional_as<T>(query, HttpFieldSource::query, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_form_as(
		SV name) const {
		return http_field_optional_as<T>(form, HttpFieldSource::form, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_cookie_as(
		SV name) const {
		return http_field_optional_as<T>(cookies, HttpFieldSource::cookies, name);
	}
	[[nodiscard]] HttpRequest to_owned() const { return *this; }
};
export struct HttpRequestView {
	SV method;
	SV path;
	SV version;
	SV remote_addr;
	bool is_tls = false;
	HttpFieldsView params;
	HttpFieldsView headers;
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	span<UploadedFile const> files;
	SV body;
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> param_as(
		SV name) const {
		return http_field_as<T>(params, HttpFieldSource::params, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> header_as(
		SV name) const {
		return http_field_as<T>(headers, HttpFieldSource::headers, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> query_as(
		SV name) const {
		return http_field_as<T>(query, HttpFieldSource::query, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> form_as(
		SV name) const {
		return http_field_as<T>(form, HttpFieldSource::form, name);
	}
	template<typename T>
	[[nodiscard]] expected<T, HttpFieldError> cookie_as(
		SV name) const {
		return http_field_as<T>(cookies, HttpFieldSource::cookies, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_param_as(
		SV name) const {
		return http_field_optional_as<T>(params, HttpFieldSource::params, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_header_as(
		SV name) const {
		return http_field_optional_as<T>(headers, HttpFieldSource::headers, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_query_as(
		SV name) const {
		return http_field_optional_as<T>(query, HttpFieldSource::query, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_form_as(
		SV name) const {
		return http_field_optional_as<T>(form, HttpFieldSource::form, name);
	}
	template<typename T>
	[[nodiscard]] expected<Opt<T>, HttpFieldError> optional_cookie_as(
		SV name) const {
		return http_field_optional_as<T>(cookies, HttpFieldSource::cookies, name);
	}
	HttpRequestView(
		SV method_,
		SV path_,
		SV version_,
		SV remote_addr_,
		bool is_tls_,
		HttpFieldsView params_,
		HttpFieldsView headers_,
		HttpFieldsView query_,
		HttpFieldsView form_,
		HttpFieldsView cookies_,
		span<UploadedFile const> files_,
		SV body_)
		: method(method_)
		, path(path_)
		, version(version_)
		, remote_addr(remote_addr_)
		, is_tls(is_tls_)
		, params(move(params_))
		, headers(move(headers_))
		, query(move(query_))
		, form(move(form_))
		, cookies(move(cookies_))
		, files(files_)
		, body(body_) {}
	HttpRequestView(
		HttpRequest const &req)
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
	[[nodiscard]] HttpRequest to_owned() const {
		HttpRequest owned;
		owned.method = S{method};
		owned.path = S{path};
		owned.version = S{version};
		owned.remote_addr = S{remote_addr};
		owned.is_tls = is_tls;
		owned.params = params.to_owned();
		owned.headers = headers.to_owned();
		owned.query = query.to_owned();
		owned.form = form.to_owned();
		owned.cookies = cookies.to_owned();
		owned.files.reserve(files.size());
		for (auto const &file: files) {
			owned.files.push_back(file.to_owned());
		}
		owned.body = S{body};
		return owned;
	}
};

export template<typename>
class CloneableFunction;
template<typename R, typename... Args>
class CloneableFunction<R(Args...)> {
	struct Concept {
		virtual ~Concept() = default;
		virtual R invoke(Args... args) = 0;
		[[nodiscard]] virtual UP<Concept> clone() const = 0;
	};
	template<typename F>
	struct Model final : Concept {
		F fn;
		explicit Model(
			F f)
			: fn(move(f)) {}
		R invoke(
			Args... args) override {
			return std::invoke(fn, forward<Args>(args)...);
		}
		[[nodiscard]] UP<Concept> clone() const override { return make_unique<Model>(fn); }
	};
	UP<Concept> fn_{};

public:
	CloneableFunction() = default;
	CloneableFunction(
		std::nullptr_t) {}
	template<typename F>
		requires(!same_as<std::remove_cvref_t<F>, CloneableFunction>)
	CloneableFunction(
		F &&f)
		: fn_(make_unique<Model<std::remove_cvref_t<F>>>(forward<F>(f))) {}
	CloneableFunction(
		CloneableFunction const &o)
		: fn_(o.fn_ ? o.fn_->clone() : nullptr) {}
	CloneableFunction &operator =(
		CloneableFunction const &o) {
		if (this != &o) {
			fn_ = o.fn_ ? o.fn_->clone() : nullptr;
		}
		return *this;
	}
	CloneableFunction(CloneableFunction &&) noexcept = default;
	CloneableFunction &operator =(CloneableFunction &&) noexcept = default;
	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(fn_); }
	R operator ()(
		Args... args) const {
		return fn_->invoke(forward<Args>(args)...);
	}
};
