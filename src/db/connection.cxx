module;
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <libpq-fe.h>
#include <memory>
#include <poll.h>

export module conflux.pg.connection;

import std;
import std.compat;
import conflux.types;
import conflux.work;
import conflux.uring.timeout;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.file_io;
import conflux.file_io_sync;
import conflux.pg.types;
import conflux.pg.params;
import conflux.pg.result;

namespace conflux::pg {
namespace root = conflux::work::root;
namespace detail {

struct ConnectState;
inline void rstrip_nl_(
	std::string_view &s) noexcept {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
		s.remove_suffix(1);
	}
}
inline void rstrip_nl_(
	std::string &s) noexcept {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
		s.pop_back();
	}
}
inline PgError from_conn(
	PGconn *c,
	std::string_view label) {
	char const *raw = c != nullptr ? ::PQerrorMessage(c) : "";
	std::string_view trimmed{raw != nullptr ? raw : ""};
	rstrip_nl_(trimmed);
	std::string sqlstate;
	if (c != nullptr && ::PQstatus(c) == CONNECTION_BAD) {
		sqlstate = "08006";
	}
	return PgError{std::format("{}: {}", label, trimmed), std::move(sqlstate)};
}
inline PgError from_result(
	PGresult *r,
	std::string_view label) {
	auto const fetch = [r](int code) -> std::string {
		char const *p = ::PQresultErrorField(r, code);
		return p != nullptr ? std::string{p} : std::string{};
	};
	auto state = fetch(PG_DIAG_SQLSTATE);
	auto primary = fetch(PG_DIAG_MESSAGE_PRIMARY);
	if (primary.empty()) {
		char const *p = ::PQresultErrorMessage(r);
		primary = p != nullptr ? std::string{p} : std::string{};
	}
	rstrip_nl_(primary);
	PgError e{std::format("{}: {}", label, primary), std::move(state), ::PQresultStatus(r)};
	e.detail = fetch(PG_DIAG_MESSAGE_DETAIL);
	e.hint = fetch(PG_DIAG_MESSAGE_HINT);
	e.where = fetch(PG_DIAG_CONTEXT);
	return e;
}
inline void install_sigpipe_ignore() noexcept {
	static std::once_flag flag;
	std::call_once(flag, [] {
		// Best effort: individual libpq calls still surface EPIPE/write failures.
		[[maybe_unused]] auto previous_handler = ::signal(SIGPIPE, SIG_IGN);
	});
}
inline bool valid_query_name(
	std::string_view name) noexcept {
	if (name.empty() || name == "." || name == "..") {
		return false;
	}
	for (char const ch: name) {
		if ((ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '_'
			|| ch == '-'
			|| ch == '.') {
			continue;
		}
		return false;
	}
	return true;
}
inline std::uint64_t fnv1a64(
	std::string_view s) noexcept {
	std::uint64_t h = 14695981039346656037ULL;
	for (char const c: s) {
		h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
		h *= 1099511628211ULL;
	}
	return h;
}

} // namespace detail
export struct ConnectParams {
	std::string conninfo{};
	std::chrono::milliseconds connect_deadline{std::chrono::seconds{15}};
};
export struct QueryOptions {
	std::optional<std::chrono::milliseconds> deadline{};
};
export class StatementCache {
public:
	struct Entry {
		std::string name;
		std::shared_ptr<std::string const> sql;
		std::vector<Oid> param_types;
	};
	static std::string stable_name(
		std::string_view sql) {
		static constexpr std::string_view kAlphabet{"abcdefghijklmnopqrstuvwxyz234567"};
		std::uint64_t const h = detail::fnv1a64(sql);
		std::string name;
		name.reserve(15);
		name += "p_";
		for (int shift = 59; shift >= 4; shift -= 5) {
			name += kAlphabet[(h >> static_cast<std::uint32_t>(shift)) & 0x1fU];
		}
		name += kAlphabet[(h & 0xfU) << 1U]; // last 4 bits, pad LSB
		return name;
	}
	[[nodiscard]] std::shared_ptr<Entry const> get(
		std::shared_ptr<std::string const> sql,
		std::vector<Oid> param_types = {}) {
		std::string const name = stable_name(*sql);
		{
			std::shared_lock const lk{mu_};
			if (auto it = cache_.find(name); it != cache_.end()) {
				return it->second;
			}
		}
		auto entry = std::make_shared<Entry const>(Entry{name, std::move(sql), std::move(param_types)});
		std::scoped_lock const lk{mu_};
		auto [it, _] = cache_.try_emplace(name, std::move(entry));
		return it->second;
	}
	[[nodiscard]] std::shared_ptr<Entry const> get(
		std::string_view sql,
		std::vector<Oid> param_types = {}) {
		return get(std::make_shared<std::string const>(sql), std::move(param_types));
	}
	void clear() noexcept {
		std::scoped_lock const lk{mu_};
		cache_.clear();
	}

private:
	mutable std::shared_mutex mu_{};
	mutable std::unordered_map<std::string, std::shared_ptr<Entry const>> cache_{};
};

