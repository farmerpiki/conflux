module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

namespace detail {

using conflux::json::JsonPatchOp;
using conflux::json::JsonPatchOptions;

struct PatchValue {
	using Object = std::vector<std::pair<std::string, PatchValue>>;
	using Array = std::vector<PatchValue>;
	std::variant<std::nullptr_t, bool, std::string, JsonNumberView, Array, Object> value{nullptr};
};

struct PatchToken {
	std::string text;
	bool append{};
};

struct PatchOperation {
	JsonPatchOp op{};
	std::string op_text;
	std::string path_text;
	std::string from_text;
	std::vector<PatchToken> path;
	std::vector<PatchToken> from;
	std::optional<PatchValue> value;
};

[[nodiscard]] bool count_patch_nodes_within(
	PatchValue const &value,
	std::size_t limit,
	std::size_t &count) {
	if (count >= limit) {
		return false;
	}
	++count;
	if (auto const *arr = std::get_if<PatchValue::Array>(std::addressof(value.value))) {
		for (auto const &child: *arr) {
			if (!count_patch_nodes_within(child, limit, count)) {
				return false;
			}
		}
	} else if (auto const *obj = std::get_if<PatchValue::Object>(std::addressof(value.value))) {
		for (auto const &[_, child]: *obj) {
			if (!count_patch_nodes_within(child, limit, count)) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] JsonError patch_error(
	JsonIssueCode code,
	std::string message,
	std::optional<std::size_t> op_index = std::nullopt,
	std::string_view op = {},
	std::string_view path = {},
	std::string_view from = {}) {
	JsonError err{.stage = JsonStage::json_patch, .code = code, .message = std::move(message)};
	if (op_index) {
		err.operation_index = *op_index;
	}
	if (!op.empty()) {
		err.operation = std::string{op};
	}
	if (!path.empty()) {
		err.pointer = std::string{path};
	}
	if (!from.empty()) {
		err.from_pointer = std::string{from};
	}
	return err;
}

[[nodiscard]] std::expected<void, JsonError> check_result_node_limit(
	PatchValue const &candidate,
	JsonPatchOptions const &opts,
	std::optional<std::size_t> op_index = std::nullopt,
	std::string_view op = {},
	std::string_view path = {},
	std::string_view from = {}) {
	std::size_t count{};
	if (!count_patch_nodes_within(candidate, opts.max_result_nodes, count)) {
		return std::unexpected(patch_error(
			JsonIssueCode::output_too_large,
			"JSON Patch result node limit exceeded",
			op_index,
			op,
			path,
			from));
	}
	return {};
}

[[nodiscard]] std::expected<std::vector<PatchToken>, JsonError> parse_patch_pointer(
	std::string_view pointer,
	JsonPatchOptions const &opts,
	std::optional<std::size_t> op_index,
	std::string_view op,
	JsonIssueCode invalid_code) {
	if (pointer.empty()) {
		return std::vector<PatchToken>{};
	}
	if (!pointer.starts_with('/')) {
		return std::unexpected(
			patch_error(invalid_code, "JSON Patch pointer must be empty or start with '/'", op_index, op, pointer));
	}
	std::vector<PatchToken> out;
	std::size_t pos = 1;
	while (pos <= pointer.size()) {
		if (out.size() >= opts.max_pointer_depth) {
			return std::unexpected(patch_error(
				JsonIssueCode::patch_pointer_too_deep,
				"JSON Patch pointer depth limit exceeded",
				op_index,
				op,
				pointer));
		}
		std::size_t slash = pointer.find('/', pos);
		if (slash == std::string_view::npos) {
			slash = pointer.size();
		}
		auto token = decode_json_pointer_token(pointer.substr(pos, slash - pos));
		if (!token) {
			return std::unexpected(
				patch_error(invalid_code, "invalid '~' escape in JSON Patch pointer", op_index, op, pointer));
		}
		bool const append = *token == "-";
		out.push_back(PatchToken{.text = std::move(*token), .append = append});
		pos = slash + 1;
	}
	return out;
}

[[nodiscard]] std::expected<std::size_t, JsonError> parse_array_index(
	PatchToken const &token,
	std::size_t size,
	bool allow_append,
	std::optional<std::size_t> op_index,
	std::string_view op,
	std::string_view path) {
	if (token.append) {
		if (allow_append) {
			return size;
		}
		return std::unexpected(patch_error(
			JsonIssueCode::patch_array_index_invalid,
			"'-' is only valid for array add",
			op_index,
			op,
			path));
	}
	if (token.text.empty() || (token.text.size() > 1 && token.text[0] == '0')) {
		return std::unexpected(
			patch_error(JsonIssueCode::patch_array_index_invalid, "invalid array index", op_index, op, path));
	}
	std::size_t index{};
	for (char const ch: token.text) {
		if (ch < '0' || ch > '9') {
			return std::unexpected(
				patch_error(JsonIssueCode::patch_array_index_invalid, "invalid array index", op_index, op, path));
		}
		auto const digit = static_cast<std::size_t>(ch - '0');
		if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
			return std::unexpected(
				patch_error(JsonIssueCode::patch_array_index_invalid, "array index overflow", op_index, op, path));
		}
		index = (index * 10U) + digit;
	}
	if (index > size || (!allow_append && index == size)) {
		return std::unexpected(
			patch_error(JsonIssueCode::patch_array_index_out_of_range, "array index out of range", op_index, op, path));
	}
	return index;
}

[[nodiscard]] std::expected<PatchValue, JsonError> patch_value_from_node(NodeRef node);

[[nodiscard]] std::expected<PatchValue::Object, JsonError> patch_object_from_node(
	ObjectView obj) {
	PatchValue::Object out;
	out.reserve(obj.size());
	for (auto const &[name, value]: obj.members()) {
		auto child = patch_value_from_node(value);
		if (!child) {
			return std::unexpected(std::move(child).error());
		}
		out.emplace_back(std::string{name}, std::move(*child));
	}
	return out;
}

[[nodiscard]] std::expected<PatchValue::Array, JsonError> patch_array_from_node(
	ArrayView arr) {
	PatchValue::Array out;
	out.reserve(arr.size());
	for (auto value: arr.elements()) {
		auto child = patch_value_from_node(value);
		if (!child) {
			return std::unexpected(std::move(child).error());
		}
		out.push_back(std::move(*child));
	}
	return out;
}

[[nodiscard]] std::expected<PatchValue, JsonError> patch_value_from_node(
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return PatchValue{};
	case JsonKind::boolean: return PatchValue{.value = *node.as_bool()};
	case JsonKind::string : return PatchValue{.value = std::string{*node.as_string()}};
	case JsonKind::number : return PatchValue{.value = *node.as_number()};
	case JsonKind::array:
		{
			auto arr = node.as_array();
			if (!arr) {
				return std::unexpected(std::move(arr).error());
			}
			auto value = patch_array_from_node(*arr);
			if (!value) {
				return std::unexpected(std::move(value).error());
			}
			return PatchValue{.value = std::move(*value)};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return std::unexpected(std::move(obj).error());
			}
			auto value = patch_object_from_node(*obj);
			if (!value) {
				return std::unexpected(std::move(value).error());
			}
			return PatchValue{.value = std::move(*value)};
		}
	}
	return std::unexpected(patch_error(JsonIssueCode::invalid_patch, "unsupported JSON kind"));
}

