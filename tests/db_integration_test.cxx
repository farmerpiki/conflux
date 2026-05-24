// Plain TU — env-gated PostgreSQL integration tests (require PG_TEST_CONNINFO).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <libpq-fe.h>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db;

using namespace conflux::db;
using conflux::work::root::Task;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	FileReader reader;
	bool ring_ok{false};
	RingFixture()
		: reader{&ring, &completions, [](std::uint32_t slot, std::uint32_t gen) noexcept {
					 return pack_ud(slot, gen);
				 }} {}
	static std::unique_ptr<RingFixture> make(
		unsigned entries = 128) {
		auto fx = std::make_unique<RingFixture>();
		if (::io_uring_queue_init(entries, &fx->ring, 0) < 0) {
			return {};
		}
		fx->ring_ok = true;
		return fx;
	}
	~RingFixture() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingFixture(RingFixture const &) = delete;
	RingFixture &operator =(RingFixture const &) = delete;
	RingFixture(RingFixture &&) = delete;
	RingFixture &operator =(RingFixture &&) = delete;
};
std::unique_ptr<RingFixture> require_ring_fixture() {
	auto fx = RingFixture::make();
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}
std::optional<std::string> conninfo() {
	char const *p = std::getenv("PG_TEST_CONNINFO");
	if (p == nullptr || *p == '\0') {
		return std::nullopt;
	}
	return std::string{p};
}
std::shared_ptr<Connection> connect_or_skip(
	RingFixture &fx,
	std::string const &ci) {
	ConnectParams const cp{.conninfo = ci, .connect_deadline = std::chrono::seconds{10}};
	return block_on(fx.reader, Connection::connect(cp), std::chrono::seconds{30});
}

} // namespace
TEST_CASE(
	"db: connect happy path + simple SELECT",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	REQUIRE(conn);
	REQUIRE(conn->ok());
	CHECK(conn->backend_pid() != 0);

	auto r = block_on(fx->reader, conn->query("SELECT 1::int8 AS v"), std::chrono::seconds{30});
	REQUIRE(r.ok());
	REQUIRE(r.rows() == 1);
	REQUIRE(r.cols() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 1);
	CHECK(r.column_name(0) == "v");

	// P11b: connect enforces UTF-8 client_encoding.
	char const *enc = ::PQparameterStatus(conn->raw(), "client_encoding");
	REQUIRE(enc != nullptr);
	CHECK(std::string_view{enc} == "UTF8");
}
TEST_CASE(
	"db: bad credentials reject with PgError",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	ConnectParams const cp{
		.conninfo = "host=127.0.0.1 port=1 user=nope dbname=nope connect_timeout=2",
		.connect_deadline = std::chrono::seconds{5},
	};
	CHECK_THROWS_AS(block_on(fx->reader, Connection::connect(cp), std::chrono::seconds{30}), PgError);
}
TEST_CASE(
	"db: query with mixed null/text params + UPSERT + 23505",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	(void)block_on(
		fx->reader,
		conn->query(R"(CREATE TEMP TABLE t (id int8 PRIMARY KEY, name text, note text))"),
		std::chrono::seconds{30});

	{
		Params p;
		p.add(std::int64_t{1}).add("alpha").add_null();
		auto r = block_on(
			fx->reader,
			conn->query("INSERT INTO t (id, name, note) VALUES ($1, $2, $3)", std::move(p)),
			std::chrono::seconds{30});
		REQUIRE(r.ok());
		CHECK(r.command_tag() == "1");
	}

	{
		Params p;
		p.add(std::int64_t{1});
		auto r = block_on(
			fx->reader,
			conn->query("SELECT name, note FROM t WHERE id = $1", std::move(p)),
			std::chrono::seconds{30});
		REQUIRE(r.ok());
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::string>(0) == "alpha");
		CHECK(r[0].is_null(1));
	}

	{
		Params p;
		p.add(std::int64_t{1}).add("dup").add_null();
		try {
			(void)block_on(
				fx->reader,
				conn->query("INSERT INTO t (id, name, note) VALUES ($1, $2, $3)", std::move(p)),
				std::chrono::seconds{30});
			FAIL("expected unique violation");
		} catch (PgError const &e) { CHECK(e.is_unique_violation()); }
	}

	{
		Params p;
		p.add(std::int64_t{1}).add("upserted");
		auto r = block_on(
			fx->reader,
			conn->query(
				R"(INSERT INTO t(id,name)VALUES($1,$2)
ON CONFLICT(id)DO UPDATE SET name=EXCLUDED.name)",
				std::move(p)),
			std::chrono::seconds{30});
		REQUIRE(r.ok());
	}

	{
		auto r = block_on(fx->reader, conn->query("SELECT name FROM t WHERE id = 1"), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::string>(0) == "upserted");
	}
}
TEST_CASE(
	"db: prepare + exec_prepared round trip",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	block_on(fx->reader, conn->prepare("p_add", "SELECT $1::int8 + $2::int8"), std::chrono::seconds{30});

	{
		Params p;
		p.add(std::int64_t{40}).add(std::int64_t{2});
		auto r = block_on(fx->reader, conn->exec_prepared("p_add", std::move(p)), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::int64_t>(0) == 42);
	}
	{
		Params p;
		p.add(std::int64_t{100}).add(std::int64_t{-1});
		auto r = block_on(fx->reader, conn->exec_prepared("p_add", std::move(p)), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::int64_t>(0) == 99);
	}
}
TEST_CASE(
	"db: pipeline query ordering",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});

	auto f1 = pipeline.query("SELECT 11::int8");
	auto f2 = pipeline.query("SELECT 22::int8");
	auto f3 = pipeline.query("SELECT 33::int8");

	block_on(fx->reader, pipeline.sync(), std::chrono::seconds{30});

	auto r1 = block_on(fx->reader, std::move(f1), std::chrono::seconds{30});
	auto r2 = block_on(fx->reader, std::move(f2), std::chrono::seconds{30});
	auto r3 = block_on(fx->reader, std::move(f3), std::chrono::seconds{30});

	REQUIRE(r1.rows() == 1);
	REQUIRE(r2.rows() == 1);
	REQUIRE(r3.rows() == 1);
	CHECK(r1[0].as<std::int64_t>(0) == 11);
	CHECK(r2[0].as<std::int64_t>(0) == 22);
	CHECK(r3[0].as<std::int64_t>(0) == 33);
}
TEST_CASE(
	"db: pipeline exec_cached prepares and executes on the wire",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	StatementCache sc;
	auto stmt = sc.get("SELECT $1::int8 + $2::int8");

	auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});
	Params p1;
	p1.add(std::int64_t{20}).add(std::int64_t{22});
	Params p2;
	p2.add(std::int64_t{100}).add(std::int64_t{23});
	auto f1 = pipeline.exec_cached(stmt, std::move(p1));
	auto f2 = pipeline.exec_cached(stmt, std::move(p2));

	block_on(fx->reader, pipeline.sync(), std::chrono::seconds{30});

	auto r1 = block_on(fx->reader, std::move(f1), std::chrono::seconds{30});
	auto r2 = block_on(fx->reader, std::move(f2), std::chrono::seconds{30});

	REQUIRE(r1.rows() == 1);
	REQUIRE(r2.rows() == 1);
	CHECK(r1[0].as<std::int64_t>(0) == 42);
	CHECK(r2[0].as<std::int64_t>(0) == 123);
}

