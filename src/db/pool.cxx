module;

export module conflux.pg.pool;

import std;
import conflux.types;
import conflux.pg.types;
import conflux.pg.result;
import conflux.pg.connection;
import conflux.work;
import conflux.uring.timeout;
import conflux.file_io;

namespace conflux::pg {
namespace root = conflux::work::root;
using conflux::work::Cancelled;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;
export struct TxOptions {
	enum class Iso : std::uint8_t {
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
struct awaitable_value<root::Task<T>> {
	using type = T;
};

template<class T>
using awaitable_value_t = typename awaitable_value<T>::type;
inline std::string begin_sql(
	TxOptions const &opt) {
	std::string s = "BEGIN";
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
inline std::chrono::milliseconds retry_backoff(
	int attempt) noexcept {
	constexpr int base_ms = 100;
	constexpr int cap_ms = 2000;
	return std::chrono::milliseconds{std::min(base_ms << std::min(attempt, 4), cap_ms)};
}
inline void ignore_best_effort_failure() noexcept {}

} // namespace detail
export struct PoolConfig {
	ConnectParams conn{};
	std::size_t min_connections{1};
	std::size_t max_connections{8};
	std::chrono::milliseconds acquire_timeout{std::chrono::seconds{5}};
	std::function<root::Task<void>(Connection &)> on_acquire{};
};
export class Pool : public std::enable_shared_from_this<Pool> {
public:
	class Lease {
		std::shared_ptr<Pool> pool_{};
		std::shared_ptr<Connection> conn_{};

		friend class Pool;
		Lease(
			std::shared_ptr<Pool> p,
			std::shared_ptr<Connection> c) noexcept
			: pool_{std::move(p)}
			, conn_{std::move(c)} {}

	public:
		Lease() = default;
		Lease(Lease const &) = delete;
		Lease &operator =(Lease const &) = delete;
		Lease(Lease &&) noexcept = default;
		Lease &operator =(Lease &&) noexcept = default;
		~Lease() {
			if (pool_ && conn_) {
				pool_->return_(std::move(conn_));
			}
		}
		[[nodiscard]] Connection &operator *() const noexcept { return *conn_; }
		[[nodiscard]] Connection *operator ->() const noexcept { return conn_.get(); }
		[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(conn_); }
	};
	struct Waiter {
		std::shared_ptr<root::TaskSource<Lease>> src;
		std::atomic_bool active{true};
		explicit Waiter(
			std::shared_ptr<root::TaskSource<Lease>> s) noexcept
			: src{std::move(s)} {}
	};
	static std::shared_ptr<Pool> create(PoolConfig cfg);
	~Pool() { close(); }
	Pool(Pool const &) = delete;
	Pool &operator =(Pool const &) = delete;
	Pool(Pool &&) = delete;
	Pool &operator =(Pool &&) = delete;

	root::Task<Lease> acquire();
	void close() noexcept;
	[[nodiscard]] std::size_t total() const noexcept { return total_.load(std::memory_order_acquire); }
	[[nodiscard]] std::size_t idle() const noexcept { return idle_.size(); }

private:
	explicit Pool(
		PoolConfig cfg) noexcept
		: cfg_{std::move(cfg)}
		, owner_{std::this_thread::get_id()} {}
	void return_(std::shared_ptr<Connection> conn) noexcept;
	void try_dispatch_waiters_();
	void grow_if_needed_();
	void dispatch_lease_(
		std::shared_ptr<root::TaskSource<Lease>> const &src,
		std::shared_ptr<Connection> conn) noexcept;