[[nodiscard]] PatchValue *object_member(
	PatchValue::Object &obj,
	std::string_view name) noexcept {
	for (auto &[member_name, value]: obj) {
		if (member_name == name) {
			return std::addressof(value);
		}
	}
	return nullptr;
}

[[nodiscard]] std::expected<PatchValue *, JsonError> find_patch_target(
	PatchValue &root,
	std::span<PatchToken const> path,
	std::optional<std::size_t> op_index,
	std::string_view op,
	std::string_view path_text) {
	PatchValue *cur = std::addressof(root);
	for (auto const &token: path) {
		if (auto *obj = std::get_if<PatchValue::Object>(std::addressof(cur->value))) {
			auto *member = object_member(*obj, token.text);
			if (member == nullptr) {
				return std::unexpected(patch_error(
					JsonIssueCode::patch_target_missing,
					"JSON Patch target path does not exist",
					op_index,
					op,
					path_text));
			}
			cur = member;
		} else if (auto *arr = std::get_if<PatchValue::Array>(std::addressof(cur->value))) {
			auto index = parse_array_index(token, arr->size(), false, op_index, op, path_text);
			if (!index) {
				return std::unexpected(std::move(index).error());
			}
			cur = std::addressof((*arr)[*index]);
		} else {
			return std::unexpected(patch_error(
				JsonIssueCode::patch_parent_missing,
				"JSON Patch parent is not a container",
				op_index,
				op,
				path_text));
		}
	}
	return cur;
}

