export module conflux.tests.db_module_chain_provider;

import std;
import conflux.pg;

export namespace conflux::tests::db_module_chain {

[[nodiscard]] std::optional<double> read_double(
	conflux::pg::Row row) {
	return row.as_opt<double>(0);
}

[[nodiscard]] std::optional<std::int64_t> read_i64(
	conflux::pg::Row row,
	conflux::pg::Column column) {
	return row.as_opt<std::int64_t>(column);
}

} // namespace conflux::tests::db_module_chain
