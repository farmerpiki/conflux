module;
#include <libpq-fe.h>

export module conflux.db.result;

import std;
import conflux.types;
import conflux.db.types;

using namespace std;

namespace conflux::db {

namespace detail {

// PQfnumber needs a NUL-terminated C string. Postgres NAMEDATALEN is 64,
// so a 128-byte stack buffer covers any real column name without heap alloc.
// Names longer than that cannot exist in a real result, so report not-found.
inline int fnumber_sv_(
	PGresult const *res,
	string_view col) noexcept {
	constexpr size_t kStackBuf = 128;
	if (col.size() >= kStackBuf) {
		return -1;
	}
	array<char, kStackBuf> buf{};
	ranges::copy(col, buf.begin());
	buf[col.size()] = '\0';
	return ::PQfnumber(res, buf.data());
}

} // namespace detail

export class Row {
	PGresult *res_{nullptr};
	int row_{0};

public:
	Row() = default;
	Row(
		PGresult *r,
		int i) noexcept
		: res_{r}
		, row_{i} {}

	[[nodiscard]] int ncols() const noexcept { return ::PQnfields(res_); }

	[[nodiscard]] bool is_null(
		int c) const noexcept {
		return ::PQgetisnull(res_, row_, c) != 0;
	}
	[[nodiscard]] bool is_null(
		Column c) const noexcept {
		return is_null(c.idx);
	}

	[[nodiscard]] string_view get(
		int c) const noexcept {
		char const *p = ::PQgetvalue(res_, row_, c);
		auto const n = static_cast<size_t>(::PQgetlength(res_, row_, c));
		return {p != nullptr ? p : "", n};
	}
	[[nodiscard]] string_view get(
		Column c) const noexcept {
		return get(c.idx);
	}

	[[nodiscard]] int length(
		int c) const noexcept {
		return ::PQgetlength(res_, row_, c);
	}

	[[nodiscard]] string_view get(
		string_view col) const {
		int const idx = detail::fnumber_sv_(res_, col);
		if (idx < 0) {
			throw PgError{format("column not found: {}", col)};
		}
		return get(idx);
	}

	template<class T>
	[[nodiscard]] T as(int c) const;

	template<class T>
	[[nodiscard]] T as(
		Column c) const {
		return as<T>(c.idx);
	}

	template<class T>
	[[nodiscard]] optional<T> as_opt(
		int c) const {
		if (is_null(c)) {
			return nullopt;
		}
		return as<T>(c);
	}

	template<class T>
	[[nodiscard]] optional<T> as_opt(
		Column c) const {
		return as_opt<T>(c.idx);
	}

	template<class... Ts>
	[[nodiscard]] tuple<Ts...> as_tuple(
		int start = 0) const {
		return [&]<size_t... Is>(index_sequence<Is...>) {
			return tuple<Ts...>{as<Ts>(start + static_cast<int>(Is))...};
		}(make_index_sequence<sizeof...(Ts)>{});
	}
};

template<>
inline string Row::as<string>(
	int c) const {
	return string{get(c)};
}
template<>
inline string_view Row::as<string_view>(
	int c) const {
	return get(c);
}
template<>
inline i64 Row::as<i64>(
	int c) const {
	auto sv = get(c);
	i64 v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("int64 parse failed: {}", sv)};
	}
	return v;
}
template<>
inline i32 Row::as<i32>(
	int c) const {
	auto sv = get(c);
	i32 v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("int32 parse failed: {}", sv)};
	}
	return v;
}
template<>
inline double Row::as<double>(
	int c) const {
	auto sv = get(c);
	double v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = from_chars(first, last, v);
	if (ec != errc{} || p != last) {
		throw PgError{format("double parse failed: {}", sv)};
	}
	return v;
}
template<>
inline bool Row::as<bool>(
	int c) const {
	auto sv = get(c);
	if (sv == "t" || sv == "true" || sv == "1") {
		return true;
	}
	if (sv == "f" || sv == "false" || sv == "0") {
		return false;
	}
	throw PgError{format("bool parse failed: {}", sv)};
}

export class Result {
	PGResultPtr res_{};

public:
	Result() = default;
	explicit Result(
		PGResultPtr r) noexcept
		: res_{move(r)} {}

	Result(Result const &) = delete;
	Result &operator =(Result const &) = delete;
	Result(Result &&) noexcept;
	Result &operator =(Result &&) noexcept;
	~Result();

	[[nodiscard]] PGresult *raw() const noexcept { return res_.get(); }
	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(res_); }

	[[nodiscard]] ExecStatusType status() const noexcept {
		return res_ ? ::PQresultStatus(res_.get()) : PGRES_FATAL_ERROR;
	}

	[[nodiscard]] bool ok() const noexcept {
		auto const s = status();
		return s == PGRES_TUPLES_OK || s == PGRES_COMMAND_OK;
	}

	[[nodiscard]] int rows() const noexcept { return res_ ? ::PQntuples(res_.get()) : 0; }
	[[nodiscard]] int cols() const noexcept { return res_ ? ::PQnfields(res_.get()) : 0; }

	[[nodiscard]] string_view column_name(
		int c) const noexcept {
		char const *p = res_ ? ::PQfname(res_.get(), c) : nullptr;
		return p != nullptr ? string_view{p} : string_view{};
	}

	[[nodiscard]] int column_index(
		string_view name) const noexcept {
		if (!res_) {
			return -1;
		}
		return detail::fnumber_sv_(res_.get(), name);
	}

	[[nodiscard]] Column column(
		string_view name) const noexcept {
		return Column{column_index(name)};
	}

	[[nodiscard]] string_view command_tag() const noexcept {
		char const *p = res_ ? ::PQcmdTuples(res_.get()) : nullptr;
		return p != nullptr ? string_view{p} : string_view{};
	}

	[[nodiscard]] Row operator [](
		int r) const noexcept {
		return Row{res_.get(), r};
	}

	class Iterator {
		PGresult *res_{nullptr};
		int row_{0};

	public:
		using iterator_category = forward_iterator_tag;
		using value_type = Row;
		using difference_type = ptrdiff_t;
		using pointer = void;
		using reference = Row;

		Iterator() = default;
		Iterator(
			PGresult *r,
			int i) noexcept
			: res_{r}
			, row_{i} {}

		[[nodiscard]] Row operator *() const noexcept { return Row{res_, row_}; }

		Iterator &operator ++() noexcept {
			++row_;
			return *this;
		}
		Iterator operator ++(
			int) noexcept {
			auto t = *this;
			++row_;
			return t;
		}
		[[nodiscard]] bool operator ==(
			Iterator const &o) const noexcept {
			return row_ == o.row_;
		}
	};

	[[nodiscard]] Iterator begin() const noexcept { return Iterator{res_.get(), 0}; }
	[[nodiscard]] Iterator end() const noexcept { return Iterator{res_.get(), rows()}; }
};

Result::Result(Result &&) noexcept = default;
Result &Result::operator =(Result &&) noexcept = default;
Result::~Result() = default;

} // namespace conflux::db
