// Plain TU — not a module unit. Catch2 + std imports.
#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

import std;
import conflux.types;
import conflux.work;
import conflux.db;
import conflux.pg;

using namespace conflux::db;
namespace root = conflux::work::root;

static_assert(std::same_as<conflux::pg::ConnectParams, conflux::db::ConnectParams>);
static_assert(std::same_as<conflux::pg::Connection, conflux::db::Connection>);
static_assert(std::same_as<conflux::pg::Pool, conflux::db::Pool>);
static_assert(std::same_as<conflux::pg::Result, conflux::db::Result>);
static_assert(std::same_as<conflux::pg::PgError, conflux::db::PgError>);

namespace {

struct TempDir {
	fs::path path;
	TempDir() {
		path = fs::temp_directory_path()
			 / format("conflux_db_test_{}", std::chrono::steady_clock::now().time_since_epoch().count());
		fs::create_directories(path);
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(TempDir &&) = delete;
	TempDir &operator =(TempDir &&) = delete;
	void write(
		std::string_view name,
		std::string_view content) const {
		std::ofstream out{path / name};
		out << content;
	}
};
PGresult *make_text_result(
	std::vector<std::vector<std::optional<std::string>>> const &rows,
	std::vector<std::string> const &cols) {
	std::vector<char const *> field_names;
	field_names.reserve(cols.size());
	for (auto const &c: cols) {
		field_names.push_back(c.c_str());
	}
	auto *res = ::PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
	REQUIRE(res != nullptr);
	auto const ncols = static_cast<int>(cols.size());

	std::vector<PGresAttDesc> attrs(cols.size());
	for (std::size_t i = 0; i < cols.size(); ++i) {
		attrs[i] = PGresAttDesc{
			.name = const_cast<char *>(field_names[i]),
			.tableid = 0,
			.columnid = 0,
			.format = 0,
			.typid = 25, // TEXTOID
			.typlen = -1,
			.atttypmod = -1,
		};
	}
	REQUIRE(::PQsetResultAttrs(res, ncols, attrs.data()) == 1);

	for (std::size_t r = 0; r < rows.size(); ++r) {
		auto const &row = rows[r];
		for (std::size_t c = 0; c < row.size(); ++c) {
			if (row[c]) {
				REQUIRE(
					::PQsetvalue(
						res,
						static_cast<int>(r),
						static_cast<int>(c),
						const_cast<char *>(row[c]->c_str()),
						static_cast<int>(row[c]->size()))
					== 1);
			} else {
				REQUIRE(::PQsetvalue(res, static_cast<int>(r), static_cast<int>(c), nullptr, -1) == 1);
			}
		}
	}
	return res;
}

} // namespace
TEST_CASE(
	"db: Params builder add/null/count",
	"[db][unit]") {
	Params p;
	CHECK(p.count() == 0);
	CHECK(p.values() == nullptr);
	CHECK(p.lengths() == nullptr);
	CHECK(p.formats() == nullptr);
	CHECK(p.types() == nullptr);

	p.add("hello").add_null().add(std::int64_t{42}).add(true);
	CHECK(p.count() == 4);

	auto const *vals = p.values();
	REQUIRE(vals != nullptr);
	CHECK(std::string_view{vals[0]} == "hello");
	CHECK(vals[1] == nullptr);
	CHECK(std::string_view{vals[2]} == "42");
	CHECK(std::string_view{vals[3]} == "t");

	auto const *lens = p.lengths();
	REQUIRE(lens != nullptr);
	CHECK(lens[0] == 5);
	CHECK(lens[2] == 2);

	auto const *fmts = p.formats();
	REQUIRE(fmts != nullptr);
	CHECK(fmts[0] == 0); // text v1
	CHECK(p.result_format() == 0);
}
TEST_CASE(
	"db: Params bool/double/json",
	"[db][unit]") {
	Params p;
	p.add(false).add(2.5).add_json(R"({"k":1})");
	auto const *vals = p.values();
	REQUIRE(vals != nullptr);
	CHECK(std::string_view{vals[0]} == "f");
	CHECK(std::string_view{vals[1]} == "2.5");
	CHECK(std::string_view{vals[2]} == R"({"k":1})");
}
TEST_CASE(
	"db: Params values() pointer table refreshes after rebuild",
	"[db][unit]") {
	Params p;
	p.add("first");
	auto const *v1 = p.values();
	REQUIRE(v1 != nullptr);
	CHECK(std::string_view{v1[0]} == "first");
	p.add("second");
	auto const *v2 = p.values();
	REQUIRE(v2 != nullptr);
	CHECK(std::string_view{v2[0]} == "first");
	CHECK(std::string_view{v2[1]} == "second");
}
TEST_CASE(
	"db: Params overflow path (>kInline params)",
	"[db][unit]") {
	Params p;
	for (int i = 0; i < 12; ++i) {
		p.add(static_cast<std::int64_t>(i));
	}
	CHECK(p.count() == 12);
	auto const *vals = p.values();
	REQUIRE(vals != nullptr);
	CHECK(std::string_view{vals[0]} == "0");
	CHECK(std::string_view{vals[7]} == "7");
	CHECK(std::string_view{vals[11]} == "11");
}
TEST_CASE(
	"db: Params binary bind",
	"[db][unit]") {
	using namespace oids;
	Params p;
	p.add_binary(std::int64_t{0x0102030405060708LL});
	p.add_binary(std::int32_t{0x01020304});
	p.add_binary(3.14);

	CHECK(p.count() == 3);

	auto const *fmts = p.formats();
	REQUIRE(fmts != nullptr);
	CHECK(fmts[0] == 1);
	CHECK(fmts[1] == 1);
	CHECK(fmts[2] == 1);

	auto const *lens = p.lengths();
	REQUIRE(lens != nullptr);
	CHECK(lens[0] == 8);
	CHECK(lens[1] == 4);
	CHECK(lens[2] == 8);

	auto const *typs = p.types();
	REQUIRE(typs != nullptr);
	CHECK(typs[0] == int8);
	CHECK(typs[1] == int4);
	CHECK(typs[2] == float8);

	// Verify big-endian wire encoding for int64
	auto const *vals = p.values();
	REQUIRE(vals != nullptr);
	std::array<std::uint8_t, 8> wire{};
	memcpy(wire.data(), vals[0], 8);
	CHECK(wire[0] == 0x01);
	CHECK(wire[1] == 0x02);
	CHECK(wire[7] == 0x08);
}
TEST_CASE(
	"db: Params result_format setter",
	"[db][unit]") {
	Params p;
	CHECK(p.result_format() == 0);
	p.result_format(1);
	CHECK(p.result_format() == 1);
}
TEST_CASE(
	"db: Params copy semantics",
	"[db][unit]") {
	Params orig;
	orig.add("hello").add(std::int64_t{42});
	Params const copy{orig};
	CHECK(copy.count() == 2);
	auto const *vals = copy.values();
	REQUIRE(vals != nullptr);
	CHECK(std::string_view{vals[0]} == "hello");
	CHECK(std::string_view{vals[1]} == "42");
	// Mutating orig does not affect copy
	orig.add("extra");
	CHECK(orig.count() == 3);
	CHECK(copy.count() == 2);
}
TEST_CASE(
	"db: PgError SQLSTATE classifiers",
	"[db][unit]") {
	{
		PgError e{"unique"};
		e.sqlstate = "23505";
		CHECK(e.is_unique_violation());
		CHECK_FALSE(e.is_serialization());
		CHECK_FALSE(e.is_deadlock());
		CHECK_FALSE(e.is_connection_lost());
	}
	{
		PgError e{"serialization"};
		e.sqlstate = "40001";
		CHECK(e.is_serialization());
	}
	{
		PgError e{"deadlock"};
		e.sqlstate = "40P01";
		CHECK(e.is_deadlock());
	}
	{
		PgError e{"conn lost"};
		e.sqlstate = "08006";
		CHECK(e.is_connection_lost());
	}
	{
		PgError e{"conn lost short"};
		e.sqlstate = "08";
		CHECK(e.is_connection_lost());
	}
	{
		PgError e{"other"};
		e.sqlstate = "42703";
		CHECK_FALSE(e.is_unique_violation());
		CHECK_FALSE(e.is_connection_lost());
	}
}
TEST_CASE(
	"db: pipeline closed state rejects queued work",
	"[db][unit]") {
	Pipeline pipeline;
	auto query = pipeline.query("SELECT 1::int8");
	auto out = root::blocking_join(move(query));
	REQUIRE(out.is_failure());

	auto sync = pipeline.sync();
	auto sync_out = root::blocking_join(move(sync));
	REQUIRE(sync_out.is_failure());
}
TEST_CASE(
	"db: QueryCache load + cache + miss + clear",
	"[db][unit]") {
	TempDir const td;
	td.write("select_one.psql", "SELECT 1");
	td.write(
		"upsert_user.psql",
		"INSERT INTO users (id) VALUES ($1) "
		"ON CONFLICT (id) DO UPDATE SET id = EXCLUDED.id");
	QueryCache qc{td.path};

	auto a = qc.load_or_throw("select_one");
	REQUIRE(a);
	CHECK(*a == "SELECT 1");

	auto b = qc.load_or_throw("select_one");
	REQUIRE(b);
	CHECK(a.get() == b.get()); // pointer identity → cached

	auto u = qc.load_or_throw("upsert_user");
	REQUIRE(u);
	CHECK(u->find("ON CONFLICT") != std::string::npos);

	qc.clear();
	auto c = qc.load_or_throw("select_one");
	REQUIRE(c);
	CHECK(*c == "SELECT 1");
	CHECK(c.get() != a.get()); // new buffer post-clear

	CHECK_THROWS_AS(qc.load_or_throw("does_not_exist"), fs::filesystem_error);
	CHECK_THROWS_AS(qc.load_or_throw("../outside"), std::invalid_argument);
	CHECK_THROWS_AS(qc.load_or_throw("nested/query"), std::invalid_argument);
	CHECK_THROWS_AS(qc.load_or_throw(""), std::invalid_argument);
}
TEST_CASE(
	"db: Row::as<T> text parsers",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{std::string{"42"},   std::string{"3.5"}, std::string{"t"}, nullopt,    std::string{},         std::string{"abc"}},
			{std::string{"-7"}, std::string{"-0.25"}, std::string{"f"}, std::string{"hi"}, std::string{"x"}, std::string{"99999999999"}}
    },
		{"i", "d", "b", "nul", "empty", "txt"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	REQUIRE(r.ok());
	REQUIRE(r.rows() == 2);
	REQUIRE(r.cols() == 6);

	auto row0 = r[0];
	CHECK(row0.as<std::int64_t>(0) == 42);
	CHECK(row0.as<double>(1) == 3.5);
	CHECK(row0.as<bool>(2));
	CHECK(row0.is_null(3));
	CHECK(row0.get(4).empty());
	CHECK(row0.as<std::string>(5) == "abc");
	CHECK(row0.as<std::string_view>(5) == "abc");

	auto row1 = r[1];
	CHECK(row1.as<std::int64_t>(0) == -7);
	CHECK(row1.as<double>(1) == -0.25);
	CHECK_FALSE(row1.as<bool>(2));
	CHECK(row1.as<std::string>(3) == "hi");

	CHECK_THROWS_AS(row0.as<std::int64_t>(5), PgError); // "abc"
	CHECK_THROWS_AS(row1.as<double>(4), PgError); // "x"
	CHECK_THROWS_AS(row0.as<std::int64_t>(4), PgError); // empty

	CHECK(r.column_index("d") == 1);
	CHECK(r.column_index("missing") < 0);
	CHECK(r.column_name(0) == "i");
}
TEST_CASE(
	"db: Row::as<T> rejects partial text parses",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{std::string{"42x"}, std::string{"3.5ms"}, std::string{"maybe"}}
    },
		{"i", "d", "b"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];

