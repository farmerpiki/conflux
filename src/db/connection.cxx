module;
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <libpq-fe.h>
#include <poll.h>

export module conflux.db.connection;

import std;
import std.compat;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db.types;
import conflux.db.params;
import conflux.db.result;

using namespace std;

namespace conflux::db {

namespace detail {

struct ConnectState;

inline void rstrip_nl_(
	string_view &s) noexcept {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
		s.remove_suffix(1);
	}
}

inline void rstrip_nl_(
	string &s) noexcept {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
		s.pop_back();
	}
}

inline PgError from_conn(
	PGconn *c,
	string_view label) {
	char const *raw = c != nullptr ? ::PQerrorMessage(c) : "";
	string_view trimmed{raw != nullptr ? raw : ""};
	rstrip_nl_(trimmed);
	string sqlstate;
	if (c != nullptr && ::PQstatus(c) == CONNECTION_BAD) {
		sqlstate = "08006";
	}
	return PgError{format("{}: {}", label, trimmed), move(sqlstate)};
}

inline PgError from_result(
	PGresult *r,
	string_view label) {
	auto const fetch = [r](int code) -> string {
		char const *p = ::PQresultErrorField(r, code);
		return p != nullptr ? string{p} : string{};
	};
	auto state = fetch(PG_DIAG_SQLSTATE);
	auto primary = fetch(PG_DIAG_MESSAGE_PRIMARY);
	if (primary.empty()) {
		char const *p = ::PQresultErrorMessage(r);
		primary = p != nullptr ? string{p} : string{};
	}
	rstrip_nl_(primary);
	PgError e{format("{}: {}", label, primary), move(state), ::PQresultStatus(r)};
	e.detail = fetch(PG_DIAG_MESSAGE_DETAIL);
	e.hint = fetch(PG_DIAG_MESSAGE_HINT);
	e.where = fetch(PG_DIAG_CONTEXT);
	return e;
}

inline void install_sigpipe_ignore() noexcept {
	static once_flag flag;
	call_once(flag, [] { (void)::signal(SIGPIPE, SIG_IGN); });
}

inline bool valid_query_name(
	string_view name) noexcept {
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

inline uint64_t fnv1a64(
	string_view s) noexcept {
	uint64_t h = 14695981039346656037ULL;
	for (char const c: s) {
		h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
		h *= 1099511628211ULL;
	}
	return h;
}

template<class T>
[[nodiscard]] auto flow_to_root_task(
	Flow<T> flow) -> conflux::work::root::Task<T> {
	using namespace conflux::work::root;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = false});
	auto shared_src = make_shared<TaskSource<T>>(move(src));
	spawn(
		move(flow)
		| then([shared_src](T value) mutable { (void)shared_src->commit_success(Success<T>{move(value)}); })
		| on_error([shared_src](exception_ptr const &ex) mutable { (void)shared_src->commit_failure(ex); })
		| on_cancel([shared_src]() mutable { (void)shared_src->commit_cancelled(CancelReason::requested); }));
	return move(task);
}

template<>
[[nodiscard]] auto flow_to_root_task<void>(
	Flow<void> flow) -> conflux::work::root::Task<void> {
	using namespace conflux::work::root;
	auto [task, src] = make_task_source<void>(SubmitOptions{.enable_cancellation = false});
	auto shared_src = make_shared<TaskSource<void>>(move(src));
	spawn(
		move(flow)
		| then([shared_src]() mutable { (void)shared_src->commit_success(Success<void>{}); })
		| on_error([shared_src](exception_ptr const &ex) mutable { (void)shared_src->commit_failure(ex); })
		| on_cancel([shared_src]() mutable { (void)shared_src->commit_cancelled(CancelReason::requested); }));
	return move(task);
}

} // namespace detail

export struct ConnectParams {
	string conninfo{};
	chrono::milliseconds connect_deadline{chrono::seconds{15}};
};

export struct QueryOptions {
	optional<chrono::milliseconds> deadline{};
};

export class StatementCache {
public:
	struct Entry {
		string name;
		shared_ptr<string const> sql;
		vector<Oid> param_types;
	};

	static string stable_name(
		string_view sql) {
		static constexpr string_view kAlphabet{"abcdefghijklmnopqrstuvwxyz234567"};
		uint64_t const h = detail::fnv1a64(sql);
		string name;
		name.reserve(15);
		name += "p_";
		for (int shift = 59; shift >= 4; shift -= 5) {
			name += kAlphabet[(h >> static_cast<uint32_t>(shift)) & 0x1fU];
		}
		name += kAlphabet[(h & 0xfU) << 1U]; // last 4 bits, pad LSB
		return name;
	}

