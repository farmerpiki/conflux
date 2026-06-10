module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

std::expected<void, JsonError> ObjectBuilder::check_can_insert() const {
	if (frame_.committed) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ObjectBuilder already committed"});
	}
	if (frame_.state != nullptr && frame_.state->active_depth != frame_.depth) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "child builder already active"});
	}
	return {};
}

ObjectBuilder::ObjectBuilder(
	ObjectBuilder &&o) noexcept
	: frame_{std::move(o.frame_)} {
	o.frame_.state = nullptr;
}

ObjectBuilder &ObjectBuilder::operator =(
	ObjectBuilder &&o) noexcept {
	if (this != &o) {
		abort_if_open();
		frame_ = std::move(o.frame_);
		o.frame_.state = nullptr;
	}
	return *this;
}

void ObjectBuilder::abort_if_open() noexcept {
	if ((frame_.state != nullptr) && !frame_.committed) {
		auto *st = frame_.state;
		st->built_input.resize(frame_.parent.arena_start);
		frame_.local_members.clear();
		frame_.local_external_ptrs_.clear();
		frame_.dup_check.clear();
		st->active_depth = frame_.depth - 1;
		if (frame_.parent.kind == ParentSlot::Kind::set_root) {
			st->root_set = frame_.parent.saved_root_set;
			st->child_active = false;
		}
		frame_.state = nullptr;
	}
}

ObjectBuilder::~ObjectBuilder() noexcept {
	abort_if_open();
}

void ObjectBuilder::commit() && noexcept {
	if ((frame_.state == nullptr) || frame_.committed) {
		return;
	}
	auto *st = frame_.state;
	std::size_t const mem_start = st->store.object_members.size();
	for (auto m: frame_.local_members) { // copy: may patch name_off for external ptrs
		if ((m.name_flags & kMemberExternalView) != 0) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			char const *ptr = frame_.local_external_ptrs_[m.name_off];
			m.name_off = static_cast<std::uint32_t>(st->store.external_ptrs_.size());
			st->store.external_ptrs_.push_back(ptr);
		}
		st->store.object_members.push_back(m);
	}
	std::size_t const cnt = frame_.local_members.size();
	st->store.nodes.push_back(
		detail::node_object(static_cast<std::uint32_t>(mem_start), static_cast<std::uint32_t>(cnt)));
	std::size_t const node_idx = st->store.nodes.size() - 1;
	switch (frame_.parent.kind) {
	case ParentSlot::Kind::set_root:
		st->root_node = node_idx;
		st->child_active = false;
		break;
	case ParentSlot::Kind::insert_member:
		frame_.parent.parent_local_members->push_back(
			{static_cast<std::uint32_t>(frame_.parent.name_off),
			 static_cast<std::uint32_t>(frame_.parent.name_len),
			 static_cast<std::uint32_t>(node_idx),
			 kStorageInputView});
		break;
	case ParentSlot::Kind::append_child: frame_.parent.parent_local_children->push_back(node_idx); break;
	}
	st->active_depth = frame_.depth - 1;
	frame_.local_members.clear();
	frame_.local_external_ptrs_.clear();
	frame_.dup_check.clear();
	frame_.committed = true;
}

bool ArrayBuilder::arr_check_active(
	ChildFrame const &f) noexcept {
	return !f.committed && (f.state != nullptr) && (f.state->active_depth == f.depth);
}

void ArrayBuilder::abort_if_open() noexcept {
	if ((frame_.state != nullptr) && !frame_.committed) {
		auto *st = frame_.state;
		st->built_input.resize(frame_.parent.arena_start);
		frame_.local_children.clear();
		st->active_depth = frame_.depth - 1;
		if (frame_.parent.kind == ParentSlot::Kind::set_root) {
			st->root_set = frame_.parent.saved_root_set;
			st->child_active = false;
		}
		frame_.state = nullptr;
	}
}

ArrayBuilder::ArrayBuilder(
	ArrayBuilder &&o) noexcept
	: frame_{std::move(o.frame_)} {
	o.frame_.state = nullptr;
}

