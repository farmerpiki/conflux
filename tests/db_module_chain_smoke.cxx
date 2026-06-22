#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

import std;
import conflux.pg;
import conflux.tests.db_module_chain_provider;

namespace {

PGresult *make_chain_result() {
	std::array<char const *, 2> names{"d", "i"};
	auto *res = ::PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
	REQUIRE(res != nullptr);
	std::array<PGresAttDesc, 2> attrs{
		PGresAttDesc{
					 .name = const_cast<char *>(names[0]),
					 .tableid = 0,
					 .columnid = 0,
					 .format = 0,
					 .typid = 25,
					 .typlen = -1,
					 .atttypmod = -1},
		PGresAttDesc{
					 .name = const_cast<char *>(names[1]),
					 .tableid = 0,
					 .columnid = 1,
					 .format = 0,
					 .typid = 25,
					 .typlen = -1,
					 .atttypmod = -1}
    };
	REQUIRE(::PQsetResultAttrs(res, static_cast<int>(attrs.size()), attrs.data()) == 1);
	std::string d{"3.5"};
	std::string i{"42"};
	REQUIRE(::PQsetvalue(res, 0, 0, d.data(), static_cast<int>(d.size())) == 1);
	REQUIRE(::PQsetvalue(res, 0, 1, i.data(), static_cast<int>(i.size())) == 1);
	return res;
}

} // namespace

TEST_CASE(
	"db: Row as_opt works through module chain",
	"[db][module]") {
	conflux::pg::Result const result{conflux::pg::PGResultPtr{make_chain_result()}};
	auto row = result[0];
	CHECK(conflux::tests::db_module_chain::read_double(row) == std::optional<double>{3.5});
	CHECK(conflux::tests::db_module_chain::read_i64(row, result.column("i")) == std::optional<std::int64_t>{42});
}
