module conflux.json;

import std;
import std.compat;
import conflux.types;

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
	state_->store = DocumentStorage{};
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
	return ::make_document(std::move(storage));
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

std::expected<void, JsonError> ObjectBuilder::do_insert_node(
	std::string_view name,
	std::size_t node_idx) {
	auto *st = frame_.state;
	auto [it, inserted] = frame_.dup_check.try_emplace(std::string{name}, node_idx);
	if (!inserted) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = std::string{name},
				.message = std::format("duplicate member: {}", name)});
	}
	std::size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.local_members.push_back(
		{static_cast<std::uint32_t>(name_off),
		 static_cast<std::uint32_t>(name.size()),
		 static_cast<std::uint32_t>(node_idx),
		 kStorageInputView});
	return {};
}
std::expected<void, JsonError> ObjectBuilder::do_insert_node_view(
	std::string_view name,
	std::size_t node_idx) {
	auto [it, inserted] = frame_.dup_check.try_emplace(std::string{name}, node_idx);
	if (!inserted) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = std::string{name},
				.message = std::format("duplicate member: {}", name)});
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
	// Duplicate check before any work (O(1) amortized via std::hash).
	auto const inserted = frame_.dup_check.try_emplace(std::string{name}, 0).second;
	if (!inserted) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = std::string{name},
				.message = std::format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	std::size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	std::size_t const child_depth = frame_.depth + 1;
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
std::expected<ArrayBuilder, JsonError> ObjectBuilder::insert_array(
	std::string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	// Duplicate check before any work (O(1) amortized via std::hash).
	auto const inserted = frame_.dup_check.try_emplace(std::string{name}, 0).second;
	if (!inserted) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = std::string{name},
				.message = std::format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	std::size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	std::size_t const child_depth = frame_.depth + 1;
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
	auto *st = frame_.state;
	std::size_t const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::append_child,
		.arena_start = st->built_input.size(),
		.parent_local_children = &frame_.local_children};
	ObjectBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}
std::expected<ArrayBuilder, JsonError> ArrayBuilder::append_array() {
	if (auto ok = array_active_or_error(frame_); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	auto *st = frame_.state;
	std::size_t const child_depth = frame_.depth + 1;
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
		std::string token;
		for (std::size_t i = pos; i < slash; ++i) {
			if (pointer[i] != '~') {
				token += pointer[i];
				continue;
			}
			if (i + 1 >= slash) {
				return std::unexpected(
					patch_error(invalid_code, "invalid '~' escape in JSON Patch pointer", op_index, op, pointer));
			}
			++i;
			if (pointer[i] == '0') {
				token += '~';
			} else if (pointer[i] == '1') {
				token += '/';
			} else {
				return std::unexpected(
					patch_error(invalid_code, "invalid '~' escape in JSON Patch pointer", op_index, op, pointer));
			}
		}
		bool const append = token == "-";
		out.push_back(PatchToken{.text = std::move(token), .append = append});
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
	for (std::size_t i = 0; i < parent.size(); ++i) {
		if (parent[i].append != child[i].append || parent[i].text != child[i].text) {
			return false;
		}
	}
	return true;
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

namespace conflux::json {

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

} // namespace conflux::json

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