TEST_CASE(
	"db: pipeline rejects exec_cached while sync in progress",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	StatementCache sc;
	auto stmt = sc.get("SELECT 44::int8");
	auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});

	auto slow = pipeline.query("SELECT pg_sleep(0.2)");
	auto sync = pipeline.sync();
	auto rejected = pipeline.exec_cached(stmt);

	CHECK_THROWS_AS(block_on(fx->reader, std::move(rejected), std::chrono::seconds{30}), PgError);
	block_on(fx->reader, std::move(sync), std::chrono::seconds{30});
	auto r = block_on(fx->reader, std::move(slow), std::chrono::seconds{30});
	REQUIRE(r.ok());
}

TEST_CASE(
	"db: pipeline owns connection until teardown",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	{
		auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});
		CHECK_THROWS_AS(block_on(fx->reader, conn->query("SELECT 1::int8"), std::chrono::seconds{30}), PgError);
	}

	auto r = block_on(fx->reader, conn->query("SELECT 2::int8"), std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 2);
}
TEST_CASE(
	"db: pipeline isolates per-query failures",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});

	auto ok1 = pipeline.query("SELECT 7::int8");
	auto bad = pipeline.query("SELECT * FROM definitely_missing_table_for_pipeline_test");
	auto after = pipeline.query("SELECT 9::int8");

	block_on(fx->reader, pipeline.sync(), std::chrono::seconds{30});

	auto r1 = block_on(fx->reader, std::move(ok1), std::chrono::seconds{30});
	REQUIRE(r1.rows() == 1);
	CHECK(r1[0].as<std::int64_t>(0) == 7);

	CHECK_THROWS_AS(block_on(fx->reader, std::move(bad), std::chrono::seconds{30}), PgError);
	CHECK_THROWS_AS(block_on(fx->reader, std::move(after), std::chrono::seconds{30}), PgError);
}
TEST_CASE(
	"db: pipeline teardown rejects queued work",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	std::optional<Task<Result>> pending{};
	{
		auto pipeline = block_on(fx->reader, conn->pipeline(), std::chrono::seconds{30});
		pending.emplace(pipeline.query("SELECT 123::int8"));
	}
	REQUIRE(pending);
	CHECK_THROWS_AS(block_on(fx->reader, std::move(*pending), std::chrono::seconds{30}), PgError);
}
TEST_CASE(
	"db: server-side disconnect surfaces as connection_lost",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	try {
		(void)block_on(
			fx->reader,
			conn->query("SELECT pg_terminate_backend(pg_backend_pid())"),
			std::chrono::seconds{30});
		FAIL("expected backend termination to surface as PgError");
	} catch (PgError const &e) {
		// PG ends the session abruptly: SQLSTATE 57P01 (admin shutdown)
		// or class 08 from the broken socket. Accept either as "lost".
		bool const lost_or_admin = e.is_connection_lost() || e.sqlstate.starts_with("57P");
		CHECK(lost_or_admin);
	}
}
TEST_CASE(
	"db: cancel_inflight stops a long-running query",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	std::atomic_flag sleep_done{};
	std::exception_ptr sleep_err;
	[](std::atomic_flag *done, std::exception_ptr *err, decltype(conn->query("")) qt) -> Task<void> {
		try {
			co_await std::move(qt);
		} catch (...) { *err = std::current_exception(); }
		done->test_and_set(std::memory_order_release);
	}(&sleep_done, &sleep_err, conn->query("SELECT pg_sleep(10)"))
																							 .detach();

	WorkPool cancel_pool{WorkPoolOptions{.threads = 1}};
	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	block_on(fx->reader, conn->cancel_inflight(cancel_pool), std::chrono::seconds{30});
	pump_until(fx->reader, sleep_done, std::chrono::seconds{10});

	REQUIRE(sleep_err);
	try {
		rethrow_exception(sleep_err);
	} catch (PgError const &e) {
		// Cancelled query → SQLSTATE 57014 (query_canceled)
		CHECK(e.sqlstate == "57014");
	} catch (...) { FAIL("expected PgError on cancelled query"); }
}
TEST_CASE(
	"db: pool acquire/release LIFO reuse + lazy growth",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	PoolConfig cfg{
		.conn = ConnectParams{.conninfo = *ci, .connect_deadline = std::chrono::seconds{10}},
		.min_connections = 1,
		.max_connections = 2,
		.acquire_timeout = std::chrono::seconds{5},
	};
	auto pool = Pool::create(std::move(cfg));

	{
		std::optional<Pool::Lease> lease1{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
		REQUIRE(lease1);
		int const pid1 = (*lease1)->backend_pid();

		std::optional<Pool::Lease> lease2{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
		REQUIRE(lease2);
		int const pid2 = (*lease2)->backend_pid();
		CHECK(pid1 != pid2);

		// Release in order 1 then 2 — LIFO means next acquire returns conn 2.
		lease1.reset();
		lease2.reset();

		std::optional<Pool::Lease> lease3{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
		REQUIRE(lease3);
		CHECK((*lease3)->backend_pid() == pid2);
	}

	pool->close();
}
TEST_CASE(
	"db: pool max_connections pressure times out queued acquire and recovers",
	"[db][integration][pressure]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	PoolConfig cfg{
		.conn = ConnectParams{.conninfo = *ci, .connect_deadline = std::chrono::seconds{10}},
		.min_connections = 0,
		.max_connections = 1,
		.acquire_timeout = std::chrono::milliseconds{150},
	};
	auto pool = Pool::create(std::move(cfg));

	std::optional<Pool::Lease> held{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(held);
	CHECK(pool->total() == 1);
	CHECK(pool->idle() == 0);

	try {
		(void)block_on(fx->reader, pool->acquire(), std::chrono::seconds{5});
		FAIL("expected acquire timeout under max_connections pressure");
	} catch (PgError const &e) { CHECK(std::string_view{e.what()}.find("acquire timeout") != std::string_view::npos); }

	held.reset();
	CHECK(pool->total() == 1);
	CHECK(pool->idle() == 1);

	std::optional<Pool::Lease> recovered{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(recovered);
	auto r = block_on(fx->reader, (*recovered)->query("SELECT 5::int8"), std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 5);

	recovered.reset();
	pool->close();
}
TEST_CASE(
	"db: pool queued acquire cancellation completes and does not consume next lease",
	"[db][integration][pressure]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	PoolConfig cfg{
		.conn = ConnectParams{.conninfo = *ci, .connect_deadline = std::chrono::seconds{10}},
		.min_connections = 0,
		.max_connections = 1,
		.acquire_timeout = std::chrono::seconds{10},
	};
	auto pool = Pool::create(std::move(cfg));

	std::optional<Pool::Lease> held{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(held);

	auto pending = pool->acquire();
	pending.cancel();
	CHECK_THROWS_AS(block_on(fx->reader, std::move(pending), std::chrono::seconds{1}), Cancelled);

	held.reset();
	std::optional<Pool::Lease> next{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(next);
	auto r = block_on(fx->reader, (*next)->query("SELECT 6::int8"), std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 6);

	next.reset();
	pool->close();
}
TEST_CASE(
	"db: pool replaces lost connection after backend termination",
	"[db][integration][pressure]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	PoolConfig cfg{
		.conn = ConnectParams{.conninfo = *ci, .connect_deadline = std::chrono::seconds{10}},
		.min_connections = 0,
		.max_connections = 1,
		.acquire_timeout = std::chrono::seconds{5},
	};
	auto pool = Pool::create(std::move(cfg));

	std::optional<Pool::Lease> lost{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(lost);
	int const lost_pid = (*lost)->backend_pid();
	CHECK(lost_pid != 0);

	try {
		(void)block_on(
			fx->reader,
			(*lost)->query("SELECT pg_terminate_backend(pg_backend_pid())"),
			std::chrono::seconds{30});
		FAIL("expected backend termination to surface as PgError");
	} catch (PgError const &e) {
		bool const lost_or_admin = e.is_connection_lost() || e.sqlstate.starts_with("57P");
		CHECK(lost_or_admin);
	}
	lost.reset();
	CHECK(pool->idle() == 0);
	CHECK(pool->total() == 0);

	std::optional<Pool::Lease> replacement{block_on(fx->reader, pool->acquire(), std::chrono::seconds{30})};
	REQUIRE(replacement);
	CHECK((*replacement)->backend_pid() != 0);
	auto r = block_on(fx->reader, (*replacement)->query("SELECT 7::int8"), std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 7);

	replacement.reset();
	pool->close();
}

TEST_CASE(
	"db: transaction query deadline cancels and rolls back",
	"[db][integration][pressure]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	block_on(
		fx->reader,
		conn->query(R"(CREATE TEMP TABLE tx_deadline_test (id int8 PRIMARY KEY, v text))"),
		std::chrono::seconds{30});

	try {
		block_on(
			fx->reader,
			with_transaction(
				*conn,
				TxOptions{},
				[](Connection &c) -> Task<void> {
					co_await c.query("INSERT INTO tx_deadline_test VALUES (1, 'pending')");
					co_await c.query(
						"SELECT pg_sleep(10)",
						{},
						QueryOptions{.deadline = std::make_optional(std::chrono::milliseconds{100})});
				}),
			std::chrono::seconds{30});
		FAIL("expected query deadline inside transaction");
	} catch (PgError const &e) { CHECK(e.sqlstate == "57014"); }

	auto r = block_on(
		fx->reader,
		conn->query("SELECT count(*) FROM tx_deadline_test WHERE id = 1"),
		std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 0);
}

TEST_CASE(
	"db: with_transaction commit and rollback",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);
	block_on(
		fx->reader,
		conn->query(R"(CREATE TEMP TABLE tx_test (id int8 PRIMARY KEY, v text))"),
		std::chrono::seconds{30});

	// Commit path: row must persist.
	block_on(
		fx->reader,
		with_transaction(
			*conn,
			TxOptions{},
			[](Connection &c) -> Task<void> { co_await c.query("INSERT INTO tx_test VALUES (1, 'committed')"); }),
		std::chrono::seconds{30});
	{
		auto r = block_on(fx->reader, conn->query("SELECT v FROM tx_test WHERE id = 1"), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::string>(0) == "committed");
	}

	// Rollback path: thrown exception must roll back the INSERT.
	try {
		block_on(
			fx->reader,
			with_transaction(
				*conn,
				TxOptions{},
				[](Connection &c) -> Task<void> {
					co_await c.query("INSERT INTO tx_test VALUES (2, 'rolledback')");
					throw std::runtime_error{"deliberate"};
				}),
			std::chrono::seconds{30});
		FAIL("expected exception");
	} catch (std::runtime_error const &e) { CHECK(std::string_view{e.what()} == "deliberate"); }
	{
		auto r =
			block_on(fx->reader, conn->query("SELECT count(*) FROM tx_test WHERE id = 2"), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::int64_t>(0) == 0);
	}
}
TEST_CASE(
	"db: QueryCache integrated load/use",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto root = std::filesystem::temp_directory_path()
			  / std::format("conflux_db_qc_int_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	std::filesystem::create_directories(root);
	{
		std::ofstream out{root / "select_two.psql"};
		out << "SELECT 2::int8";
	}

	QueryCache const qc{root};
	auto sql = qc.load_or_throw("select_two");
	REQUIRE(sql);

	auto conn = connect_or_skip(*fx, *ci);
	auto r = block_on(fx->reader, conn->query(*sql), std::chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<std::int64_t>(0) == 2);

	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}
TEST_CASE(
	"db: cancel_inflight zero-arg uses process-wide cancel pool",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	std::atomic_flag done{};
	std::exception_ptr err;
	[](std::atomic_flag *d, std::exception_ptr *e, decltype(conn->query("")) qt) -> Task<void> {
		try {
			co_await std::move(qt);
		} catch (...) { *e = std::current_exception(); }
		d->test_and_set(std::memory_order_release);
	}(&done, &err, conn->query("SELECT pg_sleep(10)"))
																						.detach();

	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	block_on(fx->reader, conn->cancel_inflight(), std::chrono::seconds{30});
	pump_until(fx->reader, done, std::chrono::seconds{10});

	REQUIRE(err);
	try {
		rethrow_exception(err);
	} catch (PgError const &e) { CHECK(e.sqlstate == "57014"); } catch (...) {
		FAIL("expected PgError");
	}
}
TEST_CASE(
	"db: query with deadline cancels slow query",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	QueryOptions const opts{.deadline = std::chrono::milliseconds{200}};
	try {
		(void)block_on(fx->reader, conn->query("SELECT pg_sleep(10)", Params{}, opts), std::chrono::seconds{30});
		FAIL("expected deadline cancellation");
	} catch (PgError const &e) { CHECK(e.sqlstate == "57014"); }
}
TEST_CASE(
	"db: exec_cached auto-prepares on first call and reuses on second",
	"[db][integration]") {
	auto ci = conninfo();
	if (!ci) {
		SKIP("PG_TEST_CONNINFO not set");
	}
	auto fx = require_ring_fixture();
	CurrentFileReaderScope const scope{&fx->reader};

	auto conn = connect_or_skip(*fx, *ci);

	StatementCache sc;
	auto stmt = sc.get("SELECT $1::int8 + $2::int8");

	{
		Params p;
		p.add(std::int64_t{10}).add(std::int64_t{32});
		auto r = block_on(fx->reader, conn->exec_cached(stmt, std::move(p)), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::int64_t>(0) == 42);
	}
	{
		Params p;
		p.add(std::int64_t{1}).add(std::int64_t{99});
		auto r = block_on(fx->reader, conn->exec_cached(stmt, std::move(p)), std::chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<std::int64_t>(0) == 100);
	}
}