	[[nodiscard]] shared_ptr<Entry const> get(
		shared_ptr<string const> sql,
		vector<Oid> param_types = {}) {
		string const name = stable_name(*sql);
		{
			shared_lock const lk{mu_};
			if (auto it = cache_.find(name); it != cache_.end()) {
				return it->second;
			}
		}
		auto entry = make_shared<Entry const>(Entry{name, move(sql), move(param_types)});
		scoped_lock const lk{mu_};
		auto [it, _] = cache_.try_emplace(name, move(entry));
		return it->second;
	}

	[[nodiscard]] shared_ptr<Entry const> get(
		string_view sql,
		vector<Oid> param_types = {}) {
		return get(make_shared<string const>(sql), move(param_types));
	}

	void clear() noexcept {
		scoped_lock const lk{mu_};
		cache_.clear();
	}

private:
	mutable shared_mutex mu_{};
	mutable unordered_map<string, shared_ptr<Entry const>> cache_{};
};

export class Connection : public enable_shared_from_this<Connection> {
public:
	Connection(Connection const &) = delete;
	Connection &operator =(Connection const &) = delete;
	Connection(Connection &&) = delete;
	Connection &operator =(Connection &&) = delete;

	~Connection() { close(); }

	static conflux::work::root::Task<shared_ptr<Connection>> connect(ConnectParams const &params);

	conflux::work::root::Task<Result> query(string_view sql, Params params = {});
	conflux::work::root::Task<Result> query(shared_ptr<string const> sql, Params params = {});

	conflux::work::root::Task<void> prepare(string_view name, string_view sql, span<Oid const> param_types = {});
	conflux::work::root::Task<void>
	prepare(string_view name, shared_ptr<string const> sql, span<Oid const> param_types = {});

	conflux::work::root::Task<Result> exec_prepared(string_view name, Params params = {});
	conflux::work::root::Task<Result>
	exec_cached(shared_ptr<StatementCache::Entry const> const &stmt, Params params = {});

	conflux::work::root::Task<void> cancel_inflight(WorkPool &cancel_pool);
	conflux::work::root::Task<void> cancel_inflight();
	Flow<class Pipeline> pipeline();

	conflux::work::root::Task<Result> query(string_view sql, Params params, QueryOptions opts);

	[[nodiscard]] bool ok() const noexcept { return conn_ && ::PQstatus(conn_.get()) == CONNECTION_OK; }

	[[nodiscard]] string last_error() const {
		if (!conn_) {
			return {};
		}
		char const *p = ::PQerrorMessage(conn_.get());
		return p != nullptr ? string{p} : string{};
	}

	[[nodiscard]] PGconn *raw() const noexcept { return conn_.get(); }
	[[nodiscard]] int backend_pid() const noexcept { return conn_ ? ::PQbackendPID(conn_.get()) : 0; }
	[[nodiscard]] int server_version() const noexcept { return conn_ ? ::PQserverVersion(conn_.get()) : 0; }

	void close() noexcept;

private:
	static Flow<shared_ptr<Connection>> connect_flow(ConnectParams const &params);
	Flow<Result> query_flow(string_view sql, Params params = {});
	Flow<Result> query_flow(shared_ptr<string const> sql, Params params = {});
	Flow<void> prepare_flow(string_view name, string_view sql, span<Oid const> param_types = {});
	Flow<void> prepare_flow(string_view name, shared_ptr<string const> sql, span<Oid const> param_types = {});
	Flow<Result> exec_prepared_flow(string_view name, Params params = {});
	Flow<Result> exec_cached_flow(shared_ptr<StatementCache::Entry const> const &stmt, Params params = {});
	Flow<void> cancel_inflight_flow(WorkPool &cancel_pool);
	Flow<void> cancel_inflight_flow();
	Flow<Result> query_flow(string_view sql, Params params, QueryOptions opts);

	Connection(
		PGConnPtr conn,
		FileReader *reader) noexcept
		: conn_{move(conn)}
		, reader_{reader}
		, owner_{this_thread::get_id()} {}

	void enqueue_job_(function<void()> job);
	void start_next_();

	void run_query_(string const &sql, Params const &params, FlowSource<Result> dst);
	void run_prepare_(string const &name, string const &sql, vector<Oid> oids, FlowSource<void> dst);
	void run_exec_prepared_(string const &name, Params const &params, FlowSource<Result> dst);

	template<class T>
	void after_send_drive_flush_(FlowSource<T> dst, shared_ptr<Result> partial, string const &label);
	template<class T>
	void drive_consume_loop_(FlowSource<T> dst, shared_ptr<Result> partial, string const &label);

	template<class T>
	void reject_(
		FlowSource<T> &dst,
		string const &label) {
		auto err = detail::from_conn(conn_.get(), label);
		dst.reject(make_exception_ptr(move(err)));
	}

	void op_done_();

