// Structured access logging middleware: emits one JSON-line per request.
// Format: {"ts":"ISO8601","method":"GET","path":"/...","status":200,
//          "bytes":1234,"elapsed_ms":12,"remote":""[,"app":"name"]}
// Optional daily log rotation: the file is reopened whenever the UTC date changes.
module;
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

export module conflux.net.structured_log;
import std;
import conflux.types;
import conflux.file_io_sync;
import conflux.net.http.types;
import conflux.net.http.json_string;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;
export namespace conflux::http {

struct StructuredLogOptions {
	// Path to the log file. Empty = write to stderr.
	std::string log_file;
	// Rotate to a new file each UTC day. Only meaningful when log_file is set.
	// The suffix ".YYYY-MM-DD" is appended to log_file before opening.
	bool daily_rotate{false};
	// Optional application name added as "app" field to every line.
	std::string app_name;
};
namespace structured_log_detail {

} // namespace structured_log_detail
// LogSink at module scope (not anonymous namespace) so std::make_shared<LogSink> works.
class LogSink {
public:
	explicit LogSink(
		std::string path,
		bool daily_rotate)
		: path_(std::move(path))
		, daily_rotate_(daily_rotate) {
		if (path_.empty()) {
			stderr_ = true;
		}
	}
	LogSink(LogSink const &) = delete;
	LogSink &operator =(LogSink const &) = delete;
	void write(
		std::string const &line) {
		std::scoped_lock const lk{mtx_};
		maybe_rotate();
		int const fd = current_fd();
		if (fd < 0) {
			return;
		}
		std::string l = line + '\n';
		auto bytes = std::as_bytes(std::span{l});
		[[maybe_unused]] auto _ = conflux::file_io_sync::blocking_write_all_fd(fd, bytes);
	}

private:
	[[nodiscard]] int current_fd() const noexcept { return stderr_ ? STDERR_FILENO : file_.fd(); }
	void maybe_rotate() {
		if (path_.empty()) {
			return;
		}
		auto now = std::chrono::system_clock::now();
		auto tt = std::chrono::system_clock::to_time_t(now);
		tm tm_val{};
		::gmtime_r(&tt, &tm_val);
		int const today = ((tm_val.tm_year + 1900) * 10000) + ((tm_val.tm_mon + 1) * 100) + tm_val.tm_mday;
		if (today == current_day_ && file_) {
			return;
		}

		file_.reset();

		std::string fpath = path_;
		if (daily_rotate_) {
			fpath += std::format(".{:04d}-{:02d}-{:02d}", tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
		}
		auto file = conflux::file_io_sync::blocking_open_file(fpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (!file) {
			auto msg = std::format("structured_log: open '{}' failed: {}\n", fpath, file.error().what());
			[[maybe_unused]] auto _ =
				conflux::file_io_sync::blocking_write_all_fd(STDERR_FILENO, std::as_bytes(std::span{msg}));
		} else {
			file_ = std::move(*file);
		}
		current_day_ = today;
	}
	std::string path_;
	bool daily_rotate_;
	std::mutex mtx_;
	bool stderr_{false};
	conflux::file_io_sync::UniqueFd file_{};
	int current_day_{-1};
};
Router::Middleware structured_log_middleware(
	StructuredLogOptions opts = {}) {
	std::string app_name = std::move(opts.app_name);
	auto sink = std::make_shared<LogSink>(std::move(opts.log_file), opts.daily_rotate);

	return [sink, app_name = std::move(app_name)](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto t0 = std::chrono::steady_clock::now();
		auto resp = next(req);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

		auto now = std::chrono::system_clock::now();
		auto tt = std::chrono::system_clock::to_time_t(now);
		tm tm_val{};
		::gmtime_r(&tt, &tm_val);
		auto ts = std::format(
			"{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
			tm_val.tm_year + 1900,
			tm_val.tm_mon + 1,
			tm_val.tm_mday,
			tm_val.tm_hour,
			tm_val.tm_min,
			tm_val.tm_sec);

		std::string line;
		line.reserve(128 + ts.size() + req.method.size() + req.path.size() + req.remote_addr.size() + app_name.size());
		line += R"({"ts":)";
		conflux::http::detail::append_json_string(line, ts);
		line += R"(,"method":)";
		conflux::http::detail::append_json_string(line, req.method);
		line += R"(,"path":)";
		conflux::http::detail::append_json_string(line, req.path);
		line += R"(,"status":)";
		conflux::http::detail::append_decimal(line, resp.status);
		line += R"(,"bytes":)";
		conflux::http::detail::append_decimal(line, resp.content_length());
		line += R"(,"elapsed_ms":)";
		conflux::http::detail::append_decimal(line, ms);
		line += R"(,"remote":)";
		conflux::http::detail::append_json_string(line, req.remote_addr);
		if (!app_name.empty()) {
			line += R"(,"app":)";
			conflux::http::detail::append_json_string(line, app_name);
		}
		line += '}';
		sink->write(line);
		return resp;
	};
}

} // namespace conflux::http