[[nodiscard]] std::expected<void, JsonError> patch_add(
	PatchValue &root,
	std::span<PatchToken const> path,
	PatchValue value,
	std::optional<std::size_t> op_index,
	std::string_view op,
	std::string_view path_text) {
	if (path.empty()) {
		root = std::move(value);
		return {};
	}
	auto parent_path = path.subspan(0, path.size() - 1);
	auto parent = find_patch_target(root, parent_path, op_index, op, path_text);
	if (!parent) {
		return std::unexpected(std::move(parent).error());
	}
	auto const &last = path.back();
	if (auto *obj = std::get_if<PatchValue::Object>(std::addressof((*parent)->value))) {
		if (auto *member = object_member(*obj, last.text)) {
			*member = std::move(value);
		} else {
			obj->emplace_back(last.text, std::move(value));
		}
		return {};
	}
	if (auto *arr = std::get_if<PatchValue::Array>(std::addressof((*parent)->value))) {
		auto index = parse_array_index(last, arr->size(), true, op_index, op, path_text);
		if (!index) {
			return std::unexpected(std::move(index).error());
		}
		arr->insert(arr->begin() + static_cast<std::ptrdiff_t>(*index), std::move(value));
		return {};
	}
	return std::unexpected(patch_error(
		JsonIssueCode::patch_parent_missing,
		"JSON Patch parent is not a container",
		op_index,
		op,
		path_text));
}

[[nodiscard]] std::expected<std::optional<PatchValue>, JsonError> patch_remove(
	PatchValue &root,
	std::span<PatchToken const> path,
	bool allow_missing,
	bool return_removed,
	std::optional<std::size_t> op_index,
	std::string_view op,
	std::string_view path_text) {
	if (path.empty()) {
		return std::unexpected(patch_error(
			JsonIssueCode::patch_remove_document_root,
			"JSON Patch remove of document root is rejected",
			op_index,
			op,
			path_text));
	}
	auto parent = find_patch_target(root, path.subspan(0, path.size() - 1), op_index, op, path_text);
	if (!parent) {
		if (allow_missing) {
			return std::optional<PatchValue>{};
		}
		return std::unexpected(std::move(parent).error());
	}
	auto const &last = path.back();
	if (auto *obj = std::get_if<PatchValue::Object>(std::addressof((*parent)->value))) {
		for (auto it = obj->begin(); it != obj->end(); ++it) {
			if (it->first == last.text) {
				std::optional<PatchValue> removed;
				if (return_removed) {
					removed = std::move(it->second);
				}
				obj->erase(it);
				return removed;
			}
		}
		if (allow_missing) {
			return std::optional<PatchValue>{};
		}
		return std::unexpected(patch_error(
			JsonIssueCode::patch_target_missing,
			"JSON Patch target path does not exist",
			op_index,
			op,
			path_text));
	}
	if (auto *arr = std::get_if<PatchValue::Array>(std::addressof((*parent)->value))) {
		auto index = parse_array_index(last, arr->size(), false, op_index, op, path_text);
		if (!index) {
			if (allow_missing) {
				return std::optional<PatchValue>{};
			}
			return std::unexpected(std::move(index).error());
		}
		std::optional<PatchValue> removed;
		if (return_removed) {
			removed = std::move((*arr)[*index]);
		}
		arr->erase(arr->begin() + static_cast<std::ptrdiff_t>(*index));
		return removed;
	}
	return std::unexpected(patch_error(
		JsonIssueCode::patch_parent_missing,
		"JSON Patch parent is not a container",
		op_index,
		op,
		path_text));
}