export class Connection;
export class Pipeline {
public:
	Pipeline() = default;
	Pipeline(
		std::shared_ptr<Connection> conn) noexcept
		: state_{std::make_shared<State>()} {
		state_->conn = std::move(conn);
	}
	Pipeline(Pipeline const &) = delete;
	Pipeline &operator =(Pipeline const &) = delete;
	Pipeline(Pipeline &&) noexcept = default;
	Pipeline &operator =(Pipeline &&) noexcept = default;
	~Pipeline() { close_(); }
	root::Task<Result> query(std::string_view sql, Params params = {});
	root::Task<Result> exec_cached(std::shared_ptr<StatementCache::Entry const> const &stmt, Params params = {});
	root::Task<void> sync();

private:
	enum class PendingKind : std::uint8_t {
		query,
		exec_cached
	};
	enum class WireResultKind : std::uint8_t {
		prepare,
		query
	};
	struct PendingQuery {
		PendingKind kind{PendingKind::query};
		std::shared_ptr<root::TaskSource<Result>> dst;
		std::string sql;
		Params params;
		std::shared_ptr<StatementCache::Entry const> stmt{};
	};
	struct WireResult {
		WireResultKind kind{WireResultKind::query};
		std::shared_ptr<root::TaskSource<Result>> dst;
		std::string label;
		std::string prepared_name;
	};
	struct SyncState;
	struct State {
		std::shared_ptr<Connection> conn{};
		bool closed{false};
		bool syncing{false};
		std::deque<PendingQuery> pending{};
		std::shared_ptr<SyncState> active_sync{};
	};
	struct SyncState {
		std::deque<PendingQuery> batch;
		std::vector<WireResult> wire_results{};
		std::size_t next_wire_result{0};
		std::shared_ptr<root::TaskSource<void>> done;
		std::shared_ptr<State> pipe;
		bool entered_pipeline{false};
	};
	void close_() noexcept;
	static void sync_next_(std::shared_ptr<SyncState> const &st);
	static void start_wire_sync_(std::shared_ptr<SyncState> const &st);
	static void after_wire_send_drive_flush_(std::shared_ptr<SyncState> const &st);
	static void drive_wire_consume_loop_(std::shared_ptr<SyncState> const &st);
	static void fail_wire_sync_(std::shared_ptr<SyncState> const &st, std::exception_ptr error) noexcept;
	static void finish_sync_(std::shared_ptr<State> const &pipe, bool success) noexcept;

	std::shared_ptr<State> state_{};
};
export class Connection : public std::enable_shared_from_this<Connection> {
public:
	Connection(Connection const &) = delete;
	Connection &operator =(Connection const &) = delete;
	Connection(Connection &&) = delete;
	Connection &operator =(Connection &&) = delete;
	~Connection() { close(); }
	static root::Task<std::shared_ptr<Connection>> connect(ConnectParams const &params);

	root::Task<Result> query(std::string_view sql, Params params = {});
	root::Task<Result> query(std::shared_ptr<std::string const> sql, Params params = {});

	root::Task<void> prepare(std::string_view name, std::string_view sql, std::span<Oid const> param_types = {});
	root::Task<void> prepare(std::string_view name, std::shared_ptr<std::string const> sql, std::span<Oid const> param_types = {});

	root::Task<Result> exec_prepared(std::string_view name, Params params = {});
	root::Task<Result> exec_cached(std::shared_ptr<StatementCache::Entry const> const &stmt, Params params = {});

	root::Task<void> cancel_inflight(WorkPool &cancel_pool);
	root::Task<void> cancel_inflight();
	root::Task<Pipeline> pipeline();

	root::Task<Result> query(std::string_view sql, Params params, QueryOptions opts);
	[[nodiscard]] bool ok() const noexcept { return conn_ && ::PQstatus(conn_.get()) == CONNECTION_OK; }
	[[nodiscard]] std::string last_error() const {
		if (!conn_) {
			return {};
		}
		char const *p = ::PQerrorMessage(conn_.get());
		return p != nullptr ? std::string{p} : std::string{};
	}
	[[nodiscard]] PGconn *raw() const noexcept { return conn_.get(); }
	[[nodiscard]] int backend_pid() const noexcept { return conn_ ? ::PQbackendPID(conn_.get()) : 0; }
	[[nodiscard]] int server_version() const noexcept { return conn_ ? ::PQserverVersion(conn_.get()) : 0; }
	void close() noexcept;

private:
	Connection(
		PGConnPtr conn,
		FileReader *reader) noexcept
		: conn_{std::move(conn)}
		, reader_{reader}
		, owner_{std::this_thread::get_id()} {}
	void enqueue_job_(std::function<void()> job);
	void start_next_();

	void run_query_(std::string const &sql, Params const &params, std::shared_ptr<root::TaskSource<Result>> const &dst);
	void run_prepare_(
		std::string const &name,
		std::string const &sql,
		std::vector<Oid> oids,
		std::shared_ptr<root::TaskSource<void>> const &dst);
	void run_exec_prepared_(std::string const &name, Params const &params, std::shared_ptr<root::TaskSource<Result>> const &dst);

	template<class T>
	void after_send_drive_flush_(std::shared_ptr<root::TaskSource<T>> dst, std::shared_ptr<Result> partial, std::string const &label);
	template<class T>
	void drive_consume_loop_(std::shared_ptr<root::TaskSource<T>> dst, std::shared_ptr<Result> partial, std::string const &label);
	template<class T>
	void reject_(
		std::shared_ptr<root::TaskSource<T>> const &dst,
		std::string const &label) {
		auto err = detail::from_conn(conn_.get(), label);
		auto _ = dst->try_set_exception(std::make_exception_ptr(std::move(err)));
	}
	void op_done_();

	PGConnPtr conn_;
	FileReader *reader_{nullptr};
	std::thread::id owner_{};
	bool closed_{false};
	bool in_flight_{false};
	std::deque<std::function<void()>> queue_{};
	std::unordered_set<std::string> prepared_names_{};
	bool pipeline_mode_{false};

	friend struct detail::ConnectState;
	friend class Pipeline;
};
export class QueryCache {
	struct TransparentHash {
		using is_transparent = void;
		std::size_t operator ()(
			std::string_view s) const noexcept {
			return std::hash<std::string_view>{}(s);
		}
		std::size_t operator ()(
			std::string const &s) const noexcept {
			return std::hash<std::string_view>{}(s);
		}
	};
	std::filesystem::path root_{};
	mutable std::shared_mutex mtx_{};
	mutable std::unordered_map<std::string, std::shared_ptr<std::string const>, TransparentHash, std::equal_to<>> cache_{};

public:
	explicit QueryCache(
		std::filesystem::path root)
		: root_{std::move(root)} {}
	[[nodiscard]] std::shared_ptr<std::string const> lookup(
		std::string_view name) const noexcept {
		std::shared_lock const lk{mtx_};
		auto it = cache_.find(name);
		return it != cache_.end() ? it->second : nullptr;
	}
	[[nodiscard]] std::shared_ptr<std::string const> load_or_throw(
		std::string_view name) const {
		if (!detail::valid_query_name(name)) {
			throw std::invalid_argument{std::format("invalid query name: {}", name)};
		}
		{
			std::shared_lock const lk{mtx_};
			if (auto it = cache_.find(name); it != cache_.end()) {
				return it->second;
			}
		}
		auto path = root_ / (std::string{name} + ".psql");
		auto bytes = blocking_read_text_file(path.string());
		if (!bytes) {
			throw std::filesystem::filesystem_error{
				"query file open failed",
				path,
				bytes.error().code()
            };
		}
		std::string contents{std::move(*bytes)};
		auto sp = std::make_shared<std::string const>(std::move(contents));
		std::scoped_lock const lk{mtx_};
		auto [it, _] = cache_.try_emplace(std::string{name}, sp);
		return it->second;
	}
	[[nodiscard]] root::Task<std::shared_ptr<std::string const>> load_async(std::string_view name);
	void clear() noexcept {
		std::scoped_lock const lk{mtx_};
		cache_.clear();
	}
};
// ===========================================================================
// Implementation
// ===========================================================================