	CHECK_THROWS_AS(row.as<std::int64_t>(0), PgError);
	CHECK_THROWS_AS(row.as<std::int32_t>(0), PgError);
	CHECK_THROWS_AS(row.as<double>(1), PgError);
	CHECK_THROWS_AS(row.as<bool>(2), PgError);
}
TEST_CASE(
	"db: Result iteration",
	"[db][unit]") {
	auto *raw = make_text_result({{std::string{"1"}}, {std::string{"2"}}, {std::string{"3"}}}, {"v"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	int sum = 0;
	for (auto row: r) {
		sum += static_cast<int>(row.as<std::int64_t>(0));
	}
	CHECK(sum == 6);
}
TEST_CASE(
	"db: Row::get(col) throws on missing column",
	"[db][unit]") {
	auto *raw = make_text_result({{std::string{"x"}}}, {"only"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];
	CHECK(row.get("only") == "x");
	CHECK_THROWS_AS(row.get("nope"), PgError);
}
TEST_CASE(
	"db: Row::as_opt null/value",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{std::string{"42"}, nullopt, std::string{"7"}}
    },
		{"a", "b", "c"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];
	CHECK(row.as_opt<std::int64_t>(0) == std::optional<std::int64_t>{42});
	CHECK(row.as_opt<std::int64_t>(1) == nullopt);
	CHECK(row.as_opt<std::int64_t>(2) == std::optional<std::int64_t>{7});
}
TEST_CASE(
	"db: Row::as_tuple sequential unpack",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{std::string{"1"}, std::string{"hello"}, std::string{"t"}}
    },
		{"id", "name", "flag"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];
	auto [id, name, flag] = row.as_tuple<std::int64_t, std::string, bool>();
	CHECK(id == 1);
	CHECK(name == "hello");
	CHECK(flag);
}
TEST_CASE(
	"db: Result::column + Row Column overloads",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{std::string{"99"}, std::string{"world"}}
    },
		{"num", "str"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};

	auto c_num = r.column("num");
	auto c_str = r.column("str");
	auto c_bad = r.column("missing");

	CHECK(static_cast<bool>(c_num));
	CHECK(static_cast<bool>(c_str));
	CHECK_FALSE(static_cast<bool>(c_bad));

	auto row = r[0];
	CHECK(row.as<std::int64_t>(c_num) == 99);
	CHECK(row.as<std::string>(c_str) == "world");
	CHECK(row.get(c_num) == "99");
	CHECK_FALSE(row.is_null(c_num));
	CHECK(row.as_opt<std::string>(c_str) == std::optional<std::string>{"world"});
}
TEST_CASE(
	"db: StatementCache stable_name deterministic + length",
	"[db][unit]") {
	auto const n1 = StatementCache::stable_name("SELECT 1");
	auto const n2 = StatementCache::stable_name("SELECT 1");
	auto const n3 = StatementCache::stable_name("SELECT 2");

	CHECK(n1 == n2); // same SQL → same name
	CHECK(n1 != n3); // different SQL → different name
	CHECK(n1.size() == 15); // "p_" + 13 base32 chars
	CHECK(n1.starts_with("p_"));
}
TEST_CASE(
	"db: StatementCache get caches by SQL text",
	"[db][unit]") {
	StatementCache sc;
	auto e1 = sc.get("SELECT 1");
	auto e2 = sc.get("SELECT 1");
	auto e3 = sc.get("SELECT 2");

	CHECK(e1.get() == e2.get()); // pointer identity → same entry
	CHECK(e1.get() != e3.get());
	CHECK(e1->name == StatementCache::stable_name("SELECT 1"));
	CHECK(*e1->sql == "SELECT 1");
}