[[nodiscard]] bool patch_path_is_child(
	std::span<PatchToken const> parent,
	std::span<PatchToken const> child) noexcept {
	if (child.size() <= parent.size()) {
		return false;
	}
	return std::ranges::equal(parent, child.first(parent.size()), [](PatchToken const &lhs, PatchToken const &rhs) {
		return lhs.append == rhs.append && lhs.text == rhs.text;
	});
}

[[nodiscard]] std::expected<void, JsonError> write_patch_value(ValueBuilder &out, PatchValue const &value);
[[nodiscard]] std::expected<void, JsonError> write_patch_value(ArrayBuilder &out, PatchValue const &value);

[[nodiscard]] std::expected<void, JsonError> write_patch_value(
	ObjectBuilder &out,
	std::string_view name,
	PatchValue const &value) {
	if (std::holds_alternative<std::nullptr_t>(value.value)) {
		return out.insert_null(name);
	}
	if (auto v = std::get_if<bool>(std::addressof(value.value))) {
		return out.insert_bool(name, *v);
	}
	if (auto v = std::get_if<std::string>(std::addressof(value.value))) {
		return out.insert_string(name, *v);
	}
	if (auto v = std::get_if<JsonNumberView>(std::addressof(value.value))) {
		return out.insert_number(name, v->lexeme());
	}
	if (auto v = std::get_if<PatchValue::Array>(std::addressof(value.value))) {
		auto arr = out.insert_array(name);
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		for (auto const &child: *v) {
			if (auto ok = write_patch_value(*arr, child); !ok) {
				return ok;
			}
		}
		std::move(*arr).commit();
		return {};
	}
	auto obj = out.insert_object(name);
	if (!obj) {
		return std::unexpected(std::move(obj).error());
	}
	for (auto const &[member_name, child]: std::get<PatchValue::Object>(value.value)) {
		if (auto ok = write_patch_value(*obj, member_name, child); !ok) {
			return ok;
		}
	}
	std::move(*obj).commit();
	return {};
}

[[nodiscard]] std::expected<void, JsonError> write_patch_value(
	ArrayBuilder &out,
	PatchValue const &value) {
	if (std::holds_alternative<std::nullptr_t>(value.value)) {
		return out.append_null();
	}
	if (auto v = std::get_if<bool>(std::addressof(value.value))) {
		return out.append_bool(*v);
	}
	if (auto v = std::get_if<std::string>(std::addressof(value.value))) {
		return out.append_string(*v);
	}
	if (auto v = std::get_if<JsonNumberView>(std::addressof(value.value))) {
		return out.append_number(v->lexeme());
	}
	if (auto v = std::get_if<PatchValue::Array>(std::addressof(value.value))) {
		auto arr = out.append_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		for (auto const &child: *v) {
			if (auto ok = write_patch_value(*arr, child); !ok) {
				return ok;
			}
		}
		std::move(*arr).commit();
		return {};
	}
	auto obj = out.append_object();
	if (!obj) {
		return std::unexpected(std::move(obj).error());
	}
	for (auto const &[member_name, child]: std::get<PatchValue::Object>(value.value)) {
		if (auto ok = write_patch_value(*obj, member_name, child); !ok) {
			return ok;
		}
	}
	std::move(*obj).commit();
	return {};
}

[[nodiscard]] std::expected<void, JsonError> write_patch_value(
	ValueBuilder &out,
	PatchValue const &value) {
	if (std::holds_alternative<std::nullptr_t>(value.value)) {
		return out.set_null();
	}
	if (auto v = std::get_if<bool>(std::addressof(value.value))) {
		return out.set_bool(*v);
	}
	if (auto v = std::get_if<std::string>(std::addressof(value.value))) {
		return out.set_string(*v);
	}
	if (auto v = std::get_if<JsonNumberView>(std::addressof(value.value))) {
		return out.set_number(v->lexeme());
	}
	if (auto v = std::get_if<PatchValue::Array>(std::addressof(value.value))) {
		auto arr = out.begin_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		for (auto const &child: *v) {
			if (auto ok = write_patch_value(*arr, child); !ok) {
				return ok;
			}
		}
		std::move(*arr).commit();
		return {};
	}
	auto obj = out.begin_object();
	if (!obj) {
		return std::unexpected(std::move(obj).error());
	}
	for (auto const &[member_name, child]: std::get<PatchValue::Object>(value.value)) {
		if (auto ok = write_patch_value(*obj, member_name, child); !ok) {
			return ok;
		}
	}
	std::move(*obj).commit();
	return {};
}

