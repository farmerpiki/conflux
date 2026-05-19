module;
#include <libpq-fe.h>

export module conflux.pg.types;

import std;

namespace conflux::pg {

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
export using PGConnPtr = std::unique_ptr<PGconn, PGConnDeleter>;
export using PGResultPtr = std::unique_ptr<PGresult, PGResultDeleter>;
export struct PgError final : std::runtime_error {
	std::string sqlstate{};
	std::string detail{};
	std::string hint{};
	std::string where{};
	ExecStatusType status{PGRES_FATAL_ERROR};
	explicit PgError(
		std::string const &msg,
		std::string state = {},
		ExecStatusType st = PGRES_FATAL_ERROR)
		: std::runtime_error{msg}
		, sqlstate{std::move(state)}
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

} // namespace conflux::pg
