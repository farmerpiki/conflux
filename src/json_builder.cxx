module conflux.json;

import std;
import std.compat;
import conflux.types;

expected<void, JsonError> ValueBuilder::set_number(
	SV lexeme) {
	if (!validate_number_lexeme(lexeme)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = format("invalid number lexeme: {}", lexeme)});
	}
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	SZ const off = state_->built_input.size();
	state_->built_input.append(lexeme.data(), lexeme.size());
	auto node = detail::build_number_node_from_lexeme(
		static_cast<u32>(off),
		static_cast<u32>(lexeme.size()),
		kStorageInputView | kRawJsonSlice,
		lexeme);
	if (!node) {
		return unexpected(move(node).error());
	}
	return set_node(*node);
}
// ---------------------------------------------------------------------------
// ObjectBuilder member insert helpers
// ---------------------------------------------------------------------------

expected<void, JsonError> ObjectBuilder::do_insert_node(
	SV name,
	SZ node_idx) {
	auto *st = frame_.state;
	auto [it, inserted] = frame_.dup_check.try_emplace(S{name}, node_idx);
	if (!inserted) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = S{name},
				.message = format("duplicate member: {}", name)});
	}
	SZ const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.local_members.push_back(
		{static_cast<u32>(name_off), static_cast<u32>(name.size()), static_cast<u32>(node_idx), kStorageInputView});
	return {};
}
expected<void, JsonError> ObjectBuilder::do_insert_node_view(
	SV name,
	SZ node_idx) {
	auto [it, inserted] = frame_.dup_check.try_emplace(S{name}, node_idx);
	if (!inserted) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = S{name},
				.message = format("duplicate member: {}", name)});
	}
	MemberEntry m{};
	m.name_off = static_cast<u32>(frame_.local_external_ptrs_.size());
	m.name_len = static_cast<u32>(name.size());
	m.val_node = static_cast<u32>(node_idx);
	m.name_flags = kMemberExternalView;
	frame_.local_external_ptrs_.push_back(name.data());
	frame_.local_members.push_back(m);
	return {};
}
expected<void, JsonError> ObjectBuilder::insert_null(
	SV name) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_null());
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_bool(
	SV name,
	bool v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_bool(v));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string(
	SV name,
	SV value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string_checked(
	SV name,
	SV value) {
	for (SZ i = 0; i < value.size();) {
		auto const c = static_cast<unsigned char>(value[i]);
		SZ const seq = utf8_seq_len(c);
		if (seq == 0) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = format("invalid UTF-8 byte at offset {}", i)});
		}
		if (i + seq > value.size()) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = format("truncated UTF-8 at offset {}", i)});
		}
		for (SZ k = 1; k < seq; ++k) {
			if (!is_cont(static_cast<unsigned char>(value[i + k]))) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_utf8,
						.message = format("invalid UTF-8 continuation at offset {}", i + k)});
			}
		}
		i += seq;
	}
	return insert_string(name, value);
}
expected<void, JsonError> ObjectBuilder::insert_string_borrowed_name(
	SV name,
	SV value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	return do_insert_node_view(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string_borrowed(
	SV name,
	SV value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	u32 const val_ptr_idx = static_cast<u32>(st->store.external_ptrs_.size());
	st->store.external_ptrs_.push_back(value.data());
	st->store.nodes.push_back(detail::make_string(val_ptr_idx, static_cast<u32>(value.size()), kValueExternalView));
	return do_insert_node_view(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_number(
	SV name,
	SV lexeme) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	if (!validate_number_lexeme(lexeme)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = format("invalid number lexeme: {}", lexeme)});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	st->built_input.append(lexeme.data(), lexeme.size());
	auto node = detail::build_number_node_from_lexeme(
		static_cast<u32>(off),
		static_cast<u32>(lexeme.size()),
		kStorageInputView | kRawJsonSlice,
		lexeme);
	if (!node) {
		return unexpected(move(node).error());
	}
	st->store.nodes.push_back(*node);
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_i64(
	SV name,
	i64 v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	st->store.nodes.push_back(
		detail::make_number_int(static_cast<u32>(off), static_cast<u32>(len), kStorageInputView | kRawJsonSlice, v));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_u64(
	SV name,
	u64 v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	if (v <= static_cast<u64>(NL<i64>::max())) {
		st->store.nodes.push_back(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<i64>(v)));
	} else {
		st->store.nodes.push_back(
			detail::make_number_uint(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v));
	}
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_f64(
	SV name,
	double v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	if (!isfinite(v)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::number_out_of_range,
				.message = "insert_f64 requires finite value"});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	SV const lex = SV{st->built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == SV::npos;
	st->store.nodes.push_back(
		detail::make_number_f64(
			static_cast<u32>(off),
			static_cast<u32>(len),
			kStorageInputView | kRawJsonSlice,
			v,
			is_int));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<ObjectBuilder, JsonError> ObjectBuilder::insert_object(
	SV name) {
	if (auto ok = check_can_insert(); !ok) {
		return unexpected(move(ok).error());
	}
	// Duplicate check before any work (O(1) amortized via hash).
	if (frame_.dup_check.contains(S{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = S{name},
				.message = format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	SZ const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.dup_check.emplace(S{name}, 0);
	SZ const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::insert_member,
		.name_off = name_off,
		.name_len = name.size(),
		.arena_start = name_off,
		.parent_local_members = &frame_.local_members};
	ObjectBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}
expected<ArrayBuilder, JsonError> ObjectBuilder::insert_array(
	SV name) {
	if (auto ok = check_can_insert(); !ok) {
		return unexpected(move(ok).error());
	}
	// Duplicate check before any work (O(1) amortized via hash).
	if (frame_.dup_check.contains(S{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = S{name},
				.message = format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	SZ const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.dup_check.emplace(S{name}, 0);
	SZ const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::insert_member,
		.name_off = name_off,
		.name_len = name.size(),
		.arena_start = name_off,
		.parent_local_members = &frame_.local_members};
	ArrayBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}
// ---------------------------------------------------------------------------
// ArrayBuilder append helpers
// ---------------------------------------------------------------------------

expected<void, JsonError> ArrayBuilder::append_null() {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_null());
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_bool(
	bool v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_bool(v));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_string(
	SV value) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_string_checked(
	SV value) {
	for (SZ i = 0; i < value.size();) {
		auto const c = static_cast<unsigned char>(value[i]);
		SZ const seq = utf8_seq_len(c);
		if (seq == 0) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = format("invalid UTF-8 byte at offset {}", i)});
		}
		if (i + seq > value.size()) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = format("truncated UTF-8 at offset {}", i)});
		}
		for (SZ k = 1; k < seq; ++k) {
			if (!is_cont(static_cast<unsigned char>(value[i + k]))) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_utf8,
						.message = format("invalid UTF-8 continuation at offset {}", i + k)});
			}
		}
		i += seq;
	}
	return append_string(value);
}
expected<void, JsonError> ArrayBuilder::append_string_borrowed(
	SV value) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	u32 const val_ptr_idx = static_cast<u32>(st->store.external_ptrs_.size());
	st->store.external_ptrs_.push_back(value.data());
	st->store.nodes.push_back(detail::make_string(val_ptr_idx, static_cast<u32>(value.size()), kValueExternalView));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_number(
	SV lexeme) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	if (!validate_number_lexeme(lexeme)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = format("invalid number lexeme: {}", lexeme)});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	st->built_input.append(lexeme.data(), lexeme.size());
	auto node = detail::build_number_node_from_lexeme(
		static_cast<u32>(off),
		static_cast<u32>(lexeme.size()),
		kStorageInputView | kRawJsonSlice,
		lexeme);
	if (!node) {
		return unexpected(move(node).error());
	}
	st->store.nodes.push_back(*node);
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_i64(
	i64 v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	st->store.nodes.push_back(
		detail::make_number_int(static_cast<u32>(off), static_cast<u32>(len), kStorageInputView | kRawJsonSlice, v));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_u64(
	u64 v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	if (v <= static_cast<u64>(NL<i64>::max())) {
		st->store.nodes.push_back(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<i64>(v)));
	} else {
		st->store.nodes.push_back(
			detail::make_number_uint(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v));
	}
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_f64(
	double v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	if (!isfinite(v)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::number_out_of_range,
				.message = "append_f64 requires finite value"});
	}
	auto *st = frame_.state;
	SZ const off = st->built_input.size();
	A<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
	SZ const len = st->built_input.size() - off;
	SV const lex = SV{st->built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == SV::npos;
	st->store.nodes.push_back(
		detail::make_number_f64(
			static_cast<u32>(off),
			static_cast<u32>(len),
			kStorageInputView | kRawJsonSlice,
			v,
			is_int));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<ObjectBuilder, JsonError> ArrayBuilder::append_object() {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	SZ const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::append_child,
		.arena_start = st->built_input.size(),
		.parent_local_children = &frame_.local_children};
	ObjectBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}