	PGConnPtr conn_;
	FileReader *reader_{nullptr};
	thread::id owner_{};
	bool closed_{false};
	bool in_flight_{false};
	deque<function<void()>> queue_{};
	unordered_set<string> prepared_names_{};
	bool pipeline_mode_{false};

	friend struct detail::ConnectState;
	friend class Pipeline;
};

export class Pipeline {
public:
	Pipeline() = default;
	Pipeline(
		shared_ptr<Connection> conn) noexcept
		: conn_{move(conn)} {}

	Pipeline(Pipeline const &) = delete;
	Pipeline &operator =(Pipeline const &) = delete;
	Pipeline(Pipeline &&) noexcept = default;
	Pipeline &operator =(Pipeline &&) noexcept = default;

	~Pipeline() { close_(); }

	// Contract:
	// - must run on the owning ring lane thread
	// - pipeline must be active and not currently syncing
	// - query result becomes available only after sync() reaches PGRES_PIPELINE_SYNC
	conflux::work::root::Task<Result> query(string_view sql, Params params = {});
	// Contract:
	// - must run on the owning ring lane thread
	// - executes queued statements in-order through Connection::query
	// - this is a logical batching barrier (not libpq wire pipeline mode yet)
	conflux::work::root::Task<Result> exec_cached(shared_ptr<StatementCache::Entry const> stmt, Params params = {});
	conflux::work::root::Task<void> sync();

private:
	Flow<Result> query_flow(string_view sql, Params params);
	Flow<Result> exec_cached_flow(shared_ptr<StatementCache::Entry const> stmt, Params params);
	Flow<void> sync_flow();
	struct PendingQuery {
		FlowSource<Result> dst;
		string sql;
		Params params;
	};
	struct SyncState {
		deque<PendingQuery> batch;
		FlowSource<void> done;
	};

	void close_() noexcept;
	void sync_next_(shared_ptr<SyncState> st);
	void finish_sync_(bool success) noexcept;

	shared_ptr<Connection> conn_{};
	bool closed_{false};
	deque<PendingQuery> pending_{};
	bool syncing_{false};
};

export class QueryCache {
	struct TransparentHash {
		using is_transparent = void;
		size_t operator ()(
			string_view s) const noexcept {
			return hash<string_view>{}(s);
		}
		size_t operator ()(
			string const &s) const noexcept {
			return hash<string_view>{}(s);
		}
	};

	filesystem::path root_{};
	mutable shared_mutex mtx_{};
	mutable unordered_map<string, shared_ptr<string const>, TransparentHash, equal_to<>> cache_{};

public:
	explicit QueryCache(
		filesystem::path root)
		: root_{move(root)} {}

	[[nodiscard]] shared_ptr<string const> lookup(
		string_view name) const noexcept {
		shared_lock const lk{mtx_};
		auto it = cache_.find(name);
		return it != cache_.end() ? it->second : nullptr;
	}

	[[nodiscard]] shared_ptr<string const> load_or_throw(
		string_view name) const {
		if (!detail::valid_query_name(name)) {
			throw invalid_argument{format("invalid query name: {}", name)};
		}
		{
			shared_lock const lk{mtx_};
			if (auto it = cache_.find(name); it != cache_.end()) {
				return it->second;
			}
		}
		auto path = root_ / (string{name} + ".psql");
		ifstream in{path};
		if (!in) {
			throw filesystem::filesystem_error{
				"query file open failed",
				path,
				error_code{errno, generic_category()}
            };
		}
		string contents{istreambuf_iterator<char>{in}, istreambuf_iterator<char>{}};
		auto sp = make_shared<string const>(move(contents));
		scoped_lock const lk{mtx_};
		auto [it, _] = cache_.try_emplace(string{name}, sp);
		return it->second;
	}

	[[nodiscard]] conflux::work::root::Task<shared_ptr<string const>> load_async(string_view name);

	void clear() noexcept {
		scoped_lock const lk{mtx_};
		cache_.clear();
	}

private:
	[[nodiscard]] Flow<shared_ptr<string const>> load_flow(string_view name);
};

// ===========================================================================
// Implementation
// ===========================================================================

namespace detail {

struct ConnectState : enable_shared_from_this<ConnectState> {
	PGConnPtr conn{};
	FileReader *reader{nullptr};
	FlowSource<shared_ptr<Connection>> dst{};
	chrono::steady_clock::time_point deadline{};

	void start() {
		install_sigpipe_ignore();
		drive(/*initial=*/true);
	}

