module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json::detail {

[[nodiscard]] JsonError dom_policy_error(
	SV message) {
	return JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::constraint_violation, .message = S{message}};
}

[[nodiscard]] expected<void, JsonError> require_dom_storage(
	JsonDomPolicy const &policy,
	JsonDomStorageModel expected,
	SV api_name) {
	if (policy.storage == expected) {
		return {};
	}
	return unexpected(dom_policy_error(format("{} called with incompatible JsonDomPolicy storage model", api_name)));
}

} // namespace conflux::json::detail

namespace conflux::json {

[[nodiscard]] expected<Document, JsonError> parse_dom(
	SV input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return unexpected(
			detail::dom_policy_error(
				"parse_dom(string_view) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(string_view)");
		!ok) {
		return unexpected(move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return unexpected(detail::dom_policy_error("owned_move requires parse_dom(std::string&&)"));
	}
	return unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] expected<Document, JsonError> parse_dom(
	S &&input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return unexpected(
			detail::dom_policy_error(
				"parse_dom(std::string&&) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(std::string&&)");
		!ok) {
		return unexpected(move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return unexpected(detail::dom_policy_error("borrowed_view is unsafe for parse_dom(std::string&&)"));
	}
	return parse_copy(move(input), policy.parse);
}

[[nodiscard]] expected<Document, JsonError> parse_dom(
	SV input,
	std::pmr::memory_resource *resource,
	JsonDomPolicy const &policy) {
	if (resource == nullptr) {
		return unexpected(detail::dom_policy_error("parse_dom(memory_resource*) requires a non-null resource"));
	}
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::caller_pmr_document,
			"parse_dom(memory_resource*)");
		!ok) {
		return unexpected(move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_move:
		return unexpected(
			detail::dom_policy_error(
				"owned_move requires a std::string&& overload; caller_pmr cannot move-own input today"));
	}
	return unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	SV input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, string_view)");
		!ok) {
		return unexpected(move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return arena.parse_borrowed_into(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return arena.parse_into(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return unexpected(detail::dom_policy_error("owned_move requires parse_dom(JsonArena&, std::string&&)"));
	}
	return unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	S &&input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, std::string&&)");
		!ok) {
		return unexpected(move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return unexpected(detail::dom_policy_error("borrowed_view is unsafe for parse_dom(JsonArena&, std::string&&)"));
	}
	return arena.parse_moved_into(move(input), policy.parse);
}

} // namespace conflux::json
