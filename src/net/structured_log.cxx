// Structured access logging middleware: emits one JSON-line per request.
// Format: {"ts":"ISO8601","method":"GET","path":"/...","status":200,
//          "bytes":1234,"elapsed_ms":12,"remote":""[,"app":"name"]}
// Optional daily log rotation: the file is reopened whenever the UTC date changes.
module;
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

export module conflux.net.structured_log;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
export struct StructuredLogOptions {
	// Path to the log file. Empty = write to stderr.
	std::string log_file;
	// Rotate to a new file each UTC day. Only meaningful when log_file is set.
	// The suffix ".YYYY-MM-DD" is appended to log_file before opening.
	bool daily_rotate{false};
	// Optional application name added as "app" field to every line.
	std::string app_name;
};
namespace structured_log_detail {

std::string json_escape(
	std::string_view s) {
	std::string out;
	out.reserve(s.size() + 4);
	for (char const raw: s) {
		auto c = static_cast<unsigned char>(raw);
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else if (c == '\t') {
			out += "\\t";
		} else if (c < 0x20) {
			out += std::format("\\u{:04x}", c);
		} else {
			out += static_cast<char>(c);
		}
	}
	return out;
}

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
			fd_ = STDERR_FILENO;
		}
	}
	~LogSink() {
		if (fd_ >= 0 && fd_ != STDERR_FILENO) {
			::close(fd_);
		}
	}
	LogSink(LogSink const &) = delete;
	LogSink &operator =(LogSink const &) = delete;
	void write(
		std::string const &line) {
		std::scoped_lock const lk{mtx_};
		maybe_rotate();
		if (fd_ < 0) {
			return;
		}
		std::string l = line + '\n';
		std::size_t written = 0;
		while (written < l.size()) {
			ssize_t const n = ::write(fd_, l.data() + written, l.size() - written);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			written += static_cast<std::size_t>(n);
		}
	}

private:
	void maybe_rotate() {
		if (path_.empty()) {
			return;
		}
		auto now = std::chrono::system_clock::now();
		auto tt = std::chrono::system_clock::to_time_t(now);
		tm tm_val{};
		::gmtime_r(&tt, &tm_val);
		int const today = ((tm_val.tm_year + 1900) * 10000) + ((tm_val.tm_mon + 1) * 100) + tm_val.tm_mday;
		if (today == current_day_ && fd_ >= 0) {
			return;
		}

		if (fd_ >= 0 && fd_ != STDERR_FILENO) {
			::close(fd_);
			fd_ = -1;
		}

		std::string fpath = path_;
		if (daily_rotate_) {
			fpath += std::format(".{:04d}-{:02d}-{:02d}", tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
		}
		fd_ = ::open(
			fpath.c_str(),
			O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
			0644); // NOLINT(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
		if (fd_ < 0) {
			auto msg = std::format("structured_log: open '{}' failed: {}\n", fpath, strerror(errno));
			[[maybe_unused]] auto _ = ::write(STDERR_FILENO, msg.data(), msg.size());
		}
		current_day_ = today;
	}
	std::string path_;
	bool daily_rotate_;
	std::mutex mtx_;
	int fd_{-1};
	int current_day_{-1};
};
export Router::Middleware structured_log_middleware(
	StructuredLogOptions opts = {}) {
	std::string app_name = std::move(opts.app_name);
	auto sink = std::make_shared<LogSink>(std::move(opts.log_file), opts.daily_rotate);

	return [sink, app_name = std::move(app_name)](RequestView const &req, Router::Handler const &next) -> Response {
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

		auto line = std::format(
			R"({{"ts":"{}","method":"{}","path":"{}","status":{},"bytes":{},"elapsed_ms":{},"remote":"{}"{}}})",
			ts,
			structured_log_detail::json_escape(req.method),
			structured_log_detail::json_escape(req.path),
			resp.status,
			resp.content_length(),
			ms,
			structured_log_detail::json_escape(req.remote_addr),
			app_name.empty() ? std::string{} :
							   std::format(R"(,"app":"{}")", structured_log_detail::json_escape(app_name)));
		sink->write(line);
		return resp;
	};
}