	void drive(
		bool initial) {
		if (chrono::steady_clock::now() > deadline) {
			dst.reject(make_exception_ptr(PgError{"conflux.db: connect deadline exceeded", "08001"}));
			return;
		}
		PostgresPollingStatusType const status = initial ? PGRES_POLLING_WRITING : ::PQconnectPoll(conn.get());
		while (true) {
			if (::PQstatus(conn.get()) == CONNECTION_BAD && !initial) {
				dst.reject(make_exception_ptr(from_conn(conn.get(), "conflux.db: connect")));
				return;
			}
			if (status == PGRES_POLLING_FAILED) {
				dst.reject(make_exception_ptr(from_conn(conn.get(), "conflux.db: connect")));
				return;
			}
			if (status == PGRES_POLLING_OK) {
				if (::PQsetnonblocking(conn.get(), 1) != 0) {
					dst.reject(make_exception_ptr(from_conn(conn.get(), "conflux.db: PQsetnonblocking")));
					return;
				}
				auto c = shared_ptr<Connection>(new Connection{move(conn), reader});
				// P11b: pin client_encoding to UTF-8 before publishing.
				auto outer = dst;
				auto conn_sp = c;
				spawn(
					conn_sp->query_flow(string_view{"SET client_encoding = 'UTF8'"})
					| then([outer, conn_sp](Result) mutable {
						  char const *enc = ::PQparameterStatus(conn_sp->raw(), "client_encoding");
						  if (enc == nullptr || string_view{enc} != string_view{"UTF8"}) {
							  outer.reject(
								  make_exception_ptr(PgError{"conflux.db: client_encoding must be UTF8", "22021"}));
							  return;
						  }
						  outer.resolve(move(conn_sp));
					  })
					| on_error([outer](exception_ptr const &ep) mutable { outer.reject(ep); })
					| on_cancel([outer]() mutable {
						  outer.reject(make_exception_ptr(PgError{"conflux.db: connect cancelled"}));
					  }));
				return;
			}
			short mask = 0;
			if (status == PGRES_POLLING_READING) {
				mask = POLLIN;
			} else if (status == PGRES_POLLING_WRITING) {
				mask = POLLOUT;
			} else {
				dst.reject(make_exception_ptr(PgError{"conflux.db: unexpected polling status"}));
				return;
			}
			int const fd = ::PQsocket(conn.get());
			if (fd < 0) {
				dst.reject(make_exception_ptr(from_conn(conn.get(), "conflux.db: PQsocket")));
				return;
			}
			auto self = shared_from_this();
			bool const armed = reader->poll_add_oneshot(fd, mask, [self](IoResult r) {
				if (r.res < 0) {
					self->dst.reject(make_exception_ptr(PgError{format("conflux.db: poll: {}", strerror(-r.res))}));
					return;
				}
				self->drive(false);
			});
			if (!armed) {
				dst.reject(make_exception_ptr(PgError{"conflux.db: io_uring SQ full"}));
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
using PGcancelPtr = unique_ptr<PGcancel, PGcancelDeleter>;

inline WorkPool &cancel_pool() {
	static WorkPool pool{WorkPoolOptions{.threads = 1}};
	return pool;
}

} // namespace detail

Flow<shared_ptr<Connection>> Connection::connect_flow(
	ConnectParams const &params) {
	FlowSource<shared_ptr<Connection>> const src;
	auto flow = src.flow();
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		src.reject(make_exception_ptr(PgError{"conflux.db: no current FileReader (not on a ring lane)"}));
		return flow;
	}
	if (::PQisthreadsafe() == 0) {
		src.reject(make_exception_ptr(PgError{"conflux.db: libpq built without thread safety"}));
		return flow;
	}
	PGConnPtr conn{::PQconnectStart(params.conninfo.c_str())};
	if (!conn || ::PQstatus(conn.get()) == CONNECTION_BAD) {
		src.reject(make_exception_ptr(detail::from_conn(conn.get(), "conflux.db: PQconnectStart")));
		return flow;
	}
	auto st = make_shared<detail::ConnectState>();
	st->conn = move(conn);
	st->reader = reader;
	st->dst = src;
	st->deadline = chrono::steady_clock::now() + params.connect_deadline;
	st->start();
	return flow;
}

conflux::work::root::Task<shared_ptr<Connection>> Connection::connect(
	ConnectParams const &params) {
	return detail::flow_to_root_task(Connection::connect_flow(params));
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
	function<void()> job) {
	if (in_flight_) {
		queue_.push_back(move(job));
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
	auto job = move(queue_.front());
	queue_.pop_front();
	job();
}

void Connection::op_done_() {
	start_next_();
}

Flow<Result> Connection::query_flow(
	string_view sql,
	Params params) {
	FlowSource<Result> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: query off owner thread"}));
		return flow;
	}
	auto self = shared_from_this();
	enqueue_job_([self, sql_owned = string{sql}, params = move(params), src]() mutable {
		self->run_query_(sql_owned, params, src);
	});
	return flow;
}

conflux::work::root::Task<Result> Connection::query(
	string_view sql,
	Params params) {
	return detail::flow_to_root_task(query_flow(sql, move(params)));
}

Flow<Result> Connection::query_flow(
	shared_ptr<string const> sql,
	Params params) {
	FlowSource<Result> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (!sql) {
		src.reject(make_exception_ptr(PgError{"conflux.db: null SQL handle"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: query off owner thread"}));
		return flow;
	}
	auto self = shared_from_this();
	enqueue_job_(
		[self, sql = move(sql), params = move(params), src]() mutable { self->run_query_(*sql, params, src); });
	return flow;
}

conflux::work::root::Task<Result> Connection::query(
	shared_ptr<string const> sql,
	Params params) {
	return detail::flow_to_root_task(query_flow(move(sql), move(params)));
}

void Connection::run_query_(
	string const &sql,
	Params const &params,
	FlowSource<Result> dst) {
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
		reject_(dst, "conflux.db: PQsendQueryParams");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, make_shared<Result>(), "conflux.db: query");
}

Flow<void> Connection::prepare_flow(
	string_view name,
	string_view sql,
	span<Oid const> param_types) {
	FlowSource<void> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: prepare off owner thread"}));
		return flow;
	}
	auto self = shared_from_this();
	enqueue_job_([self,
				  name_owned = string{name},
				  sql_owned = string{sql},
				  oids = vector<Oid>{param_types.begin(), param_types.end()},
				  src]() mutable { self->run_prepare_(name_owned, sql_owned, move(oids), src); });
	return flow;
}

conflux::work::root::Task<void> Connection::prepare(
	string_view name,
	string_view sql,
	span<Oid const> param_types) {
	return detail::flow_to_root_task(prepare_flow(name, sql, param_types));
}

Flow<void> Connection::prepare_flow(
	string_view name,
	shared_ptr<string const> sql,
	span<Oid const> param_types) {
	FlowSource<void> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (!sql) {
		src.reject(make_exception_ptr(PgError{"conflux.db: null SQL handle"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: prepare off owner thread"}));
		return flow;
	}
	auto self = shared_from_this();
	enqueue_job_([self,
				  name_owned = string{name},
				  sql = move(sql),
				  oids = vector<Oid>{param_types.begin(), param_types.end()},
				  src]() mutable { self->run_prepare_(name_owned, *sql, move(oids), src); });
	return flow;
}

conflux::work::root::Task<void> Connection::prepare(
	string_view name,
	shared_ptr<string const> sql,
	span<Oid const> param_types) {
	return detail::flow_to_root_task(prepare_flow(name, move(sql), param_types));
}

void Connection::run_prepare_(
	string const &name,
	string const &sql,
	vector<Oid> oids,
	FlowSource<void> dst) {
	int const send = ::PQsendPrepare(
		conn_.get(),
		name.c_str(),
		sql.c_str(),
		static_cast<int>(oids.size()),
		oids.empty() ? nullptr : oids.data());
	if (send == 0) {
		reject_(dst, "conflux.db: PQsendPrepare");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, make_shared<Result>(), "conflux.db: prepare");
}

Flow<Result> Connection::exec_prepared_flow(
	string_view name,
	Params params) {
	FlowSource<Result> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: exec_prepared off owner thread"}));
		return flow;
	}
	auto self = shared_from_this();
	enqueue_job_([self, name_owned = string{name}, params = move(params), src]() mutable {
		self->run_exec_prepared_(name_owned, params, src);
	});
	return flow;
}

conflux::work::root::Task<Result> Connection::exec_prepared(
	string_view name,
	Params params) {
	return detail::flow_to_root_task(exec_prepared_flow(name, move(params)));
}

void Connection::run_exec_prepared_(
	string const &name,
	Params const &params,
	FlowSource<Result> dst) {
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
		reject_(dst, "conflux.db: PQsendQueryPrepared");
		op_done_();
		return;
	}
	after_send_drive_flush_(dst, make_shared<Result>(), "conflux.db: query");
}

Flow<Result> Connection::exec_cached_flow(
	shared_ptr<StatementCache::Entry const> const &stmt,
	Params params) {
	if (prepared_names_.contains(stmt->name)) {
		return exec_prepared_flow(stmt->name, move(params));
	}
	FlowSource<Result> const src;
	auto flow = src.flow();
	auto self = shared_from_this();
	Params params_err{params};
	spawn(
		prepare_flow(stmt->name, stmt->sql, stmt->param_types)
		| then([self, stmt, params = move(params), src]() mutable {
			  self->prepared_names_.insert(stmt->name);
			  spawn(
				  self->exec_prepared_flow(stmt->name, move(params))
				  | then([src](Result r) mutable { src.resolve(move(r)); })
				  | on_error([src](exception_ptr const &ex) mutable { src.reject(ex); })
				  | on_cancel([src]() mutable { src.cancel(); }));
		  })
		| on_error([self, stmt, params = move(params_err), src](exception_ptr const &ex) mutable {
			  bool duplicate = false;
			  try {
				  rethrow_exception(ex);
			  } catch (PgError const &e) {
				  duplicate = (e.sqlstate == "42P05");
			  }
			  // NOLINTNEXTLINE(bugprone-empty-catch)
			  catch (...) {}
			  if (duplicate) {
				  self->prepared_names_.insert(stmt->name);
				  spawn(
					  self->exec_prepared_flow(stmt->name, move(params))
					  | then([src](Result r) mutable { src.resolve(move(r)); })
					  | on_error([src](exception_ptr const &ex2) mutable { src.reject(ex2); })
					  | on_cancel([src]() mutable { src.cancel(); }));
			  } else {
				  src.reject(ex);
			  }
		  })
		| on_cancel([src]() mutable { src.cancel(); }));
	return flow;
}

conflux::work::root::Task<Result> Connection::exec_cached(
	shared_ptr<StatementCache::Entry const> const &stmt,
	Params params) {
	return detail::flow_to_root_task(exec_cached_flow(stmt, move(params)));
}

template<class T>
void Connection::after_send_drive_flush_(
	FlowSource<T> dst,
	shared_ptr<Result> partial,
	string const &label) {
	int const f = ::PQflush(conn_.get());
	if (f < 0) {
		reject_(dst, "conflux.db: PQflush");
		op_done_();
		return;
	}
	if (f == 0) {
		drive_consume_loop_(move(dst), move(partial), label);
		return;
	}
	int const fd = ::PQsocket(conn_.get());
	auto self = shared_from_this();
	// NOLINTNEXTLINE(bugprone-exception-escape) — poll callback; any throw would terminate, treated as fatal.
	bool const armed = reader_->poll_add_oneshot(fd, POLLOUT, [self, dst, partial, label](IoResult r) mutable {
		if (self->closed_) {
			dst.cancel();
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			dst.reject(make_exception_ptr(PgError{format("conflux.db: poll write: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		self->after_send_drive_flush_(move(dst), move(partial), label);
	});
	if (!armed) {
		dst.reject(make_exception_ptr(PgError{"conflux.db: io_uring SQ full"}));
		op_done_();
	}
}

// Contract: callers issue a single-statement query via PQsendQueryParams/
// PQsendPrepare/PQsendQueryPrepared, so PQgetResult produces at most one
// non-null result followed by nullptr. Multi-statement queries keep only
// the last result.
template<class T>
void Connection::drive_consume_loop_(
	FlowSource<T> dst,
	shared_ptr<Result> partial,
	string const &label) {
	if (::PQconsumeInput(conn_.get()) == 0) {
		reject_(dst, "conflux.db: PQconsumeInput");
		op_done_();
		return;
	}
	while (::PQisBusy(conn_.get()) == 0) {
		PGResultPtr next{::PQgetResult(conn_.get())};
		if (!next) {
			if (partial && *partial && partial->ok()) {
				if constexpr (is_void_v<T>) {
					dst.resolve();
				} else {
					dst.resolve(move(*partial));
				}
			} else if (partial && *partial) {
				auto err = detail::from_result(partial->raw(), label);
				dst.reject(make_exception_ptr(move(err)));
			} else {
				dst.reject(make_exception_ptr(PgError{"conflux.db: empty result"}));
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
			dst.reject(make_exception_ptr(move(err)));
			op_done_();
			return;
		}
		*partial = Result{move(next)};
	}
	int const fd = ::PQsocket(conn_.get());
	auto self = shared_from_this();
	// NOLINTNEXTLINE(bugprone-exception-escape) — poll callback; any throw would terminate, treated as fatal.
	bool const armed = reader_->poll_add_oneshot(fd, POLLIN, [self, dst, partial, label](IoResult r) mutable {
		if (self->closed_) {
			dst.cancel();
			self->op_done_();
			return;
		}
		if (r.res < 0) {
			dst.reject(make_exception_ptr(PgError{format("conflux.db: poll read: {}", strerror(-r.res))}));
			self->op_done_();
			return;
		}
		self->drive_consume_loop_(move(dst), move(partial), label);
	});
	if (!armed) {
		dst.reject(make_exception_ptr(PgError{"conflux.db: io_uring SQ full"}));
		op_done_();
	}
}

Flow<void> Connection::cancel_inflight_flow(
	WorkPool &wpool) {
	FlowSource<void> const src;
	auto flow = src.flow();
	if (!conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	detail::PGcancelPtr handle{::PQgetCancel(conn_.get())};
	if (!handle) {
		src.reject(make_exception_ptr(PgError{"conflux.db: PQgetCancel returned null"}));
		return flow;
	}
	auto shared_handle = shared_ptr<PGcancel>{handle.release(), detail::PGcancelDeleter{}};
	bool const queued = wpool.enqueue([shared_handle, src]() mutable {
		array<char, 256> buf{};
		int const ok = ::PQcancel(shared_handle.get(), buf.data(), static_cast<int>(buf.size()));
		if (ok == 0) {
			src.reject(make_exception_ptr(PgError{string{buf.data()}}));
		} else {
			src.resolve();
		}
	});
	if (!queued) {
		src.cancel();
	}
	return flow;
}

conflux::work::root::Task<void> Connection::cancel_inflight(
	WorkPool &cancel_pool) {
	return detail::flow_to_root_task(cancel_inflight_flow(cancel_pool));
}

Flow<void> Connection::cancel_inflight_flow() {
	return cancel_inflight_flow(detail::cancel_pool());
}

conflux::work::root::Task<void> Connection::cancel_inflight() {
	return detail::flow_to_root_task(cancel_inflight_flow());
}

Flow<Result> Connection::query_flow(
	string_view sql,
	Params params,
	QueryOptions opts) {
	if (!opts.deadline || opts.deadline->count() <= 0) {
		return query_flow(sql, move(params));
	}
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		return query_flow(sql, move(params));
	}
	FlowSource<Result> const src;
	auto flow = src.flow();
	auto self = shared_from_this();
	spawn(
		query_flow(sql, move(params))
		| then([src](Result r) mutable { src.resolve(move(r)); })
		| on_error([src](exception_ptr const &ex) mutable { src.reject(ex); })
		| on_cancel([src]() mutable { src.cancel(); }));
	auto const deadline = *opts.deadline;
	spawn(
		reader->timeout_async(deadline)
		| then([self, src]() mutable {
			  if (!src.armed()) {
				  return;
			  }
			  spawn(self->cancel_inflight_flow() | on_error([](exception_ptr const &) {}) | on_cancel([]() {}));
			  src.reject(make_exception_ptr(PgError{"conflux.db: query deadline exceeded", "57014"}));
		  })
		| on_error([](exception_ptr const &) {})
		| on_cancel([]() {}));
	return flow;
}

conflux::work::root::Task<Result> Connection::query(
	string_view sql,
	Params params,
	QueryOptions opts) {
	return detail::flow_to_root_task(query_flow(sql, move(params), opts));
}

Flow<Pipeline> Connection::pipeline() {
	FlowSource<Pipeline> const src;
	auto flow = src.flow();
	if (closed_ || !conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pipeline off owner thread"}));
		return flow;
	}
	if (pipeline_mode_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pipeline already active"}));
		return flow;
	}
	pipeline_mode_ = true;
	src.resolve(Pipeline{shared_from_this()});
	return flow;
}

Flow<Result> Pipeline::query_flow(
	string_view sql,
	Params params) {
	FlowSource<Result> const src;
	auto flow = src.flow();
	if (closed_ || !conn_ || !conn_->conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pipeline closed"}));
		return flow;
	}
	if (this_thread::get_id() != conn_->owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: query off owner thread"}));
		return flow;
	}
	if (syncing_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: query while sync in progress"}));
		return flow;
	}
	pending_.push_back(
		PendingQuery{
			.dst = src,
			.sql = string{sql},
			.params = move(params),
		});
	return flow;
}

conflux::work::root::Task<Result> Pipeline::query(
	string_view sql,
	Params params) {
	return detail::flow_to_root_task(query_flow(sql, move(params)));
}

Flow<Result> Pipeline::exec_cached_flow(
	shared_ptr<StatementCache::Entry const> stmt,
	Params params) {
	if (!stmt || !stmt->sql) {
		FlowSource<Result> const src;
		auto flow = src.flow();
		src.reject(make_exception_ptr(PgError{"conflux.db: null cached statement"}));
		return flow;
	}
	return query_flow(*stmt->sql, move(params));
}

conflux::work::root::Task<Result> Pipeline::exec_cached(
	shared_ptr<StatementCache::Entry const> stmt,
	Params params) {
	return detail::flow_to_root_task(exec_cached_flow(move(stmt), move(params)));
}

Flow<void> Pipeline::sync_flow() {
	FlowSource<void> const src;
	auto flow = src.flow();
	if (closed_ || !conn_ || !conn_->conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pipeline closed"}));
		return flow;
	}
	if (this_thread::get_id() != conn_->owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: sync off owner thread"}));
		return flow;
	}
	if (syncing_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: sync already in progress"}));
		return flow;
	}
	if (pending_.empty()) {
		src.resolve();
		return flow;
	}
	syncing_ = true;
	auto st = make_shared<SyncState>();
	st->batch = move(pending_);
	st->done = src;
	pending_.clear();
	sync_next_(st);
	return flow;
}

conflux::work::root::Task<void> Pipeline::sync() {
	return detail::flow_to_root_task(sync_flow());
}

void Pipeline::sync_next_(
	shared_ptr<SyncState> st) {
	if (st->batch.empty()) {
		st->done.resolve();
		finish_sync_(true);
		return;
	}
	auto item = move(st->batch.front());
	st->batch.pop_front();
	auto dst = item.dst;
	spawn(
		conn_->query_flow(item.sql, move(item.params))
		| then([this, st, dst](Result r) mutable {
			  dst.resolve(move(r));
			  sync_next_(st);
		  })
		| on_error([this, st, dst](exception_ptr const &ex) mutable {
			  dst.reject(ex);
			  while (!st->batch.empty()) {
				  auto rem = move(st->batch.front());
				  st->batch.pop_front();
				  rem.dst.reject(make_exception_ptr(PgError{"conflux.db: pipeline query"}));
			  }
			  st->done.resolve();
			  finish_sync_(true);
		  })
		| on_cancel([this, st, dst]() mutable {
			  dst.cancel();
			  while (!st->batch.empty()) {
				  auto rem = move(st->batch.front());
				  st->batch.pop_front();
				  rem.dst.cancel();
			  }
			  st->done.cancel();
			  finish_sync_(false);
		  }));
}

void Pipeline::finish_sync_(
	bool success) noexcept {
	syncing_ = false;
	if (!success) {
		while (!pending_.empty()) {
			auto dst = move(pending_.front().dst);
			pending_.pop_front();
			dst.reject(make_exception_ptr(PgError{"conflux.db: pipeline sync failed"}));
		}
	}
}

void Pipeline::close_() noexcept {
	if (closed_) {
		return;
	}
	closed_ = true;
	while (!pending_.empty()) {
		auto dst = move(pending_.front().dst);
		pending_.pop_front();
		dst.reject(make_exception_ptr(PgError{"conflux.db: pipeline closed"}));
	}
	if (!conn_ || !conn_->conn_) {
		return;
	}
	conn_->pipeline_mode_ = false;
}

Flow<shared_ptr<string const>> QueryCache::load_flow(
	string_view name) {
	FlowSource<shared_ptr<string const>> const src;
	auto flow = src.flow();
	if (!detail::valid_query_name(name)) {
		src.reject(make_exception_ptr(invalid_argument{format("invalid query name: {}", name)}));
		return flow;
	}
	{
		shared_lock const lk{mtx_};
		if (auto it = cache_.find(name); it != cache_.end()) {
			src.resolve(it->second);
			return flow;
		}
	}
	auto *reader = current_file_reader();
	if (reader == nullptr) {
		try {
			src.resolve(load_or_throw(name));
		} catch (...) { src.reject(current_exception()); }
		return flow;
	}
	auto name_owned = string{name};
	auto path = (root_ / (name_owned + ".psql")).string();
	spawn(
		reader->open_async(AT_FDCWD, move(path), O_RDONLY)
		| then([reader, src, name_owned, this](FileHandle fh) mutable {
			  auto fh_sp = make_shared<FileHandle>(move(fh));
			  spawn(
				  reader->stat_async(*fh_sp)
				  | then([reader, src, name_owned, fh_sp, this](FileStat st) mutable {
						if (st.size == 0) {
							auto sp = make_shared<string const>();
							scoped_lock const lk{mtx_};
							auto [it, ok] = cache_.try_emplace(name_owned, sp);
							src.resolve(it->second);
							return;
						}
						auto buf = make_shared<string>(st.size, '\0');
						auto raw_span = span<byte>{reinterpret_cast<byte *>(buf->data()), buf->size()};
						spawn(
							reader->read_into(*fh_sp, 0, raw_span)
							| then([src, name_owned, buf, fh_sp, this](size_t n) mutable {
								  buf->resize(n);
								  auto sp = make_shared<string const>(move(*buf));
								  scoped_lock const lk{mtx_};
								  auto [it, ok] = cache_.try_emplace(name_owned, sp);
								  src.resolve(it->second);
							  })
							| on_error([src](exception_ptr const &ep) mutable { src.reject(ep); })
							| on_cancel([src]() mutable { src.cancel(); }));
					})
				  | on_error([src](exception_ptr const &ep) mutable { src.reject(ep); })
				  | on_cancel([src]() mutable { src.cancel(); }));
		  })
		| on_error([src](exception_ptr const &ep) mutable { src.reject(ep); })
		| on_cancel([src]() mutable { src.cancel(); }));
	return flow;
}

conflux::work::root::Task<shared_ptr<string const>> QueryCache::load_async(
	string_view name) {
	return detail::flow_to_root_task(load_flow(name));
}

} // namespace conflux::db
