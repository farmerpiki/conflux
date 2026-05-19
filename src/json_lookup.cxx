module conflux.json;

import std;
import std.compat;
import conflux.types;

// ---------------------------------------------------------------------------
// Field accessor helpers
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline JsonError missing_err(
	std::string_view name) {
	return JsonError{
		.stage = JsonStage::lookup,
		.code = JsonIssueCode::missing_member,
		.path =
			[&] {
				JsonPath p;
				p.push_member(name);
				return p;
			}(),
		.member_name = std::string{name},
		.message = std::format("missing member: {}", name)};
}
[[nodiscard]] inline JsonError type_err(
	std::string_view name,
	JsonKind expected_kind,
	JsonKind actual) {
	return JsonError{
		.stage = JsonStage::lookup,
		.code = JsonIssueCode::wrong_kind,
		.path =
			[&] {
				JsonPath p;
				p.push_member(name);
				return p;
			}(),
		.expected_kind = expected_kind,
		.actual_kind = actual,
		.member_name = std::string{name},
		.message = std::format("member '{}' has wrong type", name)};
}

} // namespace detail
[[nodiscard]] std::expected<std::string, JsonError> require_string(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node) {
		return std::unexpected(detail::missing_err(name));
	}
	auto sv = node->as_string();
	if (!sv) {
		return std::unexpected(detail::type_err(name, JsonKind::string, node->kind()));
	}
	return std::string{*sv};
}
[[nodiscard]] std::expected<std::int64_t, JsonError> require_int(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node) {
		return std::unexpected(detail::missing_err(name));
	}
	auto v = node->as_i64();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return *v;
}
[[nodiscard]] std::expected<std::uint64_t, JsonError> require_uint(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node) {
		return std::unexpected(detail::missing_err(name));
	}
	auto v = node->as_u64();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return *v;
}
[[nodiscard]] std::expected<double, JsonError> require_double(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node) {
		return std::unexpected(detail::missing_err(name));
	}
	auto v = node->as_double();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return *v;
}
[[nodiscard]] std::expected<bool, JsonError> require_bool(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node) {
		return std::unexpected(detail::missing_err(name));
	}
	auto v = node->as_bool();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return *v;
}
[[nodiscard]] std::expected<std::optional<std::string>, JsonError> optional_string(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return std::optional<std::string>{};
	}
	auto sv = node->as_string();
	if (!sv) {
		return std::unexpected(detail::type_err(name, JsonKind::string, node->kind()));
	}
	return std::optional<std::string>{std::string{*sv}};
}
[[nodiscard]] std::expected<std::optional<std::int64_t>, JsonError> optional_int(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return std::optional<std::int64_t>{};
	}
	auto v = node->as_i64();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return std::optional<std::int64_t>{*v};
}
[[nodiscard]] std::expected<std::optional<std::uint64_t>, JsonError> optional_uint(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return std::optional<std::uint64_t>{};
	}
	auto v = node->as_u64();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return std::optional<std::uint64_t>{*v};
}
[[nodiscard]] std::expected<std::optional<double>, JsonError> optional_double(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return std::optional<double>{};
	}
	auto v = node->as_double();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return std::optional<double>{*v};
}
[[nodiscard]] std::expected<std::optional<bool>, JsonError> optional_bool(
	ObjectView const &obj,
	std::string_view name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return std::optional<bool>{};
	}
	auto v = node->as_bool();
	if (!v) {
		auto e = std::move(v).error();
		e.path.push_member(name);
		e.member_name = std::string{name};
		return std::unexpected(std::move(e));
	}
	return std::optional<bool>{*v};
}

// ---------------------------------------------------------------------------
// Schema validation
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<void, JsonError> validate(
	NodeRef root,
	NodeRef schema) {
	auto schema_obj = schema.as_object();
	if (!schema_obj) {
		return {};
	}
	auto type_node = schema_obj->find_member("type");
	if (!type_node) {
		return {};
	}
	auto type_sv = type_node->as_string();
	if (!type_sv) {
		return {};
	}
	std::string_view const expected_type = *type_sv;

	auto kind_matches = [&]() -> bool {
		if (expected_type == "object") {
			return root.kind() == JsonKind::object;
		}
		if (expected_type == "array") {
			return root.kind() == JsonKind::array;
		}
		if (expected_type == "string") {
			return root.kind() == JsonKind::string;
		}
		if (expected_type == "integer") {
			return root.kind() == JsonKind::number;
		}
		if (expected_type == "number") {
			return root.kind() == JsonKind::number;
		}
		if (expected_type == "boolean") {
			return root.kind() == JsonKind::boolean;
		}
		if (expected_type == "any") {
			return true;
		}
		return true;
	};

	if (root.is_null()) {
		auto nullable_node = schema_obj->find_member("nullable");
		if (nullable_node && nullable_node->as_bool().value_or(false)) {
			return {};
		}
	}

	if (!root.is_null() && !kind_matches()) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::wrong_kind,
				.message = std::format(
					"std::expected type '{}', got '{}'",
					expected_type,
					root.kind() == JsonKind::object  ? "object" :
					root.kind() == JsonKind::array   ? "array" :
					root.kind() == JsonKind::string  ? "string" :
					root.kind() == JsonKind::number  ? "number" :
					root.kind() == JsonKind::boolean ? "boolean" :
					root.kind() == JsonKind::null    ? "null" :
													   "unknown")});
	}

	if (expected_type == "object" && root.kind() == JsonKind::object) {
		auto obj = *root.as_object();
		auto req_node = schema_obj->find_member("required");
		if (req_node) {
			if (auto req_arr = req_node->as_array()) {
				for (NodeRef const el: req_arr->elements()) {
					if (auto name = el.as_string()) {
						if (!obj.find_member(*name)) {
							return std::unexpected(
								JsonError{
									.stage = JsonStage::decode,
									.code = JsonIssueCode::missing_member,
									.member_name = std::string{*name},
									.message = std::format("missing required member: {}", *name)});
						}
					}
				}
			}
		}
		auto props_node = schema_obj->find_member("properties");
		if (props_node) {
			if (auto props_obj = props_node->as_object()) {
				for (auto const &[name, val]: obj.members()) {
					auto field_schema = props_obj->find_member(name);
					if (field_schema) {
						auto r = validate(val, *field_schema);
						if (!r) {
							auto e = std::move(r).error();
							if (!e.member_name.has_value()) {
								e.member_name = std::string{name};
							}
							return std::unexpected(std::move(e));
						}
					}
				}
			}
		}
	}

	return {};
}
