module;
#include <libpq-fe.h>

export module conflux.db.types;

import std;

using std::move;
using std::runtime_error;
using std::string;
using std::unique_ptr;
namespace conflux::db {

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
export struct Column {
	int idx{-1};
	[[nodiscard]] explicit operator bool() const noexcept { return idx >= 0; }
};

} // namespace conflux::db
