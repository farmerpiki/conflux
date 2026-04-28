// Plain TU — not a module unit. Catch2 + std imports.
#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

import std;
import conflux.db;

using namespace std;
using namespace conflux::db;

namespace {

struct TempDir {
	filesystem::path path;

	TempDir() {
		path = filesystem::temp_directory_path()
			 / format("conflux_db_test_{}", chrono::steady_clock::now().time_since_epoch().count());
		filesystem::create_directories(path);
	}

	~TempDir() {
		error_code ec;
		filesystem::remove_all(path, ec);
	}

	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(TempDir &&) = delete;
	TempDir &operator =(TempDir &&) = delete;

	void write(
		string_view name,
		string_view content) const {
		ofstream out{path / name};
		out << content;
	}
};

PGresult *make_text_result(
	vector<vector<optional<string>>> const &rows,
	vector<string> const &cols) {
	vector<char const *> field_names;
	field_names.reserve(cols.size());
	for (auto const &c: cols) {
		field_names.push_back(c.c_str());
	}
	auto *res = ::PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
	REQUIRE(res != nullptr);
	auto const ncols = static_cast<int>(cols.size());

	vector<PGresAttDesc> attrs(cols.size());
	for (size_t i = 0; i < cols.size(); ++i) {
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

	for (size_t r = 0; r < rows.size(); ++r) {
		auto const &row = rows[r];
		for (size_t c = 0; c < row.size(); ++c) {
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

	p.add("hello").add_null().add(int64_t{42}).add(true);
	CHECK(p.count() == 4);

	auto const *vals = p.values();
	REQUIRE(vals != nullptr);
	CHECK(string_view{vals[0]} == "hello");
	CHECK(vals[1] == nullptr);
	CHECK(string_view{vals[2]} == "42");
	CHECK(string_view{vals[3]} == "t");

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
	CHECK(string_view{vals[0]} == "f");
	CHECK(string_view{vals[1]} == "2.5");
	CHECK(string_view{vals[2]} == R"({"k":1})");
}

TEST_CASE(
	"db: Params values() pointer table refreshes after rebuild",
	"[db][unit]") {
	Params p;
	p.add("first");
	auto const *v1 = p.values();
	REQUIRE(v1 != nullptr);
	CHECK(string_view{v1[0]} == "first");
	p.add("second");
	auto const *v2 = p.values();
	REQUIRE(v2 != nullptr);
	CHECK(string_view{v2[0]} == "first");
	CHECK(string_view{v2[1]} == "second");
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
	CHECK(u->find("ON CONFLICT") != string::npos);

	qc.clear();
	auto c = qc.load_or_throw("select_one");
	REQUIRE(c);
	CHECK(*c == "SELECT 1");
	CHECK(c.get() != a.get()); // new buffer post-clear

	CHECK_THROWS_AS(qc.load_or_throw("does_not_exist"), filesystem::filesystem_error);
	CHECK_THROWS_AS(qc.load_or_throw("../outside"), invalid_argument);
	CHECK_THROWS_AS(qc.load_or_throw("nested/query"), invalid_argument);
	CHECK_THROWS_AS(qc.load_or_throw(""), invalid_argument);
}

TEST_CASE(
	"db: Row::as<T> text parsers",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{string{"42"},   string{"3.5"}, string{"t"},      nullopt,    string{},         string{"abc"}},
			{string{"-7"}, string{"-0.25"}, string{"f"}, string{"hi"}, string{"x"}, string{"99999999999"}}
    },
		{"i", "d", "b", "nul", "empty", "txt"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	REQUIRE(r.ok());
	REQUIRE(r.rows() == 2);
	REQUIRE(r.cols() == 6);

	auto row0 = r[0];
	CHECK(row0.as<int64_t>(0) == 42);
	CHECK(row0.as<double>(1) == 3.5);
	CHECK(row0.as<bool>(2));
	CHECK(row0.is_null(3));
	CHECK(row0.get(4).empty());
	CHECK(row0.as<string>(5) == "abc");
	CHECK(row0.as<string_view>(5) == "abc");

	auto row1 = r[1];
	CHECK(row1.as<int64_t>(0) == -7);
	CHECK(row1.as<double>(1) == -0.25);
	CHECK_FALSE(row1.as<bool>(2));
	CHECK(row1.as<string>(3) == "hi");

	CHECK_THROWS_AS(row0.as<int64_t>(5), PgError); // "abc"
	CHECK_THROWS_AS(row1.as<double>(4), PgError); // "x"
	CHECK_THROWS_AS(row0.as<int64_t>(4), PgError); // empty

	CHECK(r.column_index("d") == 1);
	CHECK(r.column_index("missing") < 0);
	CHECK(r.column_name(0) == "i");
}

TEST_CASE(
	"db: Row::as<T> rejects partial text parses",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{string{"42x"}, string{"3.5ms"}, string{"maybe"}}
    },
		{"i", "d", "b"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];

	CHECK_THROWS_AS(row.as<int64_t>(0), PgError);
	CHECK_THROWS_AS(row.as<int32_t>(0), PgError);
	CHECK_THROWS_AS(row.as<double>(1), PgError);
	CHECK_THROWS_AS(row.as<bool>(2), PgError);
}

TEST_CASE(
	"db: Result iteration",
	"[db][unit]") {
	auto *raw = make_text_result({{string{"1"}}, {string{"2"}}, {string{"3"}}}, {"v"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	int sum = 0;
	for (auto row: r) {
		sum += static_cast<int>(row.as<int64_t>(0));
	}
	CHECK(sum == 6);
}

TEST_CASE(
	"db: Row::get(col) throws on missing column",
	"[db][unit]") {
	auto *raw = make_text_result({{string{"x"}}}, {"only"});
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
			{string{"42"}, nullopt, string{"7"}}
    },
		{"a", "b", "c"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];
	CHECK(row.as_opt<int64_t>(0) == optional<int64_t>{42});
	CHECK(row.as_opt<int64_t>(1) == nullopt);
	CHECK(row.as_opt<int64_t>(2) == optional<int64_t>{7});
}

TEST_CASE(
	"db: Row::as_tuple sequential unpack",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{string{"1"}, string{"hello"}, string{"t"}}
    },
		{"id", "name", "flag"});
	REQUIRE(raw != nullptr);
	Result const r{PGResultPtr{raw}};
	auto row = r[0];
	auto [id, name, flag] = row.as_tuple<int64_t, string, bool>();
	CHECK(id == 1);
	CHECK(name == "hello");
	CHECK(flag);
}

TEST_CASE(
	"db: Result::column + Row Column overloads",
	"[db][unit]") {
	auto *raw = make_text_result(
		{
			{string{"99"}, string{"world"}}
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
	CHECK(row.as<int64_t>(c_num) == 99);
	CHECK(row.as<string>(c_str) == "world");
	CHECK(row.get(c_num) == "99");
	CHECK_FALSE(row.is_null(c_num));
	CHECK(row.as_opt<string>(c_str) == optional<string>{"world"});
}