ArrayBuilder &ArrayBuilder::operator =(
	ArrayBuilder &&o) noexcept {
	if (this != &o) {
		abort_if_open();
		frame_ = std::move(o.frame_);
		o.frame_.state = nullptr;
	}
	return *this;
}

ArrayBuilder::~ArrayBuilder() noexcept {
	abort_if_open();
}

void ArrayBuilder::commit() && noexcept {
	if ((frame_.state == nullptr) || frame_.committed) {
		return;
	}
	auto *st = frame_.state;
	std::size_t const child_start = st->store.array_children.size();
	for (std::size_t const idx: frame_.local_children) {
		st->store.array_children.push_back(static_cast<std::uint32_t>(idx));
	}
	std::size_t const cnt = frame_.local_children.size();
	st->store.nodes.push_back(
		detail::node_array(static_cast<std::uint32_t>(child_start), static_cast<std::uint32_t>(cnt)));
	std::size_t const node_idx = st->store.nodes.size() - 1;
	switch (frame_.parent.kind) {
	case ParentSlot::Kind::set_root:
		st->root_node = static_cast<std::uint32_t>(node_idx);
		st->child_active = false;
		break;
	case ParentSlot::Kind::insert_member:
		frame_.parent.parent_local_members->push_back(
			{static_cast<std::uint32_t>(frame_.parent.name_off),
			 static_cast<std::uint32_t>(frame_.parent.name_len),
			 static_cast<std::uint32_t>(node_idx),
			 kStorageInputView});
		break;
	case ParentSlot::Kind::append_child: frame_.parent.parent_local_children->push_back(node_idx); break;
	}
	st->active_depth = frame_.depth - 1;
	frame_.local_children.clear();
	frame_.committed = true;
}

// ---------------------------------------------------------------------------
// ValueBuilder root writer
// ---------------------------------------------------------------------------

ValueBuilder::ValueBuilder(
	BuilderState *borrowed) noexcept
	: state_{borrowed} {}

std::expected<void, JsonError> ValueBuilder::check_can_set() const {
	if (state_ == nullptr) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ValueBuilder has been discarded"});
	}
	if (state_->root_set) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "root value already fixed; use reset() to start over"});
	}
	if (state_->child_active) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "child builder is active"});
	}
	return {};
}

std::expected<void, JsonError> ValueBuilder::set_node(
	Node n) {
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	state_->store.nodes.push_back(n);
	state_->root_node = state_->store.nodes.size() - 1;
	state_->root_set = true;
	return {};
}

ValueBuilder::ValueBuilder()
	: owned_{std::make_unique<BuilderState>()}
	, state_{owned_.get()} {}

ValueBuilder::ValueBuilder(
	ValueBuilder &&o) noexcept
	: owned_{std::move(o.owned_)}
	, state_{owned_ ? owned_.get() : o.state_} {
	o.state_ = nullptr;
}

ValueBuilder &ValueBuilder::operator =(
	ValueBuilder &&o) noexcept {
	if (this != &o) {
		owned_ = std::move(o.owned_);
		state_ = owned_ ? owned_.get() : o.state_;
		o.state_ = nullptr;
	}
	return *this;
}

std::expected<void, JsonError> ValueBuilder::set_null() {
	return set_node(detail::make_null());
}

std::expected<void, JsonError> ValueBuilder::set_bool(
	bool v) {
	return set_node(detail::make_bool(v));
}

std::expected<void, JsonError> ValueBuilder::set_string(
	std::string_view sv) {
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	std::size_t const off = state_->built_input.size();
	state_->built_input.append(sv.data(), sv.size());
	return set_node(
		detail::make_string(static_cast<std::uint32_t>(off), static_cast<std::uint32_t>(sv.size()), kStorageInputView));
}

std::expected<void, JsonError> ValueBuilder::set_number(
	std::string_view lexeme) {
	if (!validate_number_lexeme(lexeme)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = std::format("invalid number lexeme: {}", lexeme)});
	}
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	std::size_t const off = state_->built_input.size();
	state_->built_input.append(lexeme.data(), lexeme.size());
	auto node = detail::build_number_node_from_lexeme(
		static_cast<std::uint32_t>(off),
		static_cast<std::uint32_t>(lexeme.size()),
		kStorageInputView | kRawJsonSlice,
		lexeme);
	if (!node) {
		return std::unexpected(std::move(node).error());
	}
	return set_node(*node);
}

