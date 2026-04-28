module;

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <libpq-fe.h>
#include <poll.h>

export module conflux.db;

import std;
import conflux.types;
import std.compat;
import conflux.work;
import conflux.file_io;

using namespace std;

namespace conflux::db {

namespace detail {

struct ConnectState;

} // namespace detail

export struct PGConnDeleter {
	void operator ()(
		PGconn *c) const noexcept {
		if (c != nullptr) {
			::PQfinish(c);
		}
	}
};

export struct PGResultDeleter {
	void operator ()(
		PGresult *r) const noexcept {
		if (r != nullptr) {
			::PQclear(r);
		}
	}
};

export using PGConnPtr = unique_ptr<PGconn, PGConnDeleter>;
export using PGResultPtr = unique_ptr<PGresult, PGResultDeleter>;

export struct PgError final : runtime_error {
	string sqlstate{};
	string detail{};
	string hint{};
	string where{};
	ExecStatusType status{PGRES_FATAL_ERROR};

	explicit PgError(
		string const &msg,
		string state = {},
		ExecStatusType st = PGRES_FATAL_ERROR)
		: runtime_error{msg}
		, sqlstate{move(state)}
		, status{st} {}

	[[nodiscard]] bool is_unique_violation() const noexcept { return sqlstate == "23505"; }
	[[nodiscard]] bool is_serialization() const noexcept { return sqlstate == "40001"; }
	[[nodiscard]] bool is_deadlock() const noexcept { return sqlstate == "40P01"; }
	[[nodiscard]] bool is_connection_lost() const noexcept {
		return sqlstate.size() >= 2 && sqlstate[0] == '0' && sqlstate[1] == '8';
	}
};

export struct TxOptions {
	enum class Iso : uint8_t {
		ReadCommitted,
		RepeatableRead,
		Serializable,
	};
	Iso iso{Iso::ReadCommitted};
	bool read_only{false};
	bool deferrable{false};
	int max_retries{3};
};

namespace detail {

template<class T>
struct awaitable_value;

template<class T>
struct awaitable_value<Task<T>> {
	using type = T;
};

template<class T>
using awaitable_value_t = typename awaitable_value<T>::type;

inline string begin_sql(
	TxOptions const &opt) {
	string s = "BEGIN";
	switch (opt.iso) {
	case TxOptions::Iso::RepeatableRead: s += " ISOLATION LEVEL REPEATABLE READ"; break;
	case TxOptions::Iso::Serializable  : s += " ISOLATION LEVEL SERIALIZABLE"; break;
	default                            : break;
	}
	if (opt.read_only) {
		s += " READ ONLY";
	}
	if (opt.deferrable) {
		s += " DEFERRABLE";
	}
	return s;
}

inline chrono::milliseconds retry_backoff(
	int attempt) noexcept {
	constexpr int base_ms = 100;
	constexpr int cap_ms = 2000;
	return chrono::milliseconds{min(base_ms << min(attempt, 4), cap_ms)};
}

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

// PQfnumber needs a NUL-terminated C string. Postgres NAMEDATALEN is 64,
// so a 128-byte stack buffer covers any real column name without heap alloc.
// Names longer than that cannot exist in a real result, so report not-found.
inline int fnumber_sv_(
	PGresult const *res,
	string_view col) noexcept {
	constexpr size_t kStackBuf = 128;
	if (col.size() >= kStackBuf) {
		return -1;
	}
	array<char, kStackBuf> buf{};
	ranges::copy(col, buf.begin());
	buf[col.size()] = '\0';
	return ::PQfnumber(res, buf.data());
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

} // namespace detail

export struct Column {
	int idx{-1};
	[[nodiscard]] explicit operator bool() const noexcept { return idx >= 0; }
};

export class Row {
	PGresult *res_{nullptr};
	int row_{0};

public:
	Row() = default;
	Row(
		PGresult *r,
		int i) noexcept
		: res_{r}
		, row_{i} {}

	[[nodiscard]] int ncols() const noexcept { return ::PQnfields(res_); }

	[[nodiscard]] bool is_null(
		int c) const noexcept {
		return ::PQgetisnull(res_, row_, c) != 0;
	}
	[[nodiscard]] bool is_null(
		Column c) const noexcept {
		return is_null(c.idx);
	}

