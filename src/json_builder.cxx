module conflux.json;

import std;
import std.compat;
import conflux.types;

// ---------------------------------------------------------------------------
// Builder number lexeme validation
// ---------------------------------------------------------------------------

bool validate_number_lexeme(
	SV lex) noexcept {
	if (lex.empty()) {
		return false;
	}
	SZ i = 0;
	if (lex[i] == '-') {
		++i;
		if (i >= lex.size()) {
			return false;
		}
	}
	if (lex[i] == '0') {
		++i;
	} else if (lex[i] >= '1' && lex[i] <= '9') {
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	} else {
		return false;
	}
	if (i < lex.size() && lex[i] == '.') {
		++i;
		if (i >= lex.size() || lex[i] < '0' || lex[i] > '9') {
			return false;
		}
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	}
	if (i < lex.size() && (lex[i] == 'e' || lex[i] == 'E')) {
		++i;
		if (i < lex.size() && (lex[i] == '+' || lex[i] == '-')) {
			++i;
		}
		if (i >= lex.size() || lex[i] < '0' || lex[i] > '9') {
			return false;
		}
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	}
	return i == lex.size();
}
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
