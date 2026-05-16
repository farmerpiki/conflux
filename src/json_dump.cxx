module conflux.json;

import std;
import std.compat;
import conflux.types;

expected<S, JsonError> Document::dump(
	JsonDumpOptions const &opts) const {
	S out;
	// R3 — skip the small-buffer doubling cycle. Empirically dump output
	// is roughly 1.05–1.2x the input size for compact corpora and within
	// 3x for pretty-printed; reserve from string_arena + nodes count.
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
expected<S, JsonError> ArenaDocument::dump(
	JsonDumpOptions const &opts) const {
	check_live();
	S out;
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