std::expected<void, JsonError> ValueBuilder::set_i64(
	std::int64_t v) {
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	std::size_t const off = state_->built_input.size();
	std::array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
	state_->built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state_->built_input.size() - off;
	return set_node(
		detail::make_number_int(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			v));
}

std::expected<void, JsonError> ValueBuilder::set_u64(
	std::uint64_t v) {
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	std::size_t const off = state_->built_input.size();
	std::array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
	state_->built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state_->built_input.size() - off;
	if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		return set_node(
			detail::make_number_int(
				static_cast<std::uint32_t>(off),
				static_cast<std::uint32_t>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<std::int64_t>(v)));
	}
	return set_node(
		detail::make_number_uint(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			v));
}

std::expected<void, JsonError> ValueBuilder::set_f64(
	double v) {
	if (!std::isfinite(v)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::number_out_of_range,
				.message = "set_f64 requires finite value"});
	}
	auto ok = check_can_set();
	if (!ok) {
		return ok;
	}
	std::size_t const off = state_->built_input.size();
	std::array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
	state_->built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state_->built_input.size() - off;
	std::string_view const lex = std::string_view{state_->built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == std::string_view::npos;
	return set_node(
		detail::make_number_f64(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			v,
			is_int));
}

std::expected<ObjectBuilder, JsonError> ValueBuilder::begin_object() {
	auto ok = check_can_set();
	if (!ok) {
		return std::unexpected(std::move(ok).error());
	}
	bool const prev_root_set = state_->root_set;
	state_->child_active = true;
	state_->root_set = true;
	state_->active_depth = 1;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::set_root,
		.arena_start = state_->built_input.size(),
		.saved_root_set = prev_root_set};
	ObjectBuilder child{state_, parent};
	child.frame_.depth = 1;
	return child;
}

std::expected<ArrayBuilder, JsonError> ValueBuilder::begin_array() {
	auto ok = check_can_set();
	if (!ok) {
		return std::unexpected(std::move(ok).error());
	}
	bool const prev_root_set = state_->root_set;
	state_->child_active = true;
	state_->root_set = true;
	state_->active_depth = 1;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::set_root,
		.arena_start = state_->built_input.size(),
		.saved_root_set = prev_root_set};
	ArrayBuilder child{state_, parent};
	child.frame_.depth = 1;
	return child;
}

void ValueBuilder::reset() noexcept {
	state_->store.reset();
	state_->built_input.clear();
	state_->root_set = false;
	state_->root_node = 0;
	state_->child_active = false;
	state_->active_depth = 0;
}

void ValueBuilder::discard() && noexcept {
	owned_.reset();
	state_ = nullptr;
}

std::expected<Document, JsonError> ValueBuilder::finish() && {
	if ((state_ == nullptr) || !state_->root_set || state_->child_active) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = (state_ != nullptr) && state_->child_active ? "child builder still active" :
																		 "root value was never set"});
	}
	// Phase 1.5: transfer built_input → owned_input; node off/len with
	// kStorageInputView point into the heap-stable buffer body. The
	// 4 GiB ceiling on built_input is enforced incrementally at each
	// insert site (Correction S); this is the final guard.
	constexpr std::size_t kU32Ceiling = (std::size_t{1} << 32) - 1;
	if (state_->built_input.size() >= kU32Ceiling) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::input_too_large,
				.message = "Builder input buffer exceeds 4 GiB hard ceiling"});
	}
	auto storage = std::make_unique<DocumentStorage>(std::move(state_->store));
	storage->root_node = static_cast<std::uint32_t>(state_->root_node);
	storage->owned_input = std::move(state_->built_input);
	storage->input_view = storage->owned_input;
	owned_.reset();
	state_ = nullptr;
	return make_document(std::move(storage));
}