	PoolConfig cfg_{};
	std::thread::id owner_{};
	std::vector<std::shared_ptr<Connection>> idle_{};
	std::deque<std::shared_ptr<Waiter>> waiters_{};
	std::atomic_size_t total_{0};
	bool closed_{false};
};
export template<class Body>
	requires requires(Body &&b, Connection &c) {
		typename detail::awaitable_value<std::invoke_result_t<Body, Connection &>>::type;
	}
auto with_transaction(
	Connection &c,
	TxOptions opt,
	Body &&body) -> root::Task<detail::awaitable_value_t<std::invoke_result_t<Body, Connection &>>> {
	using R = detail::awaitable_value_t<std::invoke_result_t<Body, Connection &>>;
	std::string const begin_stmt = detail::begin_sql(opt);
	for (int attempt = 0;; ++attempt) {
		if (attempt > 0) {
			if (auto *reader = conflux::file_io::current_file_reader(); reader != nullptr) {
				co_await conflux::uring::async_timeout(
					reader->ring(),
					*reader->completions(),
					[reader](std::uint32_t slot, std::uint32_t gen) noexcept { return reader->encode_ud(slot, gen); },
					detail::retry_backoff(attempt - 1));
			}
		}
		co_await c.query(begin_stmt);
		std::exception_ptr err{};
		if constexpr (std::same_as<R, void>) {
			try {
				co_await body(c);
			} catch (...) { err = std::current_exception(); }
			if (!err) {
				try {
					co_await c.query("COMMIT");
					co_return;
				} catch (...) { err = std::current_exception(); }
			}
		} else {
			std::optional<R> result{};
			try {
				result.emplace(co_await body(c));
			} catch (...) { err = std::current_exception(); }
			if (!err) {
				try {
					co_await c.query("COMMIT");
					co_return std::move(*result);
				} catch (...) { err = std::current_exception(); }
			}
		}
		try {
			co_await c.query("ROLLBACK");
		} catch (...) { detail::ignore_best_effort_failure(); }
		bool const retryable = [&] {
			try {
				std::rethrow_exception(err);
			} catch (PgError const &e) {
				return (e.is_serialization() || e.is_deadlock()) && attempt < opt.max_retries;
			} catch (...) { detail::ignore_best_effort_failure(); }
			return false;
		}();
		if (!retryable) {
			std::rethrow_exception(err);
		}
	}
}
export template<class Body>
	requires requires(Body &&b, Connection &c) {
		typename detail::awaitable_value<std::invoke_result_t<Body, Connection &>>::type;
	}
auto with_transaction(
	Pool &p,
	TxOptions opt,
	Body &&body) -> root::Task<detail::awaitable_value_t<std::invoke_result_t<Body, Connection &>>> {
	using R = detail::awaitable_value_t<std::invoke_result_t<Body, Connection &>>;
	auto lease = co_await p.acquire();
	if constexpr (std::same_as<R, void>) {
		co_await with_transaction(*lease, opt, std::forward<Body>(body));
	} else {
		co_return co_await with_transaction(*lease, opt, std::forward<Body>(body));
	}
}
// ===========================================================================
// Pool implementation
// ===========================================================================

std::shared_ptr<Pool> Pool::create(
	PoolConfig cfg) {
	if (cfg.conn.cancel_pool == nullptr) {
		cfg.conn.cancel_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1});
	}
	auto p = std::shared_ptr<Pool>(new Pool{std::move(cfg)});
	p->grow_if_needed_();
	return p;
}
// NOLINTNEXTLINE(bugprone-exception-escape) — try-block guards the only throwing call.
void Pool::close() noexcept {
	if (closed_) {
		return;
	}
	closed_ = true;
	for (auto &waiter: waiters_) {
		waiter->active.store(false, std::memory_order_release);
		auto _ = waiter->src->try_set_cancelled(root::work_errc::cancelled_requested);
	}
	waiters_.clear();
	idle_.clear();
}
root::Task<Pool::Lease> Pool::acquire() {
	auto [task, shared_src] = root::make_shared_task_source<Lease>(root::SubmitOptions{.enable_cancellation = false});
	if (closed_) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(PgError{"conflux.pg: pool closed"}));
		return std::move(task);
	}
	if (std::this_thread::get_id() != owner_) {
		auto _ = shared_src->try_set_exception(
			std::make_exception_ptr(PgError{"conflux.pg: pool acquire off owner std::thread"}));
		return std::move(task);
	}
	if (!idle_.empty()) {
		auto conn = std::move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(shared_src, std::move(conn));
		return std::move(task);
	}
	if (total_ < cfg_.max_connections) {
		total_.fetch_add(1, std::memory_order_acq_rel);
		auto self = shared_from_this();
		[](std::shared_ptr<Pool> self,
		   std::shared_ptr<root::TaskSource<Lease>> shared_src,
		   root::Task<std::shared_ptr<Connection>> conn_task) -> root::Task<void> {
			try {
				auto conn = co_await std::move(conn_task);
				if (self->closed_) {
					self->total_.fetch_sub(1, std::memory_order_acq_rel);
					auto _ = shared_src->try_set_cancelled(root::work_errc::cancelled_requested);
					co_return;
				}
				self->dispatch_lease_(shared_src, std::move(conn));
			} catch (...) {
				self->total_.fetch_sub(1, std::memory_order_acq_rel);
				auto _ = shared_src->try_set_exception(std::current_exception());
			}
		}(self, shared_src, Connection::connect(cfg_.conn))
																	 .detach();
		return std::move(task);
	}
	auto waiter = std::make_shared<Waiter>(shared_src);
	waiters_.push_back(waiter);
	std::weak_ptr<Waiter> const weak_waiter{waiter};
	(void)shared_src->install_cancel_hook([weak_waiter](root::CancelReason) noexcept {
		if (auto waiter = weak_waiter.lock()) {
			waiter->active.store(false, std::memory_order_release);
			auto _ = waiter->src->try_set_cancelled(root::work_errc::cancelled_requested);
		}
	});
	if (cfg_.acquire_timeout.count() > 0) {
		if (auto *reader = conflux::file_io::current_file_reader(); reader != nullptr) {
			auto self = shared_from_this();
			[](std::shared_ptr<Waiter> waiter, root::Task<void> to_task) -> root::Task<void> {
				try {
					co_await std::move(to_task);
					if (waiter->active.exchange(false, std::memory_order_acq_rel)) {
						auto _ = waiter->src->try_set_exception(
							std::make_exception_ptr(PgError{"conflux.pg: acquire timeout"}));
					}
				} catch (...) { detail::ignore_best_effort_failure(); }
			}(waiter,
			  conflux::uring::async_timeout(
				  reader->ring(),
				  *reader->completions(),
				  [reader](std::uint32_t slot, std::uint32_t gen) noexcept { return reader->encode_ud(slot, gen); },
				  cfg_.acquire_timeout))
																				.detach();
		}
	}
	return std::move(task);
}
// NOLINTNEXTLINE(bugprone-exception-escape) — try-block guards the only throwing call.
void Pool::return_(
	std::shared_ptr<Connection> conn) noexcept {
	if (std::this_thread::get_id() != owner_) {
		if (conn) {
			conn->close();
			total_.fetch_sub(1, std::memory_order_acq_rel);
		}
		return;
	}
	if (closed_ || !conn || !conn->ok()) {
		if (conn) {
			conn->close();
		}
		if (total_ > 0) {
			total_.fetch_sub(1, std::memory_order_acq_rel);
		}
	} else {
		idle_.push_back(std::move(conn));
	}
	try {
		try_dispatch_waiters_();
	} catch (...) { detail::ignore_best_effort_failure(); }
}
// NOLINTNEXTLINE(misc-no-recursion) — mutual indirect recursion through Lease RAII and async on_acquire boundary; no
// stack cycle in steady state.
void Pool::try_dispatch_waiters_() {
	while (!waiters_.empty() && !idle_.empty()) {
		auto waiter = std::move(waiters_.front());
		waiters_.pop_front();
		if (!waiter->active.exchange(false, std::memory_order_acq_rel)) {
			continue;
		}
		auto conn = std::move(idle_.back());
		idle_.pop_back();
		dispatch_lease_(waiter->src, std::move(conn));
	}
}
void Pool::grow_if_needed_() {
	while (total_ < cfg_.min_connections) {
		total_.fetch_add(1, std::memory_order_acq_rel);
		auto self = shared_from_this();
		[](std::shared_ptr<Pool> self, root::Task<std::shared_ptr<Connection>> conn_task) -> root::Task<void> {
			try {
				auto conn = co_await std::move(conn_task);
				if (self->closed_) {
					self->total_.fetch_sub(1, std::memory_order_acq_rel);
					co_return;
				}
				self->idle_.push_back(std::move(conn));
				self->try_dispatch_waiters_();
			} catch (...) { self->total_.fetch_sub(1, std::memory_order_acq_rel); }
		}(self, Connection::connect(cfg_.conn))
																								 .detach();
	}
}
// NOLINTNEXTLINE(bugprone-exception-escape,misc-no-recursion)
void Pool::dispatch_lease_(
	std::shared_ptr<root::TaskSource<Lease>> const &src,
	std::shared_ptr<Connection> conn) noexcept {
	if (!cfg_.on_acquire) {
		// If commit fails (waiter timed out), ~Lease fires → return_ → try_dispatch_waiters_.
		auto _ = src->try_set_value(
			root::Success<Lease>{
				Lease{shared_from_this(), std::move(conn)}
        });
		return;
	}
	auto self = shared_from_this();
	try {
		[](std::shared_ptr<Pool> self,
		   std::shared_ptr<root::TaskSource<Lease>> src,
		   std::shared_ptr<Connection> conn,
		   root::Task<void> on_acq_task) -> root::Task<void> {
			try {
				co_await std::move(on_acq_task);
				if (self->closed_) {
					conn->close();
					self->total_.fetch_sub(1, std::memory_order_acq_rel);
					auto _ = src->try_set_cancelled(root::work_errc::cancelled_requested);
					co_return;
				}
				auto _ = src->try_set_value(
					root::Success<Lease>{
						Lease{self, std::move(conn)}
                });
			} catch (Cancelled const &) {
				conn->close();
				self->total_.fetch_sub(1, std::memory_order_acq_rel);
				auto _ = src->try_set_cancelled(root::work_errc::cancelled_requested);
			} catch (...) {
				conn->close();
				self->total_.fetch_sub(1, std::memory_order_acq_rel);
				auto _ = src->try_set_exception(std::current_exception());
			}
		}(self, src, conn, cfg_.on_acquire(*conn))
												.detach();
	} catch (...) {
		conn->close();
		self->total_.fetch_sub(1, std::memory_order_acq_rel);
		auto _ = src->try_set_exception(std::current_exception());
	}
}

} // namespace conflux::pg