[[nodiscard]] std::expected<JsonPatchOp, JsonError> parse_patch_op(
	std::string_view op,
	std::size_t index) {
	if (op == "add") {
		return JsonPatchOp::add;
	}
	if (op == "remove") {
		return JsonPatchOp::remove;
	}
	if (op == "replace") {
		return JsonPatchOp::replace;
	}
	if (op == "move") {
		return JsonPatchOp::move;
	}
	if (op == "copy") {
		return JsonPatchOp::copy;
	}
	if (op == "test") {
		return JsonPatchOp::test;
	}
	return std::unexpected(patch_error(JsonIssueCode::patch_op_unknown, "unknown JSON Patch operation", index, op));
}

[[nodiscard]] std::expected<std::vector<PatchOperation>, JsonError> parse_patch_operations(
	NodeRef patch,
	JsonPatchOptions const &opts,
	bool capture_values) {
	auto arr = patch.as_array();
	if (!arr) {
		return std::unexpected(patch_error(JsonIssueCode::invalid_patch, "JSON Patch document must be an array"));
	}
	if (arr->size() > opts.max_operations) {
		return std::unexpected(
			patch_error(JsonIssueCode::patch_too_many_operations, "JSON Patch operation limit exceeded"));
	}
	std::vector<PatchOperation> operations;
	operations.reserve(arr->size());
	std::size_t index = 0;
	for (auto op_node: arr->elements()) {
		auto obj = op_node.as_object();
		if (!obj) {
			return std::unexpected(
				patch_error(JsonIssueCode::invalid_patch, "JSON Patch operation must be an object", index));
		}
		auto op_member = obj->find_member("op");
		if (!op_member) {
			return std::unexpected(
				patch_error(JsonIssueCode::patch_op_missing, "JSON Patch operation is missing 'op'", index));
		}
		auto op_text = op_member->as_string();
		if (!op_text) {
			return std::unexpected(
				patch_error(JsonIssueCode::patch_op_unknown, "JSON Patch 'op' must be a string", index));
		}
		auto parsed_op = parse_patch_op(*op_text, index);
		if (!parsed_op) {
			return std::unexpected(std::move(parsed_op).error());
		}
		auto path_member = obj->find_member("path");
		if (!path_member) {
			return std::unexpected(patch_error(
				JsonIssueCode::patch_path_missing,
				"JSON Patch operation is missing 'path'",
				index,
				*op_text));
		}
		auto path_text = path_member->as_string();
		if (!path_text) {
			return std::unexpected(
				patch_error(JsonIssueCode::patch_path_invalid, "JSON Patch 'path' must be a string", index, *op_text));
		}
		auto path = parse_patch_pointer(*path_text, opts, index, *op_text, JsonIssueCode::patch_path_invalid);
		if (!path) {
			return std::unexpected(std::move(path).error());
		}
		PatchOperation op{
			.op = *parsed_op,
			.op_text = std::string{*op_text},
			.path_text = std::string{*path_text},
			.path = std::move(*path)};
		if (*parsed_op == JsonPatchOp::move || *parsed_op == JsonPatchOp::copy) {
			auto from_member = obj->find_member("from");
			if (!from_member) {
				return std::unexpected(patch_error(
					JsonIssueCode::patch_from_missing,
					"JSON Patch operation is missing 'from'",
					index,
					*op_text,
					*path_text));
			}
			auto from_text = from_member->as_string();
			if (!from_text) {
				return std::unexpected(patch_error(
					JsonIssueCode::patch_from_invalid,
					"JSON Patch 'from' must be a string",
					index,
					*op_text,
					*path_text));
			}
			auto from = parse_patch_pointer(*from_text, opts, index, *op_text, JsonIssueCode::patch_from_invalid);
			if (!from) {
				return std::unexpected(std::move(from).error());
			}
			op.from_text = std::string{*from_text};
			op.from = std::move(*from);
		}
		if (*parsed_op == JsonPatchOp::add || *parsed_op == JsonPatchOp::replace || *parsed_op == JsonPatchOp::test) {
			auto value_member = obj->find_member("value");
			if (!value_member) {
				return std::unexpected(patch_error(
					JsonIssueCode::invalid_patch,
					"JSON Patch operation is missing 'value'",
					index,
					*op_text,
					*path_text));
			}
			if (capture_values) {
				auto value = patch_value_from_node(*value_member);
				if (!value) {
					return std::unexpected(std::move(value).error());
				}
				op.value = std::move(*value);
			}
		}
		operations.push_back(std::move(op));
		++index;
	}
	return operations;
}

} // namespace detail