expected<ArrayBuilder, JsonError> ArrayBuilder::append_array() {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	SZ const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::append_child,
		.arena_start = st->built_input.size(),
		.parent_local_children = &frame_.local_children};
	ArrayBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}

ValueBuilder value_builder() {
	return {};
}
// RFC 7396 JSON Merge Patch. The DOM stays immutable; this builds a new
// owning Document while preserving target-member order for unchanged members and
// appending new patch members in patch order.
namespace detail {

expected<void, JsonError> copy_node_into(ValueBuilder &out, NodeRef node);
expected<void, JsonError> copy_node_into(
	ObjectBuilder &out,
	SV name,
	NodeRef node);
expected<void, JsonError> copy_node_into(ArrayBuilder &out, NodeRef node);
expected<void, JsonError> merge_patch_into(
	ValueBuilder &out,
	NodeRef target,
	NodeRef patch);
expected<void, JsonError> merge_patch_into(
	ObjectBuilder &out,
	SV name,
	NodeRef target,
	NodeRef patch);

[[nodiscard]] JsonError merge_patch_wrong_kind(JsonKind actual) {
	return JsonError{
		.stage = JsonStage::build,
		.code = JsonIssueCode::wrong_kind,
		.expected_kind = JsonKind::object,
		.actual_kind = actual,
		.message = "expected object while applying JSON merge patch"};
}

expected<void, JsonError> copy_members_into(ObjectBuilder &out, ObjectView obj) {
	for (auto const &[name, value]: obj.members()) {
		if (auto ok = copy_node_into(out, name, value); !ok) {
			return ok;
		}
	}
	return {};
}

expected<void, JsonError> copy_elements_into(ArrayBuilder &out, ArrayView arr) {
	for (auto value: arr.elements()) {
		if (auto ok = copy_node_into(out, value); !ok) {
			return ok;
		}
	}
	return {};
}

expected<void, JsonError> copy_node_into(ValueBuilder &out, NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.set_null();
	case JsonKind::boolean: return out.set_bool(*node.as_bool());
	case JsonKind::string : return out.set_string(*node.as_string());
	case JsonKind::number : return out.set_number(node.as_number()->lexeme());
	case JsonKind::array  :
		{
			auto arr = node.as_array();
			if (!arr) {
				return unexpected(move(arr).error());
			}
			auto child = out.begin_array();
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return unexpected(move(obj).error());
			}
			auto child = out.begin_object();
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	}
	return unexpected(merge_patch_wrong_kind(node.kind()));
}

