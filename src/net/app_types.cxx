module;

export module conflux.net.app.types;

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.server_types;
#if CONFLUX_HAS_JSON
import conflux.json;
import conflux.json.boundary;
#endif

export namespace conflux::http {

class ExtractorFailure final : public std::exception {
public:
	explicit ExtractorFailure(
		Response response)
		: response_(std::move(response)) {}

	[[nodiscard]] char const *what() const noexcept override { return "HTTP extractor failure"; }
	[[nodiscard]] Response response() && { return std::move(response_); }

private:
	Response response_;
};

struct AppRunOptions {
	std::uint16_t port = kConfigDefaultPort;
};

struct AppRateLimitOptions {
	unsigned requests{100};
	std::chrono::seconds window{60};
	unsigned burst{0};
	std::size_t max_clients{65536};
};

#if CONFLUX_HAS_JSON
struct AppJsonOptions {
	conflux::json::boundary::DecodeOptions decode{};
	conflux::json::boundary::DumpOptions dump{};
	std::size_t max_body_size{};
};
#endif

struct Problem {
	Response response;
	std::string code;
	std::string detail;
};

struct Created {
	Response response;

	[[nodiscard]] Created header(
		std::string_view name,
		std::string value) && {
		response.headers[name] = std::move(value);
		return std::move(*this);
	}

	[[nodiscard]] Created header(
		std::string_view name,
		std::string_view value) && {
		response.headers[name] = std::string{value};
		return std::move(*this);
	}

	[[nodiscard]] Created header(
		std::string_view name,
		char const *value) && {
		response.headers[name] = std::string{value};
		return std::move(*this);
	}
};

template<class T>
struct Json {
	using value_type = std::remove_cvref_t<T>;

	value_type value;

	constexpr Json(
		value_type v)
		: value(std::move(v)) {}

	[[nodiscard]] constexpr value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const *operator ->() const noexcept { return std::addressof(value); }
};

template<class T>
Json(T) -> Json<std::remove_cvref_t<T>>;

template<std::size_t N>
struct FixedString {
	char value[N]{};

	consteval FixedString(
		char const (&text)[N]) {
		std::copy_n(text, N, value);
	}

	[[nodiscard]] constexpr std::string_view view() const noexcept { return {value, N - 1}; }
};

template<FixedString Name, class T = std::string_view>
struct Path {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<std::size_t Index, class T = std::string_view>
struct PathAt {
	using value_type = T;
	static constexpr std::size_t index = Index;

	T value{};

	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Query {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Header {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Cookie {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Form {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

#if CONFLUX_HAS_JSON
template<class T>
struct QueryParams {
	using value_type = std::remove_cvref_t<T>;

	value_type value;

	[[nodiscard]] constexpr value_type const &get() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const *operator ->() const noexcept { return std::addressof(value); }
};

template<class T>
struct FormParams {
	using value_type = std::remove_cvref_t<T>;

	value_type value;

	[[nodiscard]] constexpr value_type const &get() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const *operator ->() const noexcept { return std::addressof(value); }
};
#endif

struct BodyText {
	std::string_view value{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return value; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return value; }
};

struct BodyBytes {
	std::span<std::byte const> value{};

	[[nodiscard]] constexpr std::span<std::byte const> get() const noexcept { return value; }
	[[nodiscard]] constexpr std::span<std::byte const> operator *() const noexcept { return value; }
	[[nodiscard]] std::string_view text_view() const noexcept {
		if (value.empty()) {
			return {};
		}
		return {reinterpret_cast<char const *>(value.data()), value.size()};
	}
};

struct OwnedBodyBytes {
	std::string value;

	[[nodiscard]] std::string const &get() const noexcept { return value; }
	[[nodiscard]] std::string const &operator *() const noexcept { return value; }
};

#if CONFLUX_HAS_JSON
struct JsonDocument {
	using value_type = Document;

	value_type value;

	[[nodiscard]] value_type const &get() const noexcept { return value; }
	[[nodiscard]] value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] value_type const *operator ->() const noexcept { return std::addressof(value); }
};

struct JsonPatch {
	using value_type = Document;

	value_type value;

	[[nodiscard]] value_type const &get() const noexcept { return value; }
	[[nodiscard]] value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] value_type const *operator ->() const noexcept { return std::addressof(value); }
};

struct MergePatch {
	using value_type = Document;

	value_type value;

	[[nodiscard]] value_type const &get() const noexcept { return value; }
	[[nodiscard]] value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] value_type const *operator ->() const noexcept { return std::addressof(value); }
};
#endif

struct Multipart {
	HttpFieldsView form;
	std::span<UploadedFile const> files;

	[[nodiscard]] std::string_view form_value(
		std::string_view name) const {
		return form[name];
	}

	[[nodiscard]] UploadedFile const *file(
		std::string_view name) const noexcept {
		for (auto const &item: files) {
			if (item.name == name) {
				return std::addressof(item);
			}
		}
		return nullptr;
	}
};

struct RequestId {
	std::string_view value{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return value; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return value; }
};

struct ConnectionInfo {
	std::string_view remote_addr{};
	bool is_tls{};
};

struct TraceContext {
	std::string_view traceparent{};
};

struct Bearer {
	std::string_view token{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return token; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return token; }
};

struct BasicAuth {
	std::string username;
	std::string password;
};

template<class T>
struct State {
	T *value{};

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return value != nullptr; }
	[[nodiscard]] constexpr T *try_get() const noexcept { return value; }
	[[nodiscard]] constexpr T &get() const noexcept { return *value; }
	[[nodiscard]] constexpr T &operator *() const noexcept { return *value; }
	[[nodiscard]] constexpr T *operator ->() const noexcept { return value; }
};

struct ValidationIssue {
	std::string code;
	std::string message;
	std::string method;
	std::string path;
	std::string source_file;
	std::uint_least32_t source_line{};
	std::string related_source_file;
	std::uint_least32_t related_source_line{};
};

struct ValidationReport {
	std::vector<ValidationIssue> issues;
	std::vector<ConfigIssue> config_issues;
	std::vector<conflux::runtime::CapabilityIssue> capability_issues;
	bool capability_issues_block_startup = false;

