export module conflux.db.pool;

import std;
import conflux.db.types;
import conflux.db.result;
import conflux.db.connection;
import conflux.work;
import conflux.file_io;

using namespace std;

namespace conflux::db {

namespace root = conflux::work::root;

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

} // namespace detail

export struct PoolConfig {
	ConnectParams conn{};
	size_t min_connections{1};
	size_t max_connections{8};
	chrono::milliseconds acquire_timeout{chrono::seconds{5}};
	function<root::Task<void>(Connection &)> on_acquire{};
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

	root::Task<Lease> acquire();
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
	void dispatch_lease_(shared_ptr<root::TaskSource<Lease>> const &src, shared_ptr<Connection> conn) noexcept;

	PoolConfig cfg_{};
	thread::id owner_{};
	vector<shared_ptr<Connection>> idle_{};
	deque<shared_ptr<root::TaskSource<Lease>>> waiters_{};
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
				co_await task_as_flow(reader->timeout_async(detail::retry_backoff(attempt - 1)));
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
	auto lease = co_await task_as_flow(p.acquire());
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

// NOLINTNEXTLINE(bugprone-exception-escape) — try-block guards the only throwing call.
void Pool::close() noexcept {
	if (closed_) {
		return;
	}
	closed_ = true;
	for (auto &src: waiters_) {
		(void)src->commit_cancelled(root::CancelReason::requested);
	}
	waiters_.clear();
	idle_.clear();
}

root::Task<Pool::Lease> Pool::acquire() {
	auto [task, raw_src] = root::make_task_source<Lease>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = make_shared<root::TaskSource<Lease>>(move(raw_src));
	if (closed_) {
		(void)shared_src->commit_failure(make_exception_ptr(PgError{"conflux.db: pool closed"}));
		return move(task);
	}
	if (this_thread::get_id() != owner_) {
		(void)shared_src->commit_failure(make_exception_ptr(PgError{"conflux.db: pool acquire off owner thread"}));
		return move(task);
	}
	if (!idle_.empty()) {
		auto conn = move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(shared_src, move(conn));
		return move(task);
	}
	if (total_ < cfg_.max_connections) {
		++total_;
		auto self = shared_from_this();
		spawn(
			task_as_flow(Connection::connect(cfg_.conn))
			| then([self, shared_src](shared_ptr<Connection> conn) mutable {
				  if (self->closed_) {
					  --self->total_;
					  (void)shared_src->commit_cancelled(root::CancelReason::requested);
					  return;
				  }
				  self->dispatch_lease_(shared_src, move(conn));
			  })
			| on_error([self, shared_src](exception_ptr const &ex) mutable {
				  --self->total_;
				  (void)shared_src->commit_failure(ex);
			  }));
		return move(task);
	}
	waiters_.push_back(shared_src);
	if (cfg_.acquire_timeout.count() > 0) {
		if (auto *reader = current_file_reader(); reader != nullptr) {
			auto self = shared_from_this();
			spawn(
				task_as_flow(reader->timeout_async(cfg_.acquire_timeout))
				| then([self, shared_src]() mutable {
					  (void)shared_src->commit_failure(make_exception_ptr(PgError{"conflux.db: acquire timeout"}));
				  })
				| on_error([](exception_ptr const &) {})
				| on_cancel([]() {}));
		}
	}
	return move(task);
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
	// NOLINTNEXTLINE(bugprone-empty-catch) — spawn or container ops may throw; swallow to keep noexcept.
	catch (...) {}
}

// NOLINTNEXTLINE(misc-no-recursion) — mutual indirect recursion through Lease RAII and async on_acquire boundary; no
// stack cycle in steady state.
void Pool::try_dispatch_waiters_() {
	while (!waiters_.empty() && !idle_.empty()) {
		auto src = move(waiters_.front());
		waiters_.pop_front();
		auto conn = move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(src, move(conn));
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
	shared_ptr<root::TaskSource<Lease>> const &src,
	shared_ptr<Connection> conn) noexcept {
	if (!cfg_.on_acquire) {
		// If commit fails (waiter timed out), ~Lease fires → return_ → try_dispatch_waiters_.
		(void)src->commit_success(
			root::Success<Lease>{
				Lease{shared_from_this(), move(conn)}
        });
		return;
	}
	auto self = shared_from_this();
	try {
		spawn(
			task_as_flow(cfg_.on_acquire(*conn))
			| then([self, src, conn]() mutable {
				  if (self->closed_) {
					  conn->close();
					  --self->total_;
					  (void)src->commit_cancelled(root::CancelReason::requested);
					  return;
				  }
				  // If commit fails (timeout during on_acquire), ~Lease returns conn to pool.
				  (void)src->commit_success(
					  root::Success<Lease>{
						  Lease{self, move(conn)}
                  });
			  })
			| on_error([self, src, conn](exception_ptr const &ex) mutable {
				  conn->close();
				  --self->total_;
				  (void)src->commit_failure(ex);
			  })
			| on_cancel([self, src, conn]() mutable {
				  conn->close();
				  --self->total_;
				  (void)src->commit_cancelled(root::CancelReason::requested);
			  }));
	} catch (...) {
		conn->close();
		--self->total_;
		(void)src->commit_failure(current_exception());
	}
}

} // namespace conflux::db