expected<void, JsonError> copy_node_into(
	ObjectBuilder &out,
	SV name,
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.insert_null(name);
	case JsonKind::boolean: return out.insert_bool(name, *node.as_bool());
	case JsonKind::string : return out.insert_string(name, *node.as_string());
	case JsonKind::number : return out.insert_number(name, node.as_number()->lexeme());
	case JsonKind::array  :
		{
			auto arr = node.as_array();
			if (!arr) {
				return unexpected(move(arr).error());
			}
			auto child = out.insert_array(name);
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return unexpected(move(obj).error());
			}
			auto child = out.insert_object(name);
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	}
	return unexpected(merge_patch_wrong_kind(node.kind()));
}

expected<void, JsonError> copy_node_into(ArrayBuilder &out, NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return out.append_null();
	case JsonKind::boolean: return out.append_bool(*node.as_bool());
	case JsonKind::string : return out.append_string(*node.as_string());
	case JsonKind::number : return out.append_number(node.as_number()->lexeme());
	case JsonKind::array  :
		{
			auto arr = node.as_array();
			if (!arr) {
				return unexpected(move(arr).error());
			}
			auto child = out.append_array();
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_elements_into(*child, *arr); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	case JsonKind::object:
		{
			auto obj = node.as_object();
			if (!obj) {
				return unexpected(move(obj).error());
			}
			auto child = out.append_object();
			if (!child) {
				return unexpected(move(child).error());
			}
			if (auto ok = copy_members_into(*child, *obj); !ok) {
				return ok;
			}
			move(*child).commit();
			return {};
		}
	}
	return unexpected(merge_patch_wrong_kind(node.kind()));
}

expected<void, JsonError> merge_object_members_into(
	ObjectBuilder &out,
	Opt<ObjectView> target,
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

expected<void, JsonError> merge_patch_into(
	ValueBuilder &out,
	NodeRef target,
	NodeRef patch) {
	if (patch.kind() != JsonKind::object) {
		return copy_node_into(out, patch);
	}
	auto patch_obj = patch.as_object();
	if (!patch_obj) {
		return unexpected(move(patch_obj).error());
	}
	Opt<ObjectView> target_obj;
	if (target.kind() == JsonKind::object) {
		auto obj = target.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		target_obj.emplace(*obj);
	}
	auto child = out.begin_object();
	if (!child) {
		return unexpected(move(child).error());
	}
	if (auto ok = merge_object_members_into(*child, target_obj, *patch_obj); !ok) {
		return ok;
	}
	move(*child).commit();
	return {};
}

expected<void, JsonError> merge_patch_into(
	ObjectBuilder &out,
	SV name,
	NodeRef target,
	NodeRef patch) {
	if (patch.kind() != JsonKind::object) {
		return copy_node_into(out, name, patch);
	}
	auto patch_obj = patch.as_object();
	if (!patch_obj) {
		return unexpected(move(patch_obj).error());
	}
	Opt<ObjectView> target_obj;
	if (target.kind() == JsonKind::object) {
		auto obj = target.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		target_obj.emplace(*obj);
	}
	auto child = out.insert_object(name);
	if (!child) {
		return unexpected(move(child).error());
	}
	if (auto ok = merge_object_members_into(*child, target_obj, *patch_obj); !ok) {
		return ok;
	}
	move(*child).commit();
	return {};
}

} // namespace detail

expected<Document, JsonError> merge_patch(
	NodeRef target,
	NodeRef patch) {
	auto out = value_builder();
	if (auto ok = detail::merge_patch_into(out, target, patch); !ok) {
		return unexpected(move(ok).error());
	}
	return move(out).finish();
}

expected<Document, JsonError> merge_patch(
	Document const &target,
	Document const &patch) {
	return merge_patch(target.root(), patch.root());
}
