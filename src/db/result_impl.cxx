module conflux.pg.result;

import std;

namespace conflux::pg {

template<>
std::string Row::as<std::string>(
	int c) const {
	return std::string{get(c)};
}
template<>
std::string_view Row::as<std::string_view>(
	int c) const {
	return get(c);
}
template<>
std::int64_t Row::as<std::int64_t>(
	int c) const {
	auto sv = get(c);
	std::int64_t v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = std::from_chars(first, last, v);
	if (ec != std::errc{} || p != last) {
		throw PgError{std::format("int64 parse failed: {}", sv)};
	}
	return v;
}
template<>
std::int32_t Row::as<std::int32_t>(
	int c) const {
	auto sv = get(c);
	std::int32_t v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = std::from_chars(first, last, v);
	if (ec != std::errc{} || p != last) {
		throw PgError{std::format("int32 parse failed: {}", sv)};
	}
	return v;
}
template<>
double Row::as<double>(
	int c) const {
	auto sv = get(c);
	double v = 0;
	auto const *first = sv.data();
	auto const *last = first + sv.size();
	auto [p, ec] = std::from_chars(first, last, v);
	if (ec != std::errc{} || p != last) {
		throw PgError{std::format("double parse failed: {}", sv)};
	}
	return v;
}
template<>
bool Row::as<bool>(
	int c) const {
	auto sv = get(c);
	if (sv == "t" || sv == "true" || sv == "1") {
		return true;
	}
	if (sv == "f" || sv == "false" || sv == "0") {
		return false;
	}
	throw PgError{std::format("bool parse failed: {}", sv)};
}
template<class T>
std::optional<T> Row::as_opt(
	int c) const {
	if (is_null(c)) {
		return std::nullopt;
	}
	try {
		return as<T>(c);
	} catch (PgError const &) { return std::nullopt; }
}
template<class T>
std::optional<T> Row::as_opt(
	Column c) const {
	return as_opt<T>(checked_column(c));
}
template std::optional<std::string> Row::as_opt<std::string>(int) const;
template std::optional<std::string_view> Row::as_opt<std::string_view>(int) const;
template std::optional<std::int64_t> Row::as_opt<std::int64_t>(int) const;
template std::optional<std::int32_t> Row::as_opt<std::int32_t>(int) const;
template std::optional<double> Row::as_opt<double>(int) const;
template std::optional<bool> Row::as_opt<bool>(int) const;
template std::optional<std::string> Row::as_opt<std::string>(Column) const;
template std::optional<std::string_view> Row::as_opt<std::string_view>(Column) const;
template std::optional<std::int64_t> Row::as_opt<std::int64_t>(Column) const;
template std::optional<std::int32_t> Row::as_opt<std::int32_t>(Column) const;
template std::optional<double> Row::as_opt<double>(Column) const;
template std::optional<bool> Row::as_opt<bool>(Column) const;

} // namespace conflux::pg