// ---------------------------------------------------------------------------
// ObjectBuilder member insert helpers
// ---------------------------------------------------------------------------

static std::expected<void, JsonError> array_active_or_error(
	ChildFrame const &frame) {
	if (!frame.committed && (frame.state != nullptr) && (frame.state->active_depth == frame.depth)) {
		return {};
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::build,
			.code = JsonIssueCode::constraint_violation,
			.message = frame.committed ? "ArrayBuilder already committed" : "child builder already active"});
}

static JsonError duplicate_member_error(
	std::string_view name) {
	return JsonError{
		.stage = JsonStage::build,
		.code = JsonIssueCode::duplicate_member,
		.member_name = std::string{name},
		.message = std::format("duplicate member: {}", name)};
}

static std::expected<void, JsonError> validate_utf8_for_build(
	std::string_view value) {
	for (std::size_t i = 0; i < value.size();) {
		auto const c = static_cast<unsigned char>(value[i]);
		std::size_t const seq = utf8_seq_len(c);
		if (seq == 0) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = std::format("invalid UTF-8 std::byte at offset {}", i)});
		}
		if (i + seq > value.size()) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::invalid_utf8,
					.message = std::format("truncated UTF-8 at offset {}", i)});
		}
		for (std::size_t k = 1; k < seq; ++k) {
			if (!is_cont(static_cast<unsigned char>(value[i + k]))) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_utf8,
						.message = std::format("invalid UTF-8 continuation at offset {}", i + k)});
			}
		}
		i += seq;
	}
	return {};
}

static std::size_t push_node(
	BuilderState &state,
	Node node) {
	state.store.nodes.push_back(node);
	return state.store.nodes.size() - 1;
}

static std::size_t push_owned_string_node(
	BuilderState &state,
	std::string_view value) {
	std::size_t const off = state.built_input.size();
	state.built_input.append(value.data(), value.size());
	return push_node(
		state,
		detail::make_string(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(value.size()),
			kStorageInputView));
}

static std::size_t push_borrowed_string_node(
	BuilderState &state,
	std::string_view value) {
	std::uint32_t const val_ptr_idx = static_cast<std::uint32_t>(state.store.external_ptrs_.size());
	state.store.external_ptrs_.push_back(value.data());
	return push_node(
		state,
		detail::make_string(val_ptr_idx, static_cast<std::uint32_t>(value.size()), kValueExternalView));
}

static std::expected<std::size_t, JsonError> push_number_lexeme_node(
	BuilderState &state,
	std::string_view lexeme) {
	if (!validate_number_lexeme(lexeme)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = std::format("invalid number lexeme: {}", lexeme)});
	}
	std::size_t const off = state.built_input.size();
	state.built_input.append(lexeme.data(), lexeme.size());
	auto node = detail::build_number_node_from_lexeme(
		static_cast<std::uint32_t>(off),
		static_cast<std::uint32_t>(lexeme.size()),
		kStorageInputView | kRawJsonSlice,
		lexeme);
	if (!node) {
		return std::unexpected(std::move(node).error());
	}
	return push_node(state, *node);
}

static std::size_t push_i64_node(
	BuilderState &state,
	std::int64_t value) {
	std::size_t const off = state.built_input.size();
	std::array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	state.built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state.built_input.size() - off;
	return push_node(
		state,
		detail::make_number_int(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			value));
}

static std::size_t push_u64_node(
	BuilderState &state,
	std::uint64_t value) {
	std::size_t const off = state.built_input.size();
	std::array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	state.built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state.built_input.size() - off;
	if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		return push_node(
			state,
			detail::make_number_int(
				static_cast<std::uint32_t>(off),
				static_cast<std::uint32_t>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<std::int64_t>(value)));
	}
	return push_node(
		state,
		detail::make_number_uint(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			value));
}