namespace detail {

struct ConnectState : std::enable_shared_from_this<ConnectState> {
	PGConnPtr conn{};
	FileReader *reader{nullptr};
	std::shared_ptr<root::TaskSource<std::shared_ptr<Connection>>> dst{};
	std::chrono::steady_clock::time_point deadline{};
	void start() {
		install_sigpipe_ignore();
		drive(/*initial=*/true);
	}
	void drive(
		bool initial) {
		if (std::chrono::steady_clock::now() > deadline) {
			auto _ =
				dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connect deadline exceeded", "08001"}));
			return;
		}
		PostgresPollingStatusType const status = initial ? PGRES_POLLING_WRITING : ::PQconnectPoll(conn.get());
		while (true) {
			if (::PQstatus(conn.get()) == CONNECTION_BAD && !initial) {
				auto _ = dst->try_set_exception(std::make_exception_ptr(from_conn(conn.get(), "conflux.pg: connect")));
				return;
			}
			if (status == PGRES_POLLING_FAILED) {
				auto _ = dst->try_set_exception(std::make_exception_ptr(from_conn(conn.get(), "conflux.pg: connect")));
				return;
			}
			if (status == PGRES_POLLING_OK) {
				if (::PQsetnonblocking(conn.get(), 1) != 0) {
					auto _ = dst->try_set_exception(
						std::make_exception_ptr(from_conn(conn.get(), "conflux.pg: PQsetnonblocking")));
					return;
				}
				auto c = std::shared_ptr<Connection>(new Connection{std::move(conn), reader});
				// P11b: pin client_encoding to UTF-8 before publishing.
				auto outer = dst;
				auto conn_sp = c;
				[](std::shared_ptr<root::TaskSource<std::shared_ptr<Connection>>> outer,
				   std::shared_ptr<Connection> conn_sp,
				   root::Task<Result> q_task) mutable -> root::Task<void> {
					try {
						co_await std::move(q_task);
						char const *enc = ::PQparameterStatus(conn_sp->raw(), "client_encoding");
						if (enc == nullptr || std::string_view{enc} != std::string_view{"UTF8"}) {
							auto _ = outer->try_set_exception(
								std::make_exception_ptr(PgError{"conflux.pg: client_encoding must be UTF8", "22021"}));
							co_return;
						}
						auto _ = outer->try_set_value(root::Success<std::shared_ptr<Connection>>{std::move(conn_sp)});
					} catch (Cancelled const &) {
						auto _ = outer->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connect cancelled"}));
					} catch (...) { auto _ = outer->try_set_exception(std::current_exception()); }
				}(outer, conn_sp, conn_sp->query(std::string_view{"SET client_encoding = 'UTF8'"}))
															 .detach();
				return;
			}
			short mask = 0;
			if (status == PGRES_POLLING_READING) {
				mask = POLLIN;
			} else if (status == PGRES_POLLING_WRITING) {
				mask = POLLOUT;
			} else {
				auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: std::unexpected polling status"}));
				return;
			}
			int const fd = ::PQsocket(conn.get());
			if (fd < 0) {
				auto _ = dst->try_set_exception(std::make_exception_ptr(from_conn(conn.get(), "conflux.pg: PQsocket")));
				return;
			}
			auto self = shared_from_this();
			bool const armed = reader->poll_add_oneshot(fd, mask, [self](IoResult r) {
				if (r.res < 0) {
					auto _ = self->dst->try_set_exception(
						std::make_exception_ptr(PgError{std::format("conflux.pg: poll: {}", strerror(-r.res))}));
					return;
				}
				self->drive(false);
			});
			if (!armed) {
				auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: io_uring SQ full"}));
			}
			return;
		}
	}
};
struct PGcancelDeleter {
	void operator ()(
		PGcancel *c) const noexcept {
		if (c != nullptr) {
			::PQfreeCancel(c);
		}
	}
};
using PGcancelPtr = std::unique_ptr<PGcancel, PGcancelDeleter>;
inline WorkPool &cancel_pool() {
	static WorkPool pool{WorkPoolOptions{.threads = 1}};
	return pool;
}

} // namespace detail
root::Task<std::shared_ptr<Connection>> Connection::connect(
	ConnectParams const &params) {
	auto [task, raw_src] =
		root::make_task_source<std::shared_ptr<Connection>>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<std::shared_ptr<Connection>>>(std::move(raw_src));
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		auto _ = shared_src->try_set_exception(
			std::make_exception_ptr(PgError{"conflux.pg: no current FileReader (not on a ring lane)"}));
		return std::move(task);
	}
	if (::PQisthreadsafe() == 0) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: libpq built without std::thread safety"}));
		return std::move(task);
	}
	PGConnPtr conn{::PQconnectStart(params.conninfo.c_str())};
	if (!conn || ::PQstatus(conn.get()) == CONNECTION_BAD) {
		auto _ = shared_src->try_set_exception(
			std::make_exception_ptr(detail::from_conn(conn.get(), "conflux.pg: PQconnectStart")));
		return std::move(task);
	}
	auto st = std::make_shared<detail::ConnectState>();
	st->conn = std::move(conn);
	st->reader = reader;
	st->dst = shared_src;
	st->deadline = std::chrono::steady_clock::now() + params.connect_deadline;
	st->start();
	return std::move(task);
}
void Connection::close() noexcept {
	if (closed_) {
		return;
	}
	closed_ = true;
	conn_.reset();
	queue_.clear();
}
void Connection::enqueue_job_(
	std::function<void()> job) {
	if (in_flight_) {
		queue_.push_back(std::move(job));
	} else {
		in_flight_ = true;
		job();
	}
}
void Connection::start_next_() {
	if (queue_.empty()) {
		in_flight_ = false;
		return;
	}
	auto job = std::move(queue_.front());
	queue_.pop_front();
	job();
}
void Connection::op_done_() {
	start_next_();
}
root::Task<Result> Connection::query(
	std::string_view sql,
	Params params) {
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query while pipeline active"}));
		return std::move(task);
	}
	auto self = shared_from_this();
	enqueue_job_([self, sql_owned = std::string{sql}, params = std::move(params), shared_src]() mutable {
		self->run_query_(sql_owned, params, shared_src);
	});
	return std::move(task);
}
root::Task<Result> Connection::query(
	std::shared_ptr<std::string const> sql,
	Params params) {
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (!sql) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: null SQL handle"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query while pipeline active"}));
		return std::move(task);
	}
	auto self = shared_from_this();
	enqueue_job_([self, sql = std::move(sql), params = std::move(params), shared_src]() mutable {
		self->run_query_(*sql, params, shared_src);
	});
	return std::move(task);
}
void Connection::run_query_(
	std::string const &sql,
	Params const &params,
	std::shared_ptr<root::TaskSource<Result>> const &dst) {
	int const n = params.count();
	int const send = ::PQsendQueryParams(
		conn_.get(),
		sql.c_str(),
		n,
		params.types(),
		params.values(),
		params.lengths(),
		params.formats(),
		params.result_format());
	if (send == 0) {
		reject_(dst, "conflux.pg: PQsendQueryParams");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, std::make_shared<Result>(), "conflux.pg: query");
}
root::Task<void> Connection::prepare(
	std::string_view name,
	std::string_view sql,
	std::span<Oid const> param_types) {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: prepare off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: prepare while pipeline active"}));
		return std::move(task);
	}
	auto self = shared_from_this();
	enqueue_job_([self,
				  name_owned = std::string{name},
				  sql_owned = std::string{sql},
				  oids = std::vector<Oid>{param_types.begin(), param_types.end()},
				  shared_src]() mutable { self->run_prepare_(name_owned, sql_owned, std::move(oids), shared_src); });
	return std::move(task);
}
root::Task<void> Connection::prepare(
	std::string_view name,
	std::shared_ptr<std::string const> sql,
	std::span<Oid const> param_types) {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (!sql) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: null SQL handle"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: prepare off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: prepare while pipeline active"}));
		return std::move(task);
	}
	auto self = shared_from_this();
	enqueue_job_([self,
				  name_owned = std::string{name},
				  sql = std::move(sql),
				  oids = std::vector<Oid>{param_types.begin(), param_types.end()},
				  shared_src]() mutable { self->run_prepare_(name_owned, *sql, std::move(oids), shared_src); });
	return std::move(task);
}
void Connection::run_prepare_(
	std::string const &name,
	std::string const &sql,
	std::vector<Oid> oids,
	std::shared_ptr<root::TaskSource<void>> const &dst) {
	int const send = ::PQsendPrepare(
		conn_.get(),
		name.c_str(),
		sql.c_str(),
		static_cast<int>(oids.size()),
		oids.empty() ? nullptr : oids.data());
	if (send == 0) {
		reject_(dst, "conflux.pg: PQsendPrepare");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, std::make_shared<Result>(), "conflux.pg: prepare");
}
root::Task<Result> Connection::exec_prepared(
	std::string_view name,
	Params params) {
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: exec_prepared off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ = shared_src->try_set_exception(
			std::make_exception_ptr(PgError{"conflux.pg: exec_prepared while pipeline active"}));
		return std::move(task);
	}
	auto self = shared_from_this();
	enqueue_job_([self, name_owned = std::string{name}, params = std::move(params), shared_src]() mutable {
		self->run_exec_prepared_(name_owned, params, shared_src);
	});
	return std::move(task);
}
void Connection::run_exec_prepared_(
	std::string const &name,
	Params const &params,
	std::shared_ptr<root::TaskSource<Result>> const &dst) {
	int const n = params.count();
	int const send = ::PQsendQueryPrepared(
		conn_.get(),
		name.c_str(),
		n,
		params.values(),
		params.lengths(),
		params.formats(),
		params.result_format());
	if (send == 0) {
		reject_(dst, "conflux.pg: PQsendQueryPrepared");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, std::make_shared<Result>(), "conflux.pg: query");
}
root::Task<Result> Connection::exec_cached(
	std::shared_ptr<StatementCache::Entry const> const &stmt,
	Params params) {
	if (!stmt || !stmt->sql) {
		auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: null cached statement"}));
		return std::move(task);
	}
	if (closed_ || !conn_) {
		auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: exec_cached while pipeline active"}));
		return std::move(task);
	}
	if (prepared_names_.contains(stmt->name)) {
		return exec_prepared(stmt->name, std::move(params));
	}
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	auto self = shared_from_this();
	[](std::shared_ptr<Connection> self,
	   std::shared_ptr<StatementCache::Entry const> stmt,
	   Params params,
	   std::shared_ptr<root::TaskSource<Result>> shared_src,
	   root::Task<void> prep_task) mutable -> root::Task<void> {
		try {
			co_await std::move(prep_task);
			self->prepared_names_.insert(stmt->name);
		} catch (Cancelled const &) {
			auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
			co_return;
		} catch (PgError const &e) {
			if (e.sqlstate != "42P05") {
				auto _ = shared_src->try_set_exception(std::current_exception());
				co_return;
			}
			self->prepared_names_.insert(stmt->name);
		} catch (...) {
			auto _ = shared_src->try_set_exception(std::current_exception());
			co_return;
		}
		try {
			auto r = co_await self->exec_prepared(stmt->name, std::move(params));
			auto _ = shared_src->try_set_value(root::Success<Result>{std::move(r)});
		} catch (Cancelled const &) {
			auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	}(self, stmt, std::move(params), shared_src, prepare(stmt->name, stmt->sql, stmt->param_types))
												  .detach();
	return std::move(task);
}
template<class T>
void Connection::after_send_drive_flush_(
	std::shared_ptr<root::TaskSource<T>> dst,
	std::shared_ptr<Result> partial,
	std::string const &label) {
	int const f = ::PQflush(conn_.get());
	if (f < 0) {
		reject_(dst, "conflux.pg: PQflush");
		op_done_();
		return;
	}
	if (f == 0) {
		drive_consume_loop_(std::move(dst), std::move(partial), label);
		return;
	}
	int const fd = ::PQsocket(conn_.get());
	auto self = shared_from_this();
	// NOLINTNEXTLINE(bugprone-std::exception-escape) — poll callback; any throw would terminate, treated as fatal.
	bool const armed = reader_->poll_add_oneshot(fd, POLLOUT, [self, dst, partial, label](IoResult r) mutable {
		if (self->closed_) {
			auto _ = dst->try_set_cancelled(root::work_errc::cancelled_requested);
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			auto _ = dst->try_set_exception(
				std::make_exception_ptr(PgError{std::format("conflux.pg: poll write: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		self->after_send_drive_flush_(std::move(dst), std::move(partial), label);
	});
	if (!armed) {
		auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: io_uring SQ full"}));
		op_done_();
	}
}
// Contract: callers issue a single-statement query via PQsendQueryParams/
// PQsendPrepare/PQsendQueryPrepared, so PQgetResult produces at most one
// non-null result followed by nullptr. Multi-statement queries keep only
// the last result.
template<class T>
void Connection::drive_consume_loop_(
	std::shared_ptr<root::TaskSource<T>> dst,
	std::shared_ptr<Result> partial,
	std::string const &label) {
	if (::PQconsumeInput(conn_.get()) == 0) {
		reject_(dst, "conflux.pg: PQconsumeInput");
		op_done_();
		return;
	}
	while (::PQisBusy(conn_.get()) == 0) {
		PGResultPtr next{::PQgetResult(conn_.get())};
		if (!next) {
			if (partial && *partial && partial->ok()) {
				if constexpr (std::is_void_v<T>) {
					auto _ = dst->try_set_value(root::Success<void>{});
				} else {
					auto _ = dst->try_set_value(root::Success<T>{std::move(*partial)});
				}
			} else if (partial && *partial) {
				auto err = detail::from_result(partial->raw(), label);
				auto _ = dst->try_set_exception(std::make_exception_ptr(std::move(err)));
			} else {
				auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: empty result"}));
			}
			op_done_();
			return;
		}
		auto status = ::PQresultStatus(next.get());
		if (status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE || status == PGRES_NONFATAL_ERROR) {
			auto err = detail::from_result(next.get(), label);
			while (PGresult *drain = ::PQgetResult(conn_.get())) {
				::PQclear(drain);
			}
			auto _ = dst->try_set_exception(std::make_exception_ptr(std::move(err)));
			op_done_();
			return;
		}
		*partial = Result{std::move(next)};
	}
	int const fd = ::PQsocket(conn_.get());
	auto self = shared_from_this();
	// NOLINTNEXTLINE(bugprone-std::exception-escape) — poll callback; any throw would terminate, treated as fatal.
	bool const armed = reader_->poll_add_oneshot(fd, POLLIN, [self, dst, partial, label](IoResult r) mutable {
		if (self->closed_) {
			auto _ = dst->try_set_cancelled(root::work_errc::cancelled_requested);
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			auto _ = dst->try_set_exception(
				std::make_exception_ptr(PgError{std::format("conflux.pg: poll read: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		self->drive_consume_loop_(std::move(dst), std::move(partial), label);
	});
	if (!armed) {
		auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: io_uring SQ full"}));
		op_done_();
	}
}
root::Task<void> Connection::cancel_inflight(
	WorkPool &wpool) {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	if (!conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	detail::PGcancelPtr handle{::PQgetCancel(conn_.get())};
	if (!handle) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: PQgetCancel returned null"}));
		return std::move(task);
	}
	auto cancel_handle = std::shared_ptr<PGcancel>{handle.release(), detail::PGcancelDeleter{}};
	bool const queued = wpool.enqueue([cancel_handle, shared_src]() mutable {
		std::array<char, 256> buf{};
		int const ok = ::PQcancel(cancel_handle.get(), buf.data(), static_cast<int>(buf.size()));
		if (ok == 0) {
			auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{std::string{buf.data()}}));
		} else {
			auto _ = shared_src->try_set_value(root::Success<void>{});
		}
	});
	if (!queued) {
		auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
	}
	return std::move(task);
}
root::Task<void> Connection::cancel_inflight() {
	return cancel_inflight(detail::cancel_pool());
}
root::Task<Result> Connection::query(
	std::string_view sql,
	Params params,
	QueryOptions opts) {
	if (!opts.deadline || opts.deadline->count() <= 0) {
		return query(sql, std::move(params));
	}
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		return query(sql, std::move(params));
	}
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	auto self = shared_from_this();
	[](std::shared_ptr<root::TaskSource<Result>> src, root::Task<Result> qt) -> root::Task<void> {
		try {
			auto r = co_await std::move(qt);
			auto _ = src->try_set_value(root::Success<Result>{std::move(r)});
		} catch (Cancelled const &) {
			auto _ = src->try_set_cancelled(root::work_errc::cancelled_requested);
		} catch (...) { auto _ = src->try_set_exception(std::current_exception()); }
	}(shared_src, query(sql, std::move(params)))
																	   .detach();
	auto const deadline = *opts.deadline;
	[](std::shared_ptr<Connection> s, std::shared_ptr<root::TaskSource<Result>> src, root::Task<void> tt) -> root::Task<void> {
		try {
			co_await std::move(tt);
			if (src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query deadline exceeded", "57014"}))) {
				[](std::shared_ptr<Connection> s2) -> root::Task<void> {
					try {
						co_await s2->cancel_inflight();
					} catch (...) {}
				}(s)
											 .detach();
			}
		} catch (...) {}
	}(self,
	  shared_src,
	  conflux::uring::async_timeout(
		  reader->ring(),
		  *reader->completions(),
		  [reader](std::uint32_t slot, std::uint32_t gen) noexcept { return reader->encode_ud(slot, gen); },
		  deadline))
																					   .detach();
	return std::move(task);
}
root::Task<Pipeline> Connection::pipeline() {
	auto [task, raw_src] = root::make_task_source<Pipeline>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Pipeline>>(std::move(raw_src));
	if (closed_ || !conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline off owner std::thread"}));
		return std::move(task);
	}
	if (pipeline_mode_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline already active"}));
		return std::move(task);
	}
	if (in_flight_ || !queue_.empty()) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline while connection busy"}));
		return std::move(task);
	}
	pipeline_mode_ = true;
	auto _ = shared_src->try_set_value(root::Success<Pipeline>{Pipeline{shared_from_this()}});
	return std::move(task);
}
root::Task<Result> Pipeline::query(
	std::string_view sql,
	Params params) {
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	auto const st = state_;
	if (!st || st->closed || !st->conn || !st->conn->conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != st->conn->owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query off owner std::thread"}));
		return std::move(task);
	}
	if (st->syncing) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: query while sync in progress"}));
		return std::move(task);
	}
	st->pending.push_back(
		PendingQuery{
			.kind = PendingKind::query,
			.dst = shared_src,
			.sql = std::string{sql},
			.params = std::move(params),
		});
	return std::move(task);
}
root::Task<Result> Pipeline::exec_cached(
	std::shared_ptr<StatementCache::Entry const> const &stmt,
	Params params) {
	if (!stmt || !stmt->sql) {
		auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: null cached statement"}));
		return std::move(task);
	}
	auto [task, raw_src] = root::make_task_source<Result>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<Result>>(std::move(raw_src));
	auto const st = state_;
	if (!st || st->closed || !st->conn || !st->conn->conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != st->conn->owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: exec_cached off owner std::thread"}));
		return std::move(task);
	}
	if (st->syncing) {
		auto _ = shared_src->try_set_exception(
			std::make_exception_ptr(PgError{"conflux.pg: exec_cached while sync in progress"}));
		return std::move(task);
	}
	st->pending.push_back(
		PendingQuery{
			.kind = PendingKind::exec_cached,
			.dst = shared_src,
			.sql = *stmt->sql,
			.params = std::move(params),
			.stmt = stmt,
		});
	return std::move(task);
}
root::Task<void> Pipeline::sync() {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	auto const st = state_;
	if (!st || st->closed || !st->conn || !st->conn->conn_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != st->conn->owner_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: sync off owner std::thread"}));
		return std::move(task);
	}
	if (st->syncing) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: sync already in progress"}));
		return std::move(task);
	}
	if (st->pending.empty()) {
		auto _ = shared_src->try_set_value(root::Success<void>{});
		return std::move(task);
	}
	st->syncing = true;
	auto sync_state = std::make_shared<SyncState>();
	sync_state->batch = std::move(st->pending);
	sync_state->done = shared_src;
	sync_state->pipe = st;
	st->active_sync = sync_state;
	state_->pending.clear();
	st->conn->in_flight_ = true;
	start_wire_sync_(sync_state);
	return std::move(task);
}
// NOLINTNEXTLINE(misc-no-recursion) — mutual indirect recursion through async on Connection::query boundary; no stack
// cycle.
void Pipeline::sync_next_(
	std::shared_ptr<SyncState> const &st) {
	auto const pipe = st->pipe;
	if (st->batch.empty()) {
		auto _ = st->done->try_set_value(root::Success<void>{});
		finish_sync_(pipe, true);
		return;
	}
	auto item = std::move(st->batch.front());
	st->batch.pop_front();
	auto shared_src = item.dst;
	[](std::shared_ptr<SyncState> st, std::shared_ptr<root::TaskSource<Result>> shared_src, root::Task<Result> qt) -> root::Task<void> {
		try {
			auto r = co_await std::move(qt);
			auto _ = shared_src->try_set_value(root::Success<Result>{std::move(r)});
			Pipeline::sync_next_(st);
		} catch (Cancelled const &) {
			auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
			while (!st->batch.empty()) {
				auto rem = std::move(st->batch.front());
				st->batch.pop_front();
				auto _ = rem.dst->try_set_cancelled(root::work_errc::cancelled_requested);
			}
			auto _ = st->done->try_set_cancelled(root::work_errc::cancelled_requested);
			Pipeline::finish_sync_(st->pipe, false);
		} catch (...) {
			auto _ = shared_src->try_set_exception(std::current_exception());
			while (!st->batch.empty()) {
				auto rem = std::move(st->batch.front());
				st->batch.pop_front();
				auto _ = rem.dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline query"}));
			}
			auto _ = st->done->try_set_value(root::Success<void>{});
			Pipeline::finish_sync_(st->pipe, true);
		}
	}(st, shared_src, pipe->conn->query(item.sql, std::move(item.params)))
																								.detach();
}
void Pipeline::start_wire_sync_(
	std::shared_ptr<SyncState> const &st) {
	auto const pipe = st->pipe;
	if (!pipe || pipe->closed || !pipe->conn || !pipe->conn->conn_) {
		fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		if (pipe && pipe->conn) {
			pipe->conn->op_done_();
		}
		return;
	}
#if !defined(LIBPQ_HAS_PIPELINING)
	fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: libpq pipeline mode unavailable"}));
	pipe->conn->op_done_();
#else
	auto const conn = pipe->conn;
	if (::PQenterPipelineMode(conn->conn_.get()) == 0) {
		fail_wire_sync_(
			st,
			std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQenterPipelineMode")));
		conn->op_done_();
		return;
	}
	st->entered_pipeline = true;
	std::unordered_set<std::string> planned_prepares;
	while (!st->batch.empty()) {
		auto item = std::move(st->batch.front());
		st->batch.pop_front();
		int send = 0;
		if (item.kind == PendingKind::exec_cached) {
			if (!item.stmt || !item.stmt->sql) {
				auto _ = item.dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: null cached statement"}));
				continue;
			}
			if (!conn->prepared_names_.contains(item.stmt->name) && !planned_prepares.contains(item.stmt->name)) {
				send = ::PQsendPrepare(
					conn->conn_.get(),
					item.stmt->name.c_str(),
					item.stmt->sql->c_str(),
					static_cast<int>(item.stmt->param_types.size()),
					item.stmt->param_types.empty() ? nullptr : item.stmt->param_types.data());
				if (send == 0) {
					fail_wire_sync_(
						st,
						std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQsendPrepare")));
					conn->op_done_();
					return;
				}
				planned_prepares.insert(item.stmt->name);
				st->wire_results.push_back(
					WireResult{
						.kind = WireResultKind::prepare,
						.dst = item.dst,
						.label = "conflux.pg: pipeline prepare",
						.prepared_name = item.stmt->name,
					});
			}
			int const n = item.params.count();
			send = ::PQsendQueryPrepared(
				conn->conn_.get(),
				item.stmt->name.c_str(),
				n,
				item.params.values(),
				item.params.lengths(),
				item.params.formats(),
				item.params.result_format());
			if (send == 0) {
				fail_wire_sync_(
					st,
					std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQsendQueryPrepared")));
				conn->op_done_();
				return;
			}
		} else {
			int const n = item.params.count();
			send = ::PQsendQueryParams(
				conn->conn_.get(),
				item.sql.c_str(),
				n,
				item.params.types(),
				item.params.values(),
				item.params.lengths(),
				item.params.formats(),
				item.params.result_format());
			if (send == 0) {
				fail_wire_sync_(
					st,
					std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQsendQueryParams")));
				conn->op_done_();
				return;
			}
		}
		st->wire_results.push_back(
			WireResult{
				.kind = WireResultKind::query,
				.dst = item.dst,
				.label = "conflux.pg: pipeline query",
			});
	}
	if (::PQpipelineSync(conn->conn_.get()) == 0) {
		fail_wire_sync_(st, std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQpipelineSync")));
		conn->op_done_();
		return;
	}
	after_wire_send_drive_flush_(st);
#endif
}
void Pipeline::after_wire_send_drive_flush_(
	std::shared_ptr<SyncState> const &st) {
#if defined(LIBPQ_HAS_PIPELINING)
	auto const pipe = st->pipe;
	auto const conn = pipe ? pipe->conn : nullptr;
	if (!conn || !conn->conn_) {
		fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		if (conn) {
			conn->op_done_();
		}
		return;
	}
	int const f = ::PQflush(conn->conn_.get());
	if (f < 0) {
		fail_wire_sync_(st, std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQflush")));
		conn->op_done_();
		return;
	}
	if (f == 0) {
		drive_wire_consume_loop_(st);
		return;
	}
	int const fd = ::PQsocket(conn->conn_.get());
	auto self = conn;
	bool const armed = conn->reader_->poll_add_oneshot(fd, POLLOUT, [self, st](IoResult r) mutable {
		if (self->closed_) {
			fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			fail_wire_sync_(st, std::make_exception_ptr(PgError{std::format("conflux.pg: poll write: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		after_wire_send_drive_flush_(st);
	});
	if (!armed) {
		fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: io_uring SQ full"}));
		conn->op_done_();
	}
#else
	(void)st;
#endif
}
void Pipeline::drive_wire_consume_loop_(
	std::shared_ptr<SyncState> const &st) {
#if defined(LIBPQ_HAS_PIPELINING)
	auto const pipe = st->pipe;
	auto const conn = pipe ? pipe->conn : nullptr;
	if (!conn || !conn->conn_) {
		fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
		if (conn) {
			conn->op_done_();
		}
		return;
	}
	if (::PQconsumeInput(conn->conn_.get()) == 0) {
		fail_wire_sync_(st, std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQconsumeInput")));
		conn->op_done_();
		return;
	}
	bool retried_after_null = false;
	while (::PQisBusy(conn->conn_.get()) == 0) {
		PGResultPtr next{::PQgetResult(conn->conn_.get())};
		if (!next) {
			if (!retried_after_null) {
				retried_after_null = true;
				continue;
			}
			break;
		}
		retried_after_null = false;
		auto const status = ::PQresultStatus(next.get());
		if (status == PGRES_PIPELINE_SYNC) {
			if (st->next_wire_result != st->wire_results.size()) {
				fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: pipeline sync before all results"}));
				conn->op_done_();
				return;
			}
			if (::PQexitPipelineMode(conn->conn_.get()) == 0) {
				fail_wire_sync_(
					st,
					std::make_exception_ptr(detail::from_conn(conn->conn_.get(), "conflux.pg: PQexitPipelineMode")));
				conn->op_done_();
				return;
			}
			st->entered_pipeline = false;
			auto _ = st->done->try_set_value(root::Success<void>{});
			finish_sync_(pipe, true);
			conn->op_done_();
			return;
		}
		if (st->next_wire_result >= st->wire_results.size()) {
			fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: std::unexpected extra pipeline result"}));
			conn->op_done_();
			return;
		}
		auto &wire = st->wire_results[st->next_wire_result++];
		if (status == PGRES_PIPELINE_ABORTED) {
			auto _ = wire.dst->try_set_exception(
				std::make_exception_ptr(PgError{wire.label + ": pipeline command aborted", {}, status}));
			continue;
		}
		if (status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE || status == PGRES_NONFATAL_ERROR) {
			auto err = detail::from_result(next.get(), wire.label);
			auto _ = wire.dst->try_set_exception(std::make_exception_ptr(std::move(err)));
			continue;
		}
		if (wire.kind == WireResultKind::prepare) {
			if (status == PGRES_COMMAND_OK) {
				conn->prepared_names_.insert(wire.prepared_name);
			} else {
				auto _ = wire.dst->try_set_exception(
					std::make_exception_ptr(PgError{wire.label + ": std::unexpected result status", {}, status}));
			}
			continue;
		}
		if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
			auto _ = wire.dst->try_set_value(root::Success<Result>{Result{std::move(next)}});
		} else {
			auto _ = wire.dst->try_set_exception(
				std::make_exception_ptr(PgError{wire.label + ": std::unexpected result status", {}, status}));
		}
	}
	int const fd = ::PQsocket(conn->conn_.get());
	auto self = conn;
	bool const armed = conn->reader_->poll_add_oneshot(fd, POLLIN, [self, st](IoResult r) mutable {
		if (self->closed_) {
			fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: connection closed"}));
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			fail_wire_sync_(st, std::make_exception_ptr(PgError{std::format("conflux.pg: poll read: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		drive_wire_consume_loop_(st);
	});
	if (!armed) {
		fail_wire_sync_(st, std::make_exception_ptr(PgError{"conflux.pg: io_uring SQ full"}));
		conn->op_done_();
	}
#else
	(void)st;
#endif
}
void Pipeline::fail_wire_sync_(
	std::shared_ptr<SyncState> const &st,
	std::exception_ptr error) noexcept {
	if (!st) {
		return;
	}
	for (; st->next_wire_result < st->wire_results.size(); ++st->next_wire_result) {
		auto _ = st->wire_results[st->next_wire_result].dst->try_set_exception(error);
	}
	while (!st->batch.empty()) {
		auto item = std::move(st->batch.front());
		st->batch.pop_front();
		auto _ = item.dst->try_set_exception(error);
	}
	if (st->entered_pipeline && st->pipe && st->pipe->conn && st->pipe->conn->conn_) {
#if defined(LIBPQ_HAS_PIPELINING)
		if (::PQexitPipelineMode(st->pipe->conn->conn_.get()) == 0) {
			st->pipe->conn->closed_ = true;
			st->pipe->conn->conn_.reset();
		}
#endif
		st->entered_pipeline = false;
	}
	auto _ = st->done->try_set_exception(error);
	finish_sync_(st->pipe, false);
}
void Pipeline::finish_sync_(
	std::shared_ptr<State> const &pipe,
	bool success) noexcept {
	if (!pipe) {
		return;
	}
	pipe->syncing = false;
	pipe->active_sync.reset();
	if (pipe->closed && pipe->conn) {
		pipe->conn->pipeline_mode_ = false;
	}
	if (!success) {
		while (!pipe->pending.empty()) {
			auto dst = std::move(pipe->pending.front().dst);
			pipe->pending.pop_front();
			auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline sync failed"}));
		}
	}
}
void Pipeline::close_() noexcept {
	auto const st = state_;
	if (!st || st->closed) {
		return;
	}
	st->closed = true;
	while (!st->pending.empty()) {
		auto dst = std::move(st->pending.front().dst);
		st->pending.pop_front();
		auto _ = dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
	}
	if (st->active_sync) {
		while (!st->active_sync->batch.empty()) {
			auto rem = std::move(st->active_sync->batch.front());
			st->active_sync->batch.pop_front();
			auto _ = rem.dst->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		}
		for (std::size_t i = st->active_sync->next_wire_result; i < st->active_sync->wire_results.size(); ++i) {
			auto _ = st->active_sync->wire_results[i].dst->try_set_exception(
				std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
		}
		auto _ = st->active_sync->done->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pipeline closed"}));
	}
	if (!st->conn || !st->conn->conn_ || st->active_sync) {
		return;
	}
	st->conn->pipeline_mode_ = false;
}
root::Task<std::shared_ptr<std::string const>> QueryCache::load_async(
	std::string_view name) {
	auto [task, raw_src] =
		root::make_task_source<std::shared_ptr<std::string const>>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<std::shared_ptr<std::string const>>>(std::move(raw_src));
	if (!detail::valid_query_name(name)) {
		auto _ =
			shared_src->try_set_exception(std::make_exception_ptr(std::invalid_argument{std::format("invalid query name: {}", name)}));
		return std::move(task);
	}
	{
		std::shared_lock const lk{mtx_};
		if (auto it = cache_.find(name); it != cache_.end()) {
			auto _ = shared_src->try_set_value(root::Success<std::shared_ptr<std::string const>>{it->second});
			return std::move(task);
		}
	}
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		try {
			auto _ = shared_src->try_set_value(root::Success<std::shared_ptr<std::string const>>{load_or_throw(name)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		return std::move(task);
	}
	auto name_owned = std::string{name};
	auto path = (root_ / (name_owned + ".psql")).string();
	[](FileReader *reader,
	   std::shared_ptr<root::TaskSource<std::shared_ptr<std::string const>>> shared_src,
	   std::string name_owned,
	   QueryCache *self,
	   root::Task<FileHandle> open_task) mutable -> root::Task<void> {
		try {
			auto fh = co_await std::move(open_task);
			auto fh_sp = std::make_shared<FileHandle>(std::move(fh));
			auto const st = co_await reader->async_stat(*fh_sp);
			if (st.size == 0) {
				auto sp = std::make_shared<std::string const>();
				std::scoped_lock const lk{self->mtx_};
				auto [it, ok] = self->cache_.try_emplace(name_owned, sp);
				auto _ = shared_src->try_set_value(root::Success<std::shared_ptr<std::string const>>{it->second});
				co_return;
			}
			auto buf = std::make_shared<std::string>(st.size, '\0');
			auto raw_span = std::span<std::byte>{reinterpret_cast<std::byte *>(buf->data()), buf->size()};
			auto const n = co_await reader->read_into(*fh_sp, 0, raw_span);
			buf->resize(n);
			auto sp = std::make_shared<std::string const>(std::move(*buf));
			std::scoped_lock const lk{self->mtx_};
			auto [it, ok] = self->cache_.try_emplace(name_owned, sp);
			auto _ = shared_src->try_set_value(root::Success<std::shared_ptr<std::string const>>{it->second});
		} catch (Cancelled const &) {
			auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	}(reader, shared_src, name_owned, this, reader->async_open(AT_FDCWD, std::move(path), O_RDONLY))
														.detach();
	return std::move(task);
}

} // namespace conflux::pg