	[[nodiscard]] bool ok() const noexcept {
		return issues.empty() && config_issues.empty() && !capability_issues_block_startup;
	}
	explicit operator bool() const noexcept { return ok(); }
	[[nodiscard]] std::string summary() const {
		if (issues.empty() && config_issues.empty() && capability_issues.empty()) {
			return {};
		}
		std::string out;
		auto append = [&](std::string text) {
			if (!out.empty()) {
				out += '\n';
			}
			out += std::move(text);
		};
		for (auto const &issue: issues) {
			auto const code = issue.code.empty() ? "app.validation" : issue.code;
			append(std::format("{} {} [{}]: {}", issue.method, issue.path, code, issue.message));
		}
		for (auto const &issue: config_issues) {
			append(config_issue_summary(issue));
		}
		for (auto const &issue: capability_issues) {
			append(std::format("capability.{}: {}", issue.feature, issue.message));
		}
		return out;
	}
	[[nodiscard]] std::string detailed_summary() const {
		if (issues.empty() && config_issues.empty() && capability_issues.empty()) {
			return {};
		}
		std::string out;
		auto append = [&](std::string text) {
			if (!out.empty()) {
				out += '\n';
			}
			out += std::move(text);
		};
		for (auto const &issue: issues) {
			auto const code = issue.code.empty() ? "app.validation" : issue.code;
			std::string line = std::format("{} {} [{}]: {}", issue.method, issue.path, code, issue.message);
			if (!issue.source_file.empty()) {
				line += std::format(" at {}:{}", issue.source_file, issue.source_line);
			}
			if (!issue.related_source_file.empty()) {
				line += std::format(" related {}:{}", issue.related_source_file, issue.related_source_line);
			}
			append(std::move(line));
		}
		for (auto const &issue: config_issues) {
			append(config_issue_summary(issue));
		}
		for (auto const &issue: capability_issues) {
			append(std::format("capability.{}: {} {}", issue.feature, issue.message, issue.hint));
		}
		return out;
	}
};

struct AppRouteInfo {
	std::string method;
	std::string path;
	std::string name;
	std::string handler_kind;
	std::string source_file;
	std::uint_least32_t source_line{};
	std::vector<std::string> extractors;
	std::vector<std::string> path_params;
	std::map<std::string, std::string> path_param_types;
	std::size_t required_state_count{};
	std::vector<std::string> consumes;
	std::vector<std::string> produces;
	std::string request_body_schema;
	std::string response_schema;
	int success_status{kHttpOk};
	bool problem_response{};
	std::size_t max_body_size{};
	std::chrono::milliseconds timeout{};
	std::size_t middleware_count{};
	std::string rate_limit;
	std::string auth_policy;
	std::string openapi_summary;
	bool allow_get_body{};
};

struct AppStaticMountInfo {
	std::string url_prefix;
	std::string root_dir;
	std::string source_file;
	std::uint_least32_t source_line{};
};

} // namespace conflux::http