static std::expected<std::size_t, JsonError> push_f64_node(
	BuilderState &state,
	double value,
	std::string_view label) {
	if (!std::isfinite(value)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::number_out_of_range,
				.message = std::format("{} requires finite value", label)});
	}
	std::size_t const off = state.built_input.size();
	std::array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	state.built_input.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
	std::size_t const len = state.built_input.size() - off;
	std::string_view const lex = std::string_view{state.built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == std::string_view::npos;
	return push_node(
		state,
		detail::make_number_f64(
			static_cast<std::uint32_t>(off),
			static_cast<std::uint32_t>(len),
			kStorageInputView | kRawJsonSlice,
			value,
			is_int));
}

static std::expected<std::size_t, JsonError> reserve_owned_member_name(
	ChildFrame &frame,
	std::string_view name,
	[[maybe_unused]] std::size_t node_idx) {
	if (frame.has_member(name)) {
		return std::unexpected(duplicate_member_error(name));
	}
	frame.track_member(name);
	auto *st = frame.state;
	std::size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	return name_off;
}

static std::expected<void, JsonError> reserve_borrowed_member_name(
	ChildFrame &frame,
	std::string_view name,
	[[maybe_unused]] std::size_t node_idx) {
	if (frame.has_member(name)) {
		return std::unexpected(duplicate_member_error(name));
	}
	frame.track_member(name);
	return {};
}

static ParentSlot begin_insert_child(
	ChildFrame &frame,
	std::size_t name_off,
	std::string_view name) {
	auto *st = frame.state;
	std::size_t const child_depth = frame.depth + 1;
	st->active_depth = child_depth;
	return ParentSlot{
		.kind = ParentSlot::Kind::insert_member,
		.name_off = name_off,
		.name_len = name.size(),
		.arena_start = name_off,
		.parent_local_members = &frame.local_members};
}

static ParentSlot begin_append_child(
	ChildFrame &frame) {
	auto *st = frame.state;
	std::size_t const child_depth = frame.depth + 1;
	st->active_depth = child_depth;
	return ParentSlot{
		.kind = ParentSlot::Kind::append_child,
		.arena_start = st->built_input.size(),
		.parent_local_children = &frame.local_children};
}

std::expected<void, JsonError> ObjectBuilder::do_insert_node(
	std::string_view name,
	std::size_t node_idx) {
	auto name_off = reserve_owned_member_name(frame_, name, node_idx);
	if (!name_off) {
		return std::unexpected(std::move(name_off).error());
	}
	frame_.local_members.push_back(
		{static_cast<std::uint32_t>(*name_off),
		 static_cast<std::uint32_t>(name.size()),
		 static_cast<std::uint32_t>(node_idx),
		 kStorageInputView});
	return {};
}
std::expected<void, JsonError> ObjectBuilder::do_insert_node_view(
	std::string_view name,
	std::size_t node_idx) {
	if (auto ok = reserve_borrowed_member_name(frame_, name, node_idx); !ok) {
		return ok;
	}
	MemberEntry m{};
	m.name_off = static_cast<std::uint32_t>(frame_.local_external_ptrs_.size());
	m.name_len = static_cast<std::uint32_t>(name.size());
	m.val_node = static_cast<std::uint32_t>(node_idx);
	m.name_flags = kMemberExternalView;
	frame_.local_external_ptrs_.push_back(name.data());
	frame_.local_members.push_back(m);
	return {};
}
std::expected<void, JsonError> ObjectBuilder::insert_null(
	std::string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node(name, push_node(*st, detail::make_null()));
}
std::expected<void, JsonError> ObjectBuilder::insert_bool(
	std::string_view name,
	bool v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node(name, push_node(*st, detail::make_bool(v)));
}
std::expected<void, JsonError> ObjectBuilder::insert_string(
	std::string_view name,
	std::string_view value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node(name, push_owned_string_node(*st, value));
}
std::expected<void, JsonError> ObjectBuilder::insert_string_checked(
	std::string_view name,
	std::string_view value) {
	if (auto ok = validate_utf8_for_build(value); !ok) {
		return ok;
	}
	return insert_string(name, value);
}
std::expected<void, JsonError> ObjectBuilder::insert_string_borrowed_name(
	std::string_view name,
	std::string_view value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node_view(name, push_owned_string_node(*st, value));
}
std::expected<void, JsonError> ObjectBuilder::insert_string_borrowed(
	std::string_view name,
	std::string_view value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node_view(name, push_borrowed_string_node(*st, value));
}
std::expected<void, JsonError> ObjectBuilder::insert_number(
	std::string_view name,
	std::string_view lexeme) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	auto node_idx = push_number_lexeme_node(*st, lexeme);
	if (!node_idx) {
		return std::unexpected(std::move(node_idx).error());
	}
	return do_insert_node(name, *node_idx);
}
std::expected<void, JsonError> ObjectBuilder::insert_i64(
	std::string_view name,
	std::int64_t v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node(name, push_i64_node(*st, v));
}
std::expected<void, JsonError> ObjectBuilder::insert_u64(
	std::string_view name,
	std::uint64_t v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	return do_insert_node(name, push_u64_node(*st, v));
}
std::expected<void, JsonError> ObjectBuilder::insert_f64(
	std::string_view name,
	double v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	auto node_idx = push_f64_node(*st, v, "insert_f64");
	if (!node_idx) {
		return std::unexpected(std::move(node_idx).error());
	}
	return do_insert_node(name, *node_idx);
}
std::expected<ObjectBuilder, JsonError> ObjectBuilder::insert_object(
	std::string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	auto name_off = reserve_owned_member_name(frame_, name, 0);
	if (!name_off) {
		return std::unexpected(std::move(name_off).error());
	}
	std::size_t const child_depth = frame_.depth + 1;
	ObjectBuilder child{frame_.state, begin_insert_child(frame_, *name_off, name)};
	child.frame_.depth = child_depth;
	return child;
}
std::expected<ArrayBuilder, JsonError> ObjectBuilder::insert_array(
	std::string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	auto name_off = reserve_owned_member_name(frame_, name, 0);
	if (!name_off) {
		return std::unexpected(std::move(name_off).error());
	}
	std::size_t const child_depth = frame_.depth + 1;
	ArrayBuilder child{frame_.state, begin_insert_child(frame_, *name_off, name)};
	child.frame_.depth = child_depth;
	return child;
}
// ---------------------------------------------------------------------------
// ArrayBuilder append helpers
// ---------------------------------------------------------------------------