	[[nodiscard]] string_view get(
		int c) const noexcept {
		char const *p = ::PQgetvalue(res_, row_, c);
		auto const n = static_cast<size_t>(::PQgetlength(res_, row_, c));
		return {p != nullptr ? p : "", n};
	}
	[[nodiscard]] string_view get(
		Column c) const noexcept {
		return get(c.idx);
	}

	[[nodiscard]] int length(
		int c) const noexcept {
		return ::PQgetlength(res_, row_, c);
	}

	[[nodiscard]] string_view get(
		string_view col) const {
		int const idx = detail::fnumber_sv_(res_, col);
		if (idx < 0) {
			throw PgError{format("column not found: {}", col)};
		}
		return get(idx);
	}

	template<class T>
	[[nodiscard]] T as(int c) const;

	template<class T>
	[[nodiscard]] T as(
		Column c) const {
		return as<T>(c.idx);
	}

	template<class T>
	[[nodiscard]] optional<T> as_opt(
		int c) const {
		if (is_null(c)) {
			return nullopt;
		}
		return as<T>(c);
	}

	template<class T>
	[[nodiscard]] optional<T> as_opt(
		Column c) const {
		return as_opt<T>(c.idx);
	}

	template<class... Ts>
	[[nodiscard]] tuple<Ts...> as_tuple(
		int start = 0) const {
		int col = start;
		return tuple<Ts...>{as<Ts>(col++)...};
	}
};

template<>
inline string Row::as<string>(
	int c) const {
	return string{get(c)};
}
template<>
inline string_view Row::as<string_view>(
	int c) const {
	return get(c);
}
template<>
inline i64 Row::as<i64>(
	int c) const {
	auto sv = get(c);
	i64 v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("int64 parse failed: {}", sv)};
	}
	return v;
}
template<>
inline i32 Row::as<i32>(
	int c) const {
	auto sv = get(c);
	i32 v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("int32 parse failed: {}", sv)};
	}
	return v;
}
template<>
inline double Row::as<double>(
	int c) const {
	auto sv = get(c);
	double v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("double parse failed: {}", sv)};
	}
	return v;
}
template<>
inline bool Row::as<bool>(
	int c) const {
	auto sv = get(c);
	if (sv == "t" || sv == "true" || sv == "1") {
		return true;
	}
	if (sv == "f" || sv == "false" || sv == "0") {
		return false;
	}
	throw PgError{format("bool parse failed: {}", sv)};
}

export class Result {
	PGResultPtr res_{};

public:
	Result() = default;
	explicit Result(
		PGResultPtr r) noexcept
		: res_{move(r)} {}

	Result(Result const &) = delete;
	Result &operator =(Result const &) = delete;
	Result(Result &&) noexcept = default;
	Result &operator =(Result &&) noexcept = default;
	~Result() = default;

	[[nodiscard]] PGresult *raw() const noexcept { return res_.get(); }
	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(res_); }

	[[nodiscard]] ExecStatusType status() const noexcept {
		return res_ ? ::PQresultStatus(res_.get()) : PGRES_FATAL_ERROR;
	}

	[[nodiscard]] bool ok() const noexcept {
		auto const s = status();
		return s == PGRES_TUPLES_OK || s == PGRES_COMMAND_OK;
	}

	[[nodiscard]] int rows() const noexcept { return res_ ? ::PQntuples(res_.get()) : 0; }
	[[nodiscard]] int cols() const noexcept { return res_ ? ::PQnfields(res_.get()) : 0; }

	[[nodiscard]] string_view column_name(
		int c) const noexcept {
		char const *p = res_ ? ::PQfname(res_.get(), c) : nullptr;
		return p != nullptr ? string_view{p} : string_view{};
	}

	[[nodiscard]] int column_index(
		string_view name) const noexcept {
		if (!res_) {
			return -1;
		}
		return detail::fnumber_sv_(res_.get(), name);
	}

	[[nodiscard]] Column column(
		string_view name) const noexcept {
		return Column{column_index(name)};
	}

	[[nodiscard]] string_view command_tag() const noexcept {
		char const *p = res_ ? ::PQcmdTuples(res_.get()) : nullptr;
		return p != nullptr ? string_view{p} : string_view{};
	}

	[[nodiscard]] Row operator [](
		int r) const noexcept {
		return Row{res_.get(), r};
	}

	class Iterator {
		PGresult *res_{nullptr};
		int row_{0};

	public:
		using iterator_category = forward_iterator_tag;
		using value_type = Row;
		using difference_type = ptrdiff_t;
		using pointer = void;
		using reference = Row;

		Iterator() = default;
		Iterator(
			PGresult *r,
			int i) noexcept
			: res_{r}
			, row_{i} {}

		[[nodiscard]] Row operator *() const noexcept { return Row{res_, row_}; }

		Iterator &operator ++() noexcept {
			++row_;
			return *this;
		}
		Iterator operator ++(
			int) noexcept {
			auto t = *this;
			++row_;
			return t;
		}
		[[nodiscard]] bool operator ==(
			Iterator const &o) const noexcept {
			return row_ == o.row_;
		}
	};

	[[nodiscard]] Iterator begin() const noexcept { return Iterator{res_.get(), 0}; }
	[[nodiscard]] Iterator end() const noexcept { return Iterator{res_.get(), rows()}; }
};

