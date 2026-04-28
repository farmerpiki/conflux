// Plain TU — env-gated PostgreSQL integration tests (require PG_TEST_CONNINFO).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <liburing.h>

import std;
import conflux.work;
import conflux.file_io;
import conflux.db;

using namespace std;
using namespace conflux::db;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	FileReader reader;
	bool ring_ok{false};

	RingFixture()
		: reader{&ring, &completions, [](uint32_t slot, uint32_t gen) noexcept { return pack_ud(slot, gen); }} {}

	static unique_ptr<RingFixture> make(
		unsigned entries = 128) {
		auto fx = make_unique<RingFixture>();
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

unique_ptr<RingFixture> require_ring_fixture() {
	auto fx = RingFixture::make();
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}

optional<string> conninfo() {
	char const *p = ::getenv("PG_TEST_CONNINFO");
	if (p == nullptr || *p == '\0') {
		return nullopt;
	}
	return string{p};
}

shared_ptr<Connection> connect_or_skip(
	RingFixture &fx,
	string const &ci) {
	ConnectParams cp{.conninfo = ci, .connect_deadline = chrono::seconds{10}};
	return block_on(fx.reader, Connection::connect(move(cp)), chrono::seconds{30});
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

	auto r = block_on(fx->reader, conn->query("SELECT 1::int8 AS v"), chrono::seconds{30});
	REQUIRE(r.ok());
	REQUIRE(r.rows() == 1);
	REQUIRE(r.cols() == 1);
	CHECK(r[0].as<int64_t>(0) == 1);
	CHECK(r.column_name(0) == "v");
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

	ConnectParams cp{
		.conninfo = "host=127.0.0.1 port=1 user=nope dbname=nope connect_timeout=2",
		.connect_deadline = chrono::seconds{5},
	};
	CHECK_THROWS_AS(block_on(fx->reader, Connection::connect(move(cp)), chrono::seconds{30}), PgError);
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
		chrono::seconds{30});

	{
		Params p;
		p.add(int64_t{1}).add("alpha").add_null();
		auto r = block_on(
			fx->reader,
			conn->query("INSERT INTO t (id, name, note) VALUES ($1, $2, $3)", move(p)),
			chrono::seconds{30});
		REQUIRE(r.ok());
		CHECK(r.command_tag() == "1");
	}

	{
		Params p;
		p.add(int64_t{1});
		auto r =
			block_on(fx->reader, conn->query("SELECT name, note FROM t WHERE id = $1", move(p)), chrono::seconds{30});
		REQUIRE(r.ok());
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<string>(0) == "alpha");
		CHECK(r[0].is_null(1));
	}

	{
		Params p;
		p.add(int64_t{1}).add("dup").add_null();
		try {
			(void)block_on(
				fx->reader,
				conn->query("INSERT INTO t (id, name, note) VALUES ($1, $2, $3)", move(p)),
				chrono::seconds{30});
			FAIL("expected unique violation");
		} catch (PgError const &e) { CHECK(e.is_unique_violation()); }
	}

	{
		Params p;
		p.add(int64_t{1}).add("upserted");
		auto r = block_on(
			fx->reader,
			conn->query(
				R"(INSERT INTO t (id, name) VALUES ($1, $2)
				   ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name)",
				move(p)),
			chrono::seconds{30});
		REQUIRE(r.ok());
	}

	{
		auto r = block_on(fx->reader, conn->query("SELECT name FROM t WHERE id = 1"), chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<string>(0) == "upserted");
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

	block_on(fx->reader, conn->prepare("p_add", "SELECT $1::int8 + $2::int8"), chrono::seconds{30});

	{
		Params p;
		p.add(int64_t{40}).add(int64_t{2});
		auto r = block_on(fx->reader, conn->exec_prepared("p_add", move(p)), chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<int64_t>(0) == 42);
	}
	{
		Params p;
		p.add(int64_t{100}).add(int64_t{-1});
		auto r = block_on(fx->reader, conn->exec_prepared("p_add", move(p)), chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<int64_t>(0) == 99);
	}
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
		(void)block_on(fx->reader, conn->query("SELECT pg_terminate_backend(pg_backend_pid())"), chrono::seconds{30});
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

	atomic_flag sleep_done{};
	exception_ptr sleep_err;
	auto sleep_held = conn->query("SELECT pg_sleep(10)")
					| then([&](Result) { sleep_done.test_and_set(memory_order_release); })
					| on_error([&](exception_ptr const &ex) {
						  sleep_err = ex;
						  sleep_done.test_and_set(memory_order_release);
					  });
	(void)sleep_held;

	WorkPool cancel_pool{WorkPoolOptions{.threads = 1}};
	this_thread::sleep_for(chrono::milliseconds{100});
	block_on(fx->reader, conn->cancel_inflight(cancel_pool), chrono::seconds{30});
	pump_until(fx->reader, sleep_done, chrono::seconds{10});

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
		.conn = ConnectParams{.conninfo = *ci, .connect_deadline = chrono::seconds{10}},
		.min_connections = 1,
		.max_connections = 2,
		.acquire_timeout = chrono::seconds{5},
	};
	auto pool = Pool::create(move(cfg));

	{
		auto lease1 = optional{block_on(fx->reader, pool->acquire(), chrono::seconds{30})};
		REQUIRE(lease1);
		int const pid1 = (*lease1)->backend_pid();

		auto lease2 = optional{block_on(fx->reader, pool->acquire(), chrono::seconds{30})};
		REQUIRE(lease2);
		int const pid2 = (*lease2)->backend_pid();
		CHECK(pid1 != pid2);

		// Release in order 1 then 2 — LIFO means next acquire returns conn 2.
		lease1.reset();
		lease2.reset();

		auto lease3 = optional{block_on(fx->reader, pool->acquire(), chrono::seconds{30})};
		REQUIRE(lease3);
		CHECK((*lease3)->backend_pid() == pid2);
	}

	pool->close();
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
		chrono::seconds{30});

	// Commit path: row must persist.
	block_on(
		fx->reader,
		move(with_transaction(
				 *conn,
				 TxOptions{},
				 [](Connection &c) -> Task<void> { co_await c.query("INSERT INTO tx_test VALUES (1, 'committed')"); }))
			.flow(),
		chrono::seconds{30});
	{
		auto r = block_on(fx->reader, conn->query("SELECT v FROM tx_test WHERE id = 1"), chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<string>(0) == "committed");
	}

	// Rollback path: thrown exception must roll back the INSERT.
	try {
		block_on(
			fx->reader,
			move(with_transaction(
					 *conn,
					 TxOptions{},
					 [](Connection &c) -> Task<void> {
						 co_await c.query("INSERT INTO tx_test VALUES (2, 'rolledback')");
						 throw runtime_error{"deliberate"};
					 }))
				.flow(),
			chrono::seconds{30});
		FAIL("expected exception");
	} catch (runtime_error const &e) { CHECK(string_view{e.what()} == "deliberate"); }
	{
		auto r = block_on(fx->reader, conn->query("SELECT count(*) FROM tx_test WHERE id = 2"), chrono::seconds{30});
		REQUIRE(r.rows() == 1);
		CHECK(r[0].as<int64_t>(0) == 0);
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

	auto root = filesystem::temp_directory_path()
			  / format("conflux_db_qc_int_{}", chrono::steady_clock::now().time_since_epoch().count());
	filesystem::create_directories(root);
	{
		ofstream out{root / "select_two.psql"};
		out << "SELECT 2::int8";
	}

	QueryCache const qc{root};
	auto sql = qc.load_or_throw("select_two");
	REQUIRE(sql);

	auto conn = connect_or_skip(*fx, *ci);
	auto r = block_on(fx->reader, conn->query(*sql), chrono::seconds{30});
	REQUIRE(r.rows() == 1);
	CHECK(r[0].as<int64_t>(0) == 2);

	error_code ec;
	filesystem::remove_all(root, ec);
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

	atomic_flag done{};
	exception_ptr err;
	auto held = conn->query("SELECT pg_sleep(10)")
			  | then([&](Result) { done.test_and_set(memory_order_release); })
			  | on_error([&](exception_ptr const &ex) {
					err = ex;
					done.test_and_set(memory_order_release);
				});
	(void)held;

	this_thread::sleep_for(chrono::milliseconds{100});
	block_on(fx->reader, conn->cancel_inflight(), chrono::seconds{30});
	pump_until(fx->reader, done, chrono::seconds{10});

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

	QueryOptions const opts{.deadline = chrono::milliseconds{200}};
	try {
		(void)block_on(
			fx->reader,
			conn->query("SELECT pg_sleep(10)", Params{}, opts),
			chrono::seconds{30});
		FAIL("expected deadline cancellation");
	} catch (PgError const &e) {
		CHECK(e.sqlstate == "57014");
	}
}