std::expected<void, JsonError> ArrayBuilder::append_null() {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_node(*st, detail::make_null()));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_bool(
	bool v) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_node(*st, detail::make_bool(v)));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_string(
	std::string_view value) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_owned_string_node(*st, value));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_string_checked(
	std::string_view value) {
	if (auto ok = validate_utf8_for_build(value); !ok) {
		return ok;
	}
	return append_string(value);
}
std::expected<void, JsonError> ArrayBuilder::append_string_borrowed(
	std::string_view value) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_borrowed_string_node(*st, value));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_number(
	std::string_view lexeme) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	auto node_idx = push_number_lexeme_node(*st, lexeme);
	if (!node_idx) {
		return std::unexpected(std::move(node_idx).error());
	}
	frame_.local_children.push_back(*node_idx);
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_i64(
	std::int64_t v) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_i64_node(*st, v));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_u64(
	std::uint64_t v) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	frame_.local_children.push_back(push_u64_node(*st, v));
	return {};
}
std::expected<void, JsonError> ArrayBuilder::append_f64(
	double v) {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	auto node_idx = push_f64_node(*st, v, "append_f64");
	if (!node_idx) {
		return std::unexpected(std::move(node_idx).error());
	}
	frame_.local_children.push_back(*node_idx);
	return {};
}
std::expected<ObjectBuilder, JsonError> ArrayBuilder::append_object() {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	std::size_t const child_depth = frame_.depth + 1;
	ObjectBuilder child{frame_.state, begin_append_child(frame_)};
	child.frame_.depth = child_depth;
	return child;
}
std::expected<ArrayBuilder, JsonError> ArrayBuilder::append_array() {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	std::size_t const child_depth = frame_.depth + 1;
	ArrayBuilder child{frame_.state, begin_append_child(frame_)};
	child.frame_.depth = child_depth;
	return child;
}

ValueBuilder value_builder() {
	return {};
}

} // namespace conflux::json