export class Params {
	vector<optional<string>> owned_{};
	vector<int> lengths_{};
	vector<int> formats_{};
	vector<Oid> types_{};
	mutable vector<char const *> values_cache_{};
	mutable bool cache_dirty_{true};

	void rebuild_cache_() const {
		if (!cache_dirty_) {
			return;
		}
		values_cache_.clear();
		values_cache_.reserve(owned_.size());
		for (auto const &o: owned_) {
			values_cache_.push_back(o ? o->c_str() : nullptr);
		}
		cache_dirty_ = false;
	}

public:
	Params() {
		constexpr size_t kCommonSlots = 8;
		owned_.reserve(kCommonSlots);
		lengths_.reserve(kCommonSlots);
		formats_.reserve(kCommonSlots);
		types_.reserve(kCommonSlots);
	}

	Params &add_null() {
		owned_.emplace_back();
		lengths_.push_back(0);
		formats_.push_back(0);
		types_.push_back(0);
		cache_dirty_ = true;
		return *this;
	}

	Params &add(
		string_view v) {
		owned_.emplace_back(string{v});
		lengths_.push_back(static_cast<int>(v.size()));
		formats_.push_back(0);
		types_.push_back(0);
		cache_dirty_ = true;
		return *this;
	}

	Params &add(
		char const *v) {
		return add(string_view{v != nullptr ? v : ""});
	}

	Params &add(
		i64 v) {
		array<char, 24> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		i32 v) {
		array<char, 16> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		double v) {
		array<char, 32> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		bool v) {
		return add(v ? string_view{"t"} : string_view{"f"});
	}
	Params &add_json(
		string_view j) {
		return add(j);
	}

	[[nodiscard]] int count() const noexcept { return static_cast<int>(owned_.size()); }
	[[nodiscard]] Oid const *types() const noexcept { return types_.empty() ? nullptr : types_.data(); }
	// NOLINTNEXTLINE(bugprone-exception-escape) — vector growth in cache rebuild;
	// libpq accessors are documented as noexcept-equivalent.
	[[nodiscard]] char const *const *values() const noexcept {
		rebuild_cache_();
		return values_cache_.empty() ? nullptr : values_cache_.data();
	}
	[[nodiscard]] int const *lengths() const noexcept { return lengths_.empty() ? nullptr : lengths_.data(); }
	[[nodiscard]] int const *formats() const noexcept { return formats_.empty() ? nullptr : formats_.data(); }
	[[nodiscard]] int result_format() const noexcept { return 0; }
};

export struct ConnectParams {
	string conninfo{};
	chrono::milliseconds connect_deadline{chrono::seconds{15}};
};

export class Pool;

export class Connection : public enable_shared_from_this<Connection> {
public:
	Connection(Connection const &) = delete;
	Connection &operator =(Connection const &) = delete;
	Connection(Connection &&) = delete;
	Connection &operator =(Connection &&) = delete;

	~Connection() { close(); }

	static Flow<shared_ptr<Connection>> connect(ConnectParams const &params);

	// Ergonomic overload: SQL is materialised into an owned string captured
	// by enqueue_job_'s lambda. Same allocation count as today.
	Flow<Result> query(string_view sql, Params params = {});
	// Zero-copy overload: caller threads a cached SQL handle (e.g. from
	// QueryCache::load). The lambda captures the shared_ptr; run_query_
	// reads sql->c_str() without copying.
	Flow<Result> query(shared_ptr<string const> sql, Params params = {});

