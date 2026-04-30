export module conflux.db.pool;

import std;
import conflux.db.types;
import conflux.db.result;
import conflux.db.connection;
import conflux.work;
import conflux.file_io;

using namespace std;

namespace conflux::db {

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

} // namespace detail

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
	conflux::work::root::Task<Lease> acquire_task();
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
		co_await task_as_flow(c.query(begin_stmt));
		exception_ptr err{};
		if constexpr (same_as<R, void>) {
			try {
				co_await body(c);
			} catch (...) { err = current_exception(); }
			if (!err) {
				co_await task_as_flow(c.query("COMMIT"));
				co_return;
			}
		} else {
			optional<R> result{};
			try {
				result.emplace(co_await body(c));
			} catch (...) { err = current_exception(); }
			if (!err) {
				co_await task_as_flow(c.query("COMMIT"));
				co_return move(*result);
			}
		}
		try {
			co_await task_as_flow(c.query("ROLLBACK"));
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
// Pool implementation
// ===========================================================================

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
			task_as_flow(Connection::connect(cfg_.conn))
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

conflux::work::root::Task<Pool::Lease> Pool::acquire_task() {
	return detail::flow_to_root_task(acquire());
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
	}
	// NOLINTNEXTLINE(bugprone-empty-catch) — FlowSource::resolve may throw if waiter cancelled by close(); swallow to
	// keep noexcept.
	catch (...) {}
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
			task_as_flow(Connection::connect(cfg_.conn))
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