std::expected<void, JsonError> validate_patch(
	NodeRef patch,
	JsonPatchOptions opts) {
	auto operations = detail::parse_patch_operations(patch, opts, false);
	if (!operations) {
		return std::unexpected(std::move(operations).error());
	}
	return {};
}

std::expected<Document, JsonError> apply_patch(
	NodeRef target,
	NodeRef patch,
	JsonPatchOptions opts) {
	auto operations = detail::parse_patch_operations(patch, opts, true);
	if (!operations) {
		return std::unexpected(std::move(operations).error());
	}
	auto candidate = detail::patch_value_from_node(target);
	if (!candidate) {
		return std::unexpected(std::move(candidate).error());
	}
	if (auto ok = detail::check_result_node_limit(*candidate, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	for (std::size_t i = 0; i < operations->size(); ++i) {
		auto const &op = (*operations)[i];
		switch (op.op) {
		case JsonPatchOp::add:
			if (auto ok = detail::patch_add(*candidate, op.path, *op.value, i, op.op_text, op.path_text); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			break;
		case JsonPatchOp::remove:
			if (auto removed = detail::patch_remove(
					*candidate,
					op.path,
					opts.allow_missing_remove,
					false,
					i,
					op.op_text,
					op.path_text);
				!removed) {
				return std::unexpected(std::move(removed).error());
			}
			break;
		case JsonPatchOp::replace:
			if (op.path.empty()) {
				*candidate = *op.value;
				break;
			}
			if (auto target_node = detail::find_patch_target(*candidate, op.path, i, op.op_text, op.path_text);
				!target_node) {
				return std::unexpected(std::move(target_node).error());
			}
			if (auto removed = detail::patch_remove(*candidate, op.path, false, false, i, op.op_text, op.path_text);
				!removed) {
				return std::unexpected(std::move(removed).error());
			}
			if (auto ok = detail::patch_add(*candidate, op.path, *op.value, i, op.op_text, op.path_text); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			break;
		case JsonPatchOp::move:
			if (detail::patch_path_is_child(op.from, op.path)) {
				return std::unexpected(
					detail::patch_error(
						JsonIssueCode::patch_move_into_child,
						"JSON Patch move cannot move a value into its own child",
						i,
						op.op_text,
						op.path_text,
						op.from_text));
			}
			{
				auto source = detail::find_patch_target(*candidate, op.from, i, op.op_text, op.path_text);
				if (!source) {
					return std::unexpected(std::move(source).error());
				}
				auto value = **source;
				if (auto removed = detail::patch_remove(*candidate, op.from, false, false, i, op.op_text, op.from_text);
					!removed) {
					return std::unexpected(std::move(removed).error());
				}
				if (auto ok = detail::patch_add(*candidate, op.path, std::move(value), i, op.op_text, op.path_text);
					!ok) {
					return std::unexpected(std::move(ok).error());
				}
			}
			break;
		case JsonPatchOp::copy:
			{
				auto source = detail::find_patch_target(*candidate, op.from, i, op.op_text, op.path_text);
				if (!source) {
					return std::unexpected(std::move(source).error());
				}
				if (auto ok = detail::patch_add(*candidate, op.path, **source, i, op.op_text, op.path_text); !ok) {
					return std::unexpected(std::move(ok).error());
				}
			}
			break;
		case JsonPatchOp::test:
			{
				auto found = detail::find_patch_target(*candidate, op.path, i, op.op_text, op.path_text);
				if (!found) {
					return std::unexpected(std::move(found).error());
				}
				auto left_builder = value_builder();
				auto right_builder = value_builder();
				if (auto ok = detail::write_patch_value(left_builder, **found); !ok) {
					return std::unexpected(std::move(ok).error());
				}
				if (auto ok = detail::write_patch_value(right_builder, *op.value); !ok) {
					return std::unexpected(std::move(ok).error());
				}
				auto left_doc = std::move(left_builder).finish();
				auto right_doc = std::move(right_builder).finish();
				if (!left_doc) {
					return std::unexpected(std::move(left_doc).error());
				}
				if (!right_doc) {
					return std::unexpected(std::move(right_doc).error());
				}
				if (!is_value_equal(left_doc->root(), right_doc->root())) {
					return std::unexpected(
						detail::patch_error(
							JsonIssueCode::patch_test_failed,
							"JSON Patch test operation failed",
							i,
							op.op_text,
							op.path_text));
				}
			}
			break;
		}
		if (auto ok = detail::check_result_node_limit(*candidate, opts, i, op.op_text, op.path_text, op.from_text);
			!ok) {
			return std::unexpected(std::move(ok).error());
		}
	}
	auto out = value_builder();
	if (auto ok = detail::write_patch_value(out, *candidate); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return std::move(out).finish();
}

std::expected<Document, JsonError> apply_patch(
	Document const &target,
	Document const &patch,
	JsonPatchOptions opts) {
	return apply_patch(target.root(), patch.root(), opts);
}

// RFC 7396 JSON Merge Patch. The DOM stays immutable; this builds a new
// owning Document while preserving target-member order for unchanged members and
// appending new patch members in patch order.
namespace detail {

std::expected<void, JsonError> copy_node_into(ValueBuilder &out, NodeRef node);
std::expected<void, JsonError> copy_node_into(ObjectBuilder &out, std::string_view name, NodeRef node);
std::expected<void, JsonError> copy_node_into(ArrayBuilder &out, NodeRef node);
std::expected<void, JsonError> merge_patch_into(ValueBuilder &out, NodeRef target, NodeRef patch);
std::expected<void, JsonError>
merge_patch_into(ObjectBuilder &out, std::string_view name, NodeRef target, NodeRef patch);

[[nodiscard]] JsonError merge_patch_wrong_kind(
	JsonKind actual) {
	return JsonError{
		.stage = JsonStage::build,
		.code = JsonIssueCode::wrong_kind,
		.expected_kind = JsonKind::object,
		.actual_kind = actual,
		.message = "std::expected object while applying JSON merge patch"};
}

std::expected<void, JsonError> copy_members_into(
	ObjectBuilder &out,
	ObjectView obj) {
	for (auto const &[name, value]: obj.members()) {
		if (auto ok = copy_node_into(out, name, value); !ok) {
			return ok;
		}
	}
	return {};
}

std::expected<void, JsonError> copy_elements_into(
	ArrayBuilder &out,
	ArrayView arr) {
	for (auto value: arr.elements()) {
		if (auto ok = copy_node_into(out, value); !ok) {
			return ok;
		}
	}
	return {};
}

std::expected<void, JsonError> copy_node_into(
	ValueBuilder &out,
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.set_null();
	case JsonKind::boolean: return out.set_bool(*node.as_bool());
	case JsonKind::string : return out.set_string(*node.as_string());
	case JsonKind::number : return out.set_number(node.as_number()->lexeme());
	case JsonKind::array:
		{
			auto arr = node.as_array();
			if (!arr) {
				return std::unexpected(std::move(arr).error());
			}
			auto child = out.begin_array();
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return std::unexpected(std::move(obj).error());
			}
			auto child = out.begin_object();
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	}
	return std::unexpected(merge_patch_wrong_kind(node.kind()));
}

std::expected<void, JsonError> copy_node_into(
	ObjectBuilder &out,
	std::string_view name,
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.insert_null(name);
	case JsonKind::boolean: return out.insert_bool(name, *node.as_bool());
	case JsonKind::string : return out.insert_string(name, *node.as_string());
	case JsonKind::number : return out.insert_number(name, node.as_number()->lexeme());
	case JsonKind::array:
		{
			auto arr = node.as_array();
			if (!arr) {
				return std::unexpected(std::move(arr).error());
			}
			auto child = out.insert_array(name);
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return std::unexpected(std::move(obj).error());
			}
			auto child = out.insert_object(name);
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	}
	return std::unexpected(merge_patch_wrong_kind(node.kind()));
}

std::expected<void, JsonError> copy_node_into(
	ArrayBuilder &out,
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.append_null();
	case JsonKind::boolean: return out.append_bool(*node.as_bool());
	case JsonKind::string : return out.append_string(*node.as_string());
	case JsonKind::number : return out.append_number(node.as_number()->lexeme());
	case JsonKind::array:
		{
			auto arr = node.as_array();
			if (!arr) {
				return std::unexpected(std::move(arr).error());
			}
			auto child = out.append_array();
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return std::unexpected(std::move(obj).error());
			}
			auto child = out.append_object();
			if (!child) {
				return std::unexpected(std::move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			std::move(*child).commit();
			return {};
		}
	}
	return std::unexpected(merge_patch_wrong_kind(node.kind()));
}

std::expected<void, JsonError> merge_object_members_into(
	ObjectBuilder &out,
	std::optional<ObjectView> target,
	ObjectView patch) {
	if (target) {
		for (auto const &[name, target_value]: target->members()) {
			auto patch_value = patch.find_member(name);
			if (!patch_value) {
				if (auto ok = copy_node_into(out, name, target_value); !ok) {
					return ok;
				}
				continue;
			}
			if (patch_value->is_null()) {
				continue;
			}
			if (auto ok = merge_patch_into(out, name, target_value, *patch_value); !ok) {
				return ok;
			}
		}
	}
	for (auto const &[name, patch_value]: patch.members()) {
		if (patch_value.is_null() || (target && target->find_member(name))) {
			continue;
		}
		if (auto ok = copy_node_into(out, name, patch_value); !ok) {
			return ok;
		}
	}
	return {};
}

std::expected<void, JsonError> merge_patch_into(
	ValueBuilder &out,
	NodeRef target,
	NodeRef patch) {
	if (patch.kind() != JsonKind::object) {
		return copy_node_into(out, patch);
	}
	auto patch_obj = patch.as_object();
	if (!patch_obj) {
		return std::unexpected(std::move(patch_obj).error());
	}
	std::optional<ObjectView> target_obj;
	if (target.kind() == JsonKind::object) {
		auto obj = target.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		target_obj.emplace(*obj);
	}
	auto child = out.begin_object();
	if (!child) {
		return std::unexpected(std::move(child).error());
	}
	if (auto ok = merge_object_members_into(*child, target_obj, *patch_obj); !ok) {
		return ok;
	}
	std::move(*child).commit();
	return {};
}

std::expected<void, JsonError> merge_patch_into(
	ObjectBuilder &out,
	std::string_view name,
	NodeRef target,
	NodeRef patch) {
	if (patch.kind() != JsonKind::object) {
		return copy_node_into(out, name, patch);
	}
	auto patch_obj = patch.as_object();
	if (!patch_obj) {
		return std::unexpected(std::move(patch_obj).error());
	}
	std::optional<ObjectView> target_obj;
	if (target.kind() == JsonKind::object) {
		auto obj = target.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		target_obj.emplace(*obj);
	}
	auto child = out.insert_object(name);
	if (!child) {
		return std::unexpected(std::move(child).error());
	}
	if (auto ok = merge_object_members_into(*child, target_obj, *patch_obj); !ok) {
		return ok;
	}
	std::move(*child).commit();
	return {};
}

} // namespace detail

std::expected<Document, JsonError> merge_patch(
	NodeRef target,
	NodeRef patch) {
	auto out = value_builder();
	if (auto ok = detail::merge_patch_into(out, target, patch); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return std::move(out).finish();
}

std::expected<Document, JsonError> merge_patch(
	Document const &target,
	Document const &patch) {
	return merge_patch(target.root(), patch.root());
}

} // namespace conflux::json