	Flow<void> prepare(string_view name, string_view sql, span<Oid const> param_types = {});
	Flow<void> prepare(string_view name, shared_ptr<string const> sql, span<Oid const> param_types = {});

	Flow<Result> exec_prepared(string_view name, Params params = {});

	Flow<void> cancel_inflight(WorkPool &cancel_pool);

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

	friend class Pool;
	friend struct detail::ConnectState;
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

	[[nodiscard]] Flow<shared_ptr<string const>> load_async(string_view name);

	void clear() noexcept {
		scoped_lock const lk{mtx_};
		cache_.clear();
	}
};

// ===========================================================================
// QueryCache::load_async implementation
// ===========================================================================

Flow<shared_ptr<string const>> QueryCache::load_async(
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

export struct PoolConfig {
	ConnectParams conn{};
	size_t min_connections{1};
	size_t max_connections{8};
	chrono::milliseconds acquire_timeout{chrono::seconds{5}};
	function<Flow<void>(Connection &)> on_acquire{};
};

export class Pool : public enable_shared_from_this<Pool> {
public:
	class Lease {
		shared_ptr<Pool> pool_{};
		shared_ptr<Connection> conn_{};

		friend class Pool;

		Lease(
			shared_ptr<Pool> p,
			shared_ptr<Connection> c) noexcept
			: pool_{move(p)}
			, conn_{move(c)} {}

	public:
		Lease() = default;
		Lease(Lease const &) = delete;
		Lease &operator =(Lease const &) = delete;
		Lease(Lease &&) noexcept = default;
		Lease &operator =(Lease &&) noexcept = default;

		~Lease() {
			if (pool_ && conn_) {
				pool_->return_(move(conn_));
			}
		}

		[[nodiscard]] Connection &operator *() const noexcept { return *conn_; }
		[[nodiscard]] Connection *operator ->() const noexcept { return conn_.get(); }
		[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(conn_); }
	};

	static shared_ptr<Pool> create(PoolConfig cfg);

	~Pool() { close(); }

	Pool(Pool const &) = delete;
	Pool &operator =(Pool const &) = delete;
	Pool(Pool &&) = delete;
	Pool &operator =(Pool &&) = delete;

	Flow<Lease> acquire();
	void close() noexcept;

	[[nodiscard]] size_t total() const noexcept { return total_; }
	[[nodiscard]] size_t idle() const noexcept { return idle_.size(); }

private:
	explicit Pool(
		PoolConfig cfg) noexcept
		: cfg_{move(cfg)}
		, owner_{this_thread::get_id()} {}

	void return_(shared_ptr<Connection> conn) noexcept;
	void try_dispatch_waiters_();
	void grow_if_needed_();
	void dispatch_lease_(FlowSource<Lease> const &w, shared_ptr<Connection> conn) noexcept;

	PoolConfig cfg_{};
	thread::id owner_{};
	vector<shared_ptr<Connection>> idle_{};
	deque<FlowSource<Lease>> waiters_{};
	size_t total_{0};
	bool closed_{false};
};

export template<class Body>
	requires requires(Body &&b, Connection &c) {
		typename detail::awaitable_value<invoke_result_t<Body, Connection &>>::type;
	}
auto with_transaction(
	Connection &c,
	TxOptions opt,
	Body &&body) -> Task<detail::awaitable_value_t<invoke_result_t<Body, Connection &>>> {
	using R = detail::awaitable_value_t<invoke_result_t<Body, Connection &>>;
	string const begin_stmt = detail::begin_sql(opt);
	for (int attempt = 0;; ++attempt) {
		if (attempt > 0) {
			if (auto *reader = current_file_reader(); reader != nullptr) {
				co_await reader->timeout_async(detail::retry_backoff(attempt - 1));
			}
		}
		co_await c.query(begin_stmt);
		exception_ptr err{};
		if constexpr (same_as<R, void>) {
			try {
				co_await body(c);
			} catch (...) { err = current_exception(); }
			if (!err) {
				co_await c.query("COMMIT");
				co_return;
			}
		} else {
			optional<R> result{};
			try {
				result.emplace(co_await body(c));
			} catch (...) { err = current_exception(); }
			if (!err) {
				co_await c.query("COMMIT");
				co_return move(*result);
			}
		}
		try {
			co_await c.query("ROLLBACK");
			// NOLINTNEXTLINE(bugprone-empty-catch) — best-effort rollback; secondary errors swallowed.
		} catch (...) {}
		bool const retryable = [&] {
			try {
				rethrow_exception(err);
			} catch (PgError const &e) {
				return (e.is_serialization() || e.is_deadlock()) && attempt < opt.max_retries;
			}
			// NOLINTNEXTLINE(bugprone-empty-catch) — non-PgError → not retryable; swallow.
			catch (...) {}
			return false;
		}();
		if (!retryable) {
			rethrow_exception(err);
		}
	}
}

export template<class Body>
	requires requires(Body &&b, Connection &c) {
		typename detail::awaitable_value<invoke_result_t<Body, Connection &>>::type;
	}
auto with_transaction(
	Pool &p,
	TxOptions opt,
	Body &&body) -> Task<detail::awaitable_value_t<invoke_result_t<Body, Connection &>>> {
	using R = detail::awaitable_value_t<invoke_result_t<Body, Connection &>>;
	auto lease = co_await p.acquire();
	if constexpr (same_as<R, void>) {
		co_await with_transaction(*lease, opt, forward<Body>(body));
	} else {
		co_return co_await with_transaction(*lease, opt, forward<Body>(body));
	}
}

// ===========================================================================
// Implementation
// ===========================================================================

namespace detail {

inline FileReader *current_reader_or_throw() {
	auto *r = current_file_reader();
	if (r == nullptr) {
		throw PgError{"conflux.db: no current FileReader (not on a ring lane)"};
	}
	return r;
}

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
				// Reuses the existing async query machinery — same idiom
				// Pool::acquire uses for its own continuation pipeline.
				auto outer = dst;
				auto conn_sp = c;
				spawn(
					conn_sp->query(string_view{"SET client_encoding = 'UTF8'"})
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

} // namespace detail

Flow<shared_ptr<Connection>> Connection::connect(
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

Flow<Result> Connection::query(
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

Flow<Result> Connection::query(
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

Flow<void> Connection::prepare(
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

Flow<void> Connection::prepare(
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

Flow<Result> Connection::exec_prepared(
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

// Contract: callers (run_query_, run_prepare_, run_exec_prepared_) issue a
// single-statement query via PQsendQueryParams/PQsendPrepare/PQsendQueryPrepared,
// so PQgetResult produces at most one non-null result followed by nullptr. If a
// multi-statement query is ever routed through here the loop silently keeps
// only the last result.
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

namespace detail {

struct PGcancelDeleter {
	void operator ()(
		PGcancel *c) const noexcept {
		if (c != nullptr) {
			::PQfreeCancel(c);
		}
	}
};
using PGcancelPtr = unique_ptr<PGcancel, PGcancelDeleter>;

} // namespace detail

Flow<void> Connection::cancel_inflight(
	WorkPool &cancel_pool) {
	FlowSource<void> const src;
	auto flow = src.flow();
	if (!conn_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: connection closed"}));
		return flow;
	}
	detail::PGcancelDeleter const deleter{};
	(void)deleter;
	detail::PGcancelPtr handle{::PQgetCancel(conn_.get())};
	if (!handle) {
		src.reject(make_exception_ptr(PgError{"conflux.db: PQgetCancel returned null"}));
		return flow;
	}
	auto shared_handle = shared_ptr<PGcancel>{handle.release(), detail::PGcancelDeleter{}};
	bool const queued = cancel_pool.enqueue([shared_handle, src]() mutable {
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

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

shared_ptr<Pool> Pool::create(
	PoolConfig cfg) {
	auto p = shared_ptr<Pool>(new Pool{move(cfg)});
	p->grow_if_needed_();
	return p;
}

// NOLINTNEXTLINE(bugprone-exception-escape) — vector ops are noexcept on move-only payloads here.
void Pool::close() noexcept {
	if (closed_) {
		return;
	}
	closed_ = true;
	for (auto &w: waiters_) {
		w.cancel();
	}
	waiters_.clear();
	idle_.clear();
}

Flow<Pool::Lease> Pool::acquire() {
	FlowSource<Lease> const src;
	auto flow = src.flow();
	if (closed_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pool closed"}));
		return flow;
	}
	if (this_thread::get_id() != owner_) {
		src.reject(make_exception_ptr(PgError{"conflux.db: pool acquire off owner thread"}));
		return flow;
	}
	if (!idle_.empty()) {
		auto conn = move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(src, move(conn));
		return flow;
	}
	if (total_ < cfg_.max_connections) {
		++total_;
		auto self = shared_from_this();
		auto const &src_copy = src;
		spawn(
			Connection::connect(cfg_.conn)
			| then([self, src_copy](shared_ptr<Connection> conn) mutable {
				  if (self->closed_) {
					  --self->total_;
					  src_copy.cancel();
					  return;
				  }
				  self->dispatch_lease_(src_copy, move(conn));
			  })
			| on_error([self, src_copy](exception_ptr const &ex) mutable {
				  --self->total_;
				  src_copy.reject(ex);
			  }));
		return flow;
	}
	waiters_.push_back(src);
	if (cfg_.acquire_timeout.count() > 0) {
		if (auto *reader = current_file_reader(); reader != nullptr) {
			auto self = shared_from_this();
			spawn(
				reader->timeout_async(cfg_.acquire_timeout)
				| then(
					[self, src]() mutable { src.reject(make_exception_ptr(PgError{"conflux.db: acquire timeout"})); })
				| on_error([](exception_ptr const &) {})
				| on_cancel([]() {}));
		}
	}
	return flow;
}

// NOLINTNEXTLINE(bugprone-exception-escape) — try-block guards the only throwing call.
void Pool::return_(
	shared_ptr<Connection> conn) noexcept {
	if (closed_ || !conn || !conn->ok()) {
		if (conn) {
			conn->close();
		}
		if (total_ > 0) {
			--total_;
		}
	} else {
		idle_.push_back(move(conn));
	}
	try {
		try_dispatch_waiters_();
		// NOLINTNEXTLINE(bugprone-empty-catch) — FlowSource::resolve may throw if a waiter was already cancelled by
		// close(); swallow to keep noexcept.
	} catch (...) {}
}

// NOLINTNEXTLINE(misc-no-recursion) — mutual indirect recursion through async on_acquire boundary; no stack cycle.
void Pool::try_dispatch_waiters_() {
	while (!waiters_.empty() && !idle_.empty()) {
		while (!waiters_.empty() && !waiters_.front().armed()) {
			waiters_.pop_front();
		}
		if (waiters_.empty() || idle_.empty()) {
			break;
		}
		auto w = move(waiters_.front());
		waiters_.pop_front();
		auto conn = move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(w, move(conn));
	}
}

void Pool::grow_if_needed_() {
	while (total_ < cfg_.min_connections) {
		++total_;
		auto self = shared_from_this();
		spawn(
			Connection::connect(cfg_.conn)
			| then([self](shared_ptr<Connection> conn) mutable {
				  if (self->closed_) {
					  --self->total_;
					  return;
				  }
				  self->idle_.push_back(move(conn));
				  self->try_dispatch_waiters_();
			  })
			| on_error([self](exception_ptr const &) mutable { --self->total_; }));
	}
}

// NOLINTNEXTLINE(bugprone-exception-escape,misc-no-recursion)
void Pool::dispatch_lease_(
	FlowSource<Lease> const &w,
	shared_ptr<Connection> conn) noexcept {
	if (!w.armed()) {
		if (!closed_) {
			idle_.push_back(move(conn));
			try_dispatch_waiters_();
		} else {
			conn->close();
			--total_;
		}
		return;
	}
	if (!cfg_.on_acquire) {
		w.resolve(Lease{shared_from_this(), move(conn)});
		return;
	}
	auto self = shared_from_this();
	try {
		spawn(
			cfg_.on_acquire(*conn)
			| then([self, w, conn]() mutable {
				  if (self->closed_) {
					  conn->close();
					  --self->total_;
					  w.cancel();
					  return;
				  }
				  if (!w.armed()) {
					  self->idle_.push_back(move(conn));
					  self->try_dispatch_waiters_();
					  return;
				  }
				  w.resolve(Lease{self, move(conn)});
			  })
			| on_error([self, w, conn](exception_ptr const &ex) mutable {
				  conn->close();
				  --self->total_;
				  w.reject(ex);
			  })
			| on_cancel([self, w, conn]() mutable {
				  conn->close();
				  --self->total_;
				  w.cancel();
			  }));
	} catch (...) {
		conn->close();
		--self->total_;
		w.reject(current_exception());
	}
}

} // namespace conflux::db
