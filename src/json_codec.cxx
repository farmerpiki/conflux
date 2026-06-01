export module conflux.json:codec;
import std;
import std.compat;
import conflux.types;
import :api;

// has_json_codec — forward-declared here so builders can use it in requires
// ---------------------------------------------------------------------------

export namespace conflux::json {

template<class M>
using JsonConstraintFn = std::expected<void, JsonError> (*)(M const &);

template<class T>
struct JsonMembers;

template<class T>
struct JsonCodec;

template<class T>
struct JsonBorrowedViewFields : std::false_type {};

template<class T, class M>
struct JsonMember {
	std::string_view name;
	M T::*pointer;
};

template<class T, class M>
constexpr JsonMember<T, M> json_member(
	std::string_view name,
	M T::*p) {
	return {name, p};
}

} // namespace conflux::json

namespace conflux::json {

namespace detail {

template<class T, class = void>
struct has_codec_spec : std::false_type {};
template<class T, class = void>
struct has_members_spec : std::false_type {};
template<class T>
struct is_optional : std::false_type {};
template<class T>
struct is_optional<std::optional<T>> : std::true_type {};
template<class T>
struct is_nullable_type : std::false_type {};
template<class T>
struct is_nullable_type<Nullable<T>> : std::true_type {};
template<class T>
struct nullable_inner {
	using type = void;
};
template<class T>
struct nullable_inner<Nullable<T>> {
	using type = T;
};
template<class T>
using nullable_inner_t = typename nullable_inner<T>::type;
template<class T>
struct is_basic_string_of_char : std::false_type {};
template<class Traits, class Alloc>
struct is_basic_string_of_char<std::basic_string<char, Traits, Alloc>> : std::true_type {};
template<class T>
constexpr bool is_basic_string_of_char_v = is_basic_string_of_char<T>::value;
template<class T>
struct is_vector_of : std::false_type {};
template<class T, class Alloc>
struct is_vector_of<std::vector<T, Alloc>> : std::true_type {};
template<class T>
constexpr bool is_vector_of_v = is_vector_of<T>::value;
template<class T>
struct is_std_array : std::false_type {};
template<class T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};
template<class T>
constexpr bool is_std_array_v = is_std_array<T>::value;
template<class T, bool = is_std_array_v<T>>
struct is_fixed_numeric_array : std::false_type {};
template<class T>
struct is_fixed_numeric_array<T, true>
	: std::bool_constant<
		  (std::integral<typename T::value_type> && !std::same_as<typename T::value_type, bool>)
		  || std::floating_point<typename T::value_type>> {};
template<class T>
constexpr bool is_fixed_numeric_array_v = is_fixed_numeric_array<T>::value;
template<class T>
struct is_pair : std::false_type {};
template<class A, class B>
struct is_pair<std::pair<A, B>> : std::true_type {};
template<class T>
constexpr bool is_pair_v = is_pair<T>::value;
template<class T>
struct is_tuple_of : std::false_type {};
template<class... Ts>
struct is_tuple_of<std::tuple<Ts...>> : std::true_type {};
template<class T>
constexpr bool is_tuple_of_v = is_tuple_of<T>::value;
template<class T>
struct is_map_type : std::false_type {};
template<class Vt, class Compare, class Alloc>
struct is_map_type<std::map<std::string, Vt, Compare, Alloc>> : std::true_type {};
template<class T>
constexpr bool is_map_type_v = is_map_type<T>::value;
template<class T>
struct is_unordered_map_type : std::false_type {};
template<class Vt, class Hash, class KeyEqual, class Alloc>
struct is_unordered_map_type<std::unordered_map<std::string, Vt, Hash, KeyEqual, Alloc>> : std::true_type {};
template<class T>
constexpr bool is_unordered_map_type_v = is_unordered_map_type<T>::value;
struct PathFrame {
	enum class Kind : std::uint8_t {
		member,
		index,
	} kind;
	std::string_view member_name{};
	std::size_t index{};
};

class PathFrameStack {
	static constexpr std::size_t kInlineCapacity = 16;
	std::array<PathFrame, kInlineCapacity> inline_{};
	std::vector<PathFrame> overflow_{};
	std::size_t size_{};

public:
	[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }

	void push_back(
		PathFrame frame) {
		if (overflow_.empty() && size_ < kInlineCapacity) {
			inline_[size_++] = frame;
			return;
		}
		if (overflow_.empty()) {
			overflow_.reserve(kInlineCapacity * 2U);
			overflow_.insert(overflow_.end(), inline_.begin(), inline_.begin() + static_cast<std::ptrdiff_t>(size_));
		}
		overflow_.push_back(frame);
		size_ = overflow_.size();
	}

	void pop_back() noexcept {
		if (size_ == 0) {
			return;
		}
		if (!overflow_.empty()) {
			overflow_.pop_back();
			size_ = overflow_.size();
			return;
		}
		--size_;
	}

	[[nodiscard]] std::span<PathFrame const> view() const noexcept {
		if (!overflow_.empty()) {
			return std::span<PathFrame const>{overflow_.data(), overflow_.size()};
		}
		return std::span<PathFrame const>{inline_.data(), size_};
	}
};

[[nodiscard]] inline JsonPath materialize_path(
	std::span<PathFrame const> frames) {
	JsonPath p;
	for (auto const &f: frames) {
		if (f.kind == PathFrame::Kind::member) {
			p.push_member(f.member_name);
		} else {
			p.push_index(f.index);
		}
	}
	return p;
}

[[nodiscard]] inline JsonPath materialize_path(
	PathFrameStack const &frames) {
	return materialize_path(frames.view());
}

} // namespace detail

export template<class T>
concept has_json_codec = detail::has_codec_spec<T>::value || detail::has_members_spec<T>::value;

export template<class T>
inline constexpr bool has_json_codec_v = has_json_codec<T>;
namespace detail {

template<class T>
struct direct_writable : std::false_type {};
template<class T>
constexpr bool direct_writable_number_v =
	((std::integral<T>
	  && !std::same_as<T, bool>
	  && !std::same_as<T, char>
	  && !std::same_as<T, char8_t>
	  && !std::same_as<T, signed char>
	  && !std::same_as<T, unsigned char>
	  && !std::same_as<T, wchar_t>
	  && !std::same_as<T, char16_t>
	  && !std::same_as<T, char32_t>)
	 || std::floating_point<T>);
template<>
struct direct_writable<bool> : std::true_type {};
template<class T>
	requires direct_writable_number_v<T>
struct direct_writable<T> : std::true_type {};
template<class Traits, class Alloc>
struct direct_writable<std::basic_string<char, Traits, Alloc>> : std::true_type {};
template<>
struct direct_writable<std::string_view> : std::true_type {};
template<class T>
struct direct_writable<std::optional<T>> : direct_writable<std::remove_cvref_t<T>> {};
template<class T>
struct direct_writable<Nullable<T>> : direct_writable<std::remove_cvref_t<T>> {};
template<class T, class Alloc>
struct direct_writable<std::vector<T, Alloc>> : direct_writable<std::remove_cvref_t<T>> {};
template<class T, std::size_t N>
struct direct_writable<std::array<T, N>> : direct_writable<std::remove_cvref_t<T>> {};
template<class A, class B>
struct direct_writable<std::pair<A, B>>
	: std::bool_constant<
		  direct_writable<std::remove_cvref_t<A>>::value && direct_writable<std::remove_cvref_t<B>>::value> {};
template<class... Ts>
struct direct_writable<std::tuple<Ts...>>
	: std::bool_constant<(direct_writable<std::remove_cvref_t<Ts>>::value && ...)> {};
template<class T, class Compare, class Alloc>
struct direct_writable<std::map<std::string, T, Compare, Alloc>> : direct_writable<std::remove_cvref_t<T>> {};
template<class T, class Hash, class KeyEqual, class Alloc>
struct direct_writable<std::unordered_map<std::string, T, Hash, KeyEqual, Alloc>>
	: direct_writable<std::remove_cvref_t<T>> {};
template<class T>
	requires has_members_spec<T>::value
struct direct_writable<T> : std::true_type {};

} // namespace detail

export template<class T>
concept JsonDirectWritable = detail::direct_writable<std::remove_cvref_t<T>>::value;
// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

// Builder state shared across ValueBuilder / child builders.
struct BuilderState {
	DocumentStorage store;
	// Phase 1.5: builder-emitted strings, names, and number lexemes accumulate
	// here. On finish() this becomes store.owned_input; node off/len index
	// into it via kStorageInputView. Built_input is also the rollback target
	// for child-builder partial state.
	std::string built_input;
	bool root_set{false};
	std::size_t root_node{};
	bool child_active{false}; // true when any descendant of ValueBuilder is open
	std::size_t active_depth{}; // depth of the innermost currently-active builder
	// 1 = direct child of ValueBuilder, 2 = grandchild, etc.
};
// Describes where a committed child node gets placed in the parent.
struct ParentSlot {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : std::uint8_t {
		set_root, // top-level: store in BuilderState::root_node
		insert_member, // nested in ObjectBuilder: push to object_members
		append_child, // nested in ArrayBuilder: push parent's local_children
	};
	Kind kind{Kind::set_root};
	std::size_t name_off{}; // insert_member: name offset in string_arena
	std::size_t name_len{}; // insert_member: name length
	std::size_t arena_start{}; // rollback point for string_arena
	bool saved_root_set{}; // set_root only: root_set value before child was opened
	std::vector<std::size_t> *parent_local_children{}; // append_child only: parent's staging V
	std::vector<MemberEntry> *parent_local_members{}; // insert_member only: parent's staging V
};
// Holds the active object/A being built:
struct ChildFrame {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : std::uint8_t {
		object,
		A,
	};
	Kind kind;
	std::size_t depth{}; // this builder's own depth level (1 = direct child of ValueBuilder)
	BuilderState *state{};
	bool committed{};
	ParentSlot parent; // parent.arena_start is the rollback point for string_arena
	std::vector<std::size_t> local_children; // staged A child node indices (A builders only)
	std::vector<MemberEntry> local_members; // staged object members (object builders only)
	std::vector<char const *>
		local_external_ptrs_; // parallel to local_members; non-null only for kMemberExternalView entries
	// Per-session duplicate detection for ObjectBuilder (kind==object only).
	conflux::support::TransparentStringMap<std::size_t> dup_check;
};
export class ObjectBuilder {
	ChildFrame frame_;

	friend class ValueBuilder;
	friend class ArrayBuilder;
	ObjectBuilder(
		BuilderState *st,
		ParentSlot parent = {})
		: frame_{
			  .kind = ChildFrame::Kind::object,
			  .state = st,
			  .parent = parent,
			  .local_children = {},
			  .local_members = {},
			  .local_external_ptrs_ = {},
			  .dup_check = {}} {}
	[[nodiscard]] std::expected<void, JsonError> check_can_insert() const;
	std::expected<void, JsonError> do_insert_node(std::string_view name, std::size_t node_idx);
	std::expected<void, JsonError> do_insert_node_view(std::string_view name, std::size_t node_idx);

public:
	ObjectBuilder(ObjectBuilder &&o) noexcept;
	ObjectBuilder &operator =(ObjectBuilder &&o) noexcept;
	ObjectBuilder(ObjectBuilder const &) = delete;
	ObjectBuilder &operator =(ObjectBuilder const &) = delete;
	// NOLINTNEXTLINE(bugprone-exception-escape)
	void abort_if_open() noexcept;
	~ObjectBuilder() noexcept;
	std::expected<void, JsonError> insert_null(std::string_view name);
	std::expected<void, JsonError> insert_bool(std::string_view name, bool v);
	std::expected<void, JsonError> insert_string(std::string_view name, std::string_view value);
	std::expected<void, JsonError> insert_string_checked(std::string_view name, std::string_view value);
	std::expected<void, JsonError> insert_string_borrowed_name(std::string_view name, std::string_view value);
	std::expected<void, JsonError> insert_string_borrowed(std::string_view name, std::string_view value);
	std::expected<void, JsonError> insert_number(std::string_view name, std::string_view lexeme);
	std::expected<void, JsonError> insert_i64(std::string_view name, std::int64_t v);
	std::expected<void, JsonError> insert_u64(std::string_view name, std::uint64_t v);
	std::expected<void, JsonError> insert_f64(std::string_view name, double v);

	std::expected<ObjectBuilder, JsonError> insert_object(std::string_view name);
	std::expected<ArrayBuilder, JsonError> insert_array(std::string_view name);

	template<class T>
		requires has_json_codec<T>
	std::expected<void, JsonError> insert(std::string_view name, T const &value);
	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept;
};
export class ArrayBuilder {
	ChildFrame frame_;
	[[nodiscard]] static bool arr_check_active(ChildFrame const &f) noexcept;

	friend class ValueBuilder;
	friend class ObjectBuilder;
	ArrayBuilder(
		BuilderState *st,
		ParentSlot parent = {})
		: frame_{
			  .kind = ChildFrame::Kind::A,
			  .state = st,
			  .parent = parent,
			  .local_children = {},
			  .local_members = {},
			  .local_external_ptrs_ = {},
			  .dup_check = {}} {}
	// NOLINTNEXTLINE(bugprone-exception-escape)
	void abort_if_open() noexcept;

public:
	ArrayBuilder(ArrayBuilder &&o) noexcept;
	ArrayBuilder &operator =(ArrayBuilder &&o) noexcept;
	ArrayBuilder(ArrayBuilder const &) = delete;
	ArrayBuilder &operator =(ArrayBuilder const &) = delete;
	~ArrayBuilder() noexcept;
	std::expected<void, JsonError> append_null();
	std::expected<void, JsonError> append_bool(bool v);
	std::expected<void, JsonError> append_string(std::string_view value);
	std::expected<void, JsonError> append_string_checked(std::string_view value);
	std::expected<void, JsonError> append_string_borrowed(std::string_view value);
	std::expected<void, JsonError> append_number(std::string_view lexeme);
	std::expected<void, JsonError> append_i64(std::int64_t v);
	std::expected<void, JsonError> append_u64(std::uint64_t v);
	std::expected<void, JsonError> append_f64(double v);

	std::expected<ObjectBuilder, JsonError> append_object();
	std::expected<ArrayBuilder, JsonError> append_array();

	template<class T>
		requires has_json_codec<T>
	std::expected<void, JsonError> append(T const &value);
	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept;
};
// ---------------------------------------------------------------------------
// ValueBuilder
// ---------------------------------------------------------------------------

namespace detail {

template<class T>
std::expected<std::size_t, JsonError> encode_into(BuilderState *st, T const &value);

template<class T>
std::expected<void, JsonError> encode_dispatch(ValueBuilder &b, T const &value);

} // namespace detail
export class ValueBuilder {
	std::unique_ptr<BuilderState> owned_;
	BuilderState *state_{};

	friend class ObjectBuilder;
	friend class ArrayBuilder;
	template<class T>
	std::expected<std::size_t, JsonError> friend detail::encode_into(BuilderState *, T const &);
	explicit ValueBuilder(BuilderState *borrowed) noexcept;
	[[nodiscard]] std::expected<void, JsonError> check_can_set() const;
	std::expected<void, JsonError> set_node(Node n);

public:
	ValueBuilder();
	ValueBuilder(ValueBuilder &&o) noexcept;
	ValueBuilder &operator =(ValueBuilder &&o) noexcept;
	ValueBuilder(ValueBuilder const &) = delete;
	ValueBuilder &operator =(ValueBuilder const &) = delete;
	std::expected<void, JsonError> set_null();
	std::expected<void, JsonError> set_bool(bool v);
	std::expected<void, JsonError> set_string(std::string_view sv);
	std::expected<void, JsonError> set_number(std::string_view lexeme);
	std::expected<void, JsonError> set_i64(std::int64_t v);
	std::expected<void, JsonError> set_u64(std::uint64_t v);
	std::expected<void, JsonError> set_f64(double v);
	[[nodiscard]] std::expected<ObjectBuilder, JsonError> begin_object();
	[[nodiscard]] std::expected<ArrayBuilder, JsonError> begin_array();

	template<class T>
		requires has_json_codec<T>
	std::expected<void, JsonError> set(T const &value);
	void reset() noexcept;
	void discard() && noexcept;
	[[nodiscard]] std::expected<Document, JsonError> finish() &&;
};
export ValueBuilder value_builder();

export class ArrayWriter;

export class ObjectWriter {
	ObjectBuilder *builder_{};
	std::optional<JsonError> error_{};

	template<class Expected>
	[[nodiscard]] bool remember(
		Expected const &result) {
		if (!result && !error_) {
			error_ = result.error();
		}
		return result.has_value();
	}

public:
	explicit ObjectWriter(
		ObjectBuilder &builder) noexcept
		: builder_{&builder} {}

	[[nodiscard]] std::optional<JsonError> const &error() const noexcept { return error_; }

	template<class T>
	std::expected<void, JsonError> operator ()(
		std::string_view name,
		T const &value) {
		if (error_) {
			return std::unexpected(*error_);
		}
		auto result = [&]() {
			using Clean = std::remove_cvref_t<T>;
			if constexpr (std::same_as<Clean, bool>) {
				return builder_->insert_bool(name, value);
			} else if constexpr (std::is_integral_v<Clean> && std::is_signed_v<Clean>) {
				return builder_->insert_i64(name, static_cast<std::int64_t>(value));
			} else if constexpr (std::is_integral_v<Clean> && std::is_unsigned_v<Clean>) {
				return builder_->insert_u64(name, static_cast<std::uint64_t>(value));
			} else if constexpr (std::is_floating_point_v<Clean>) {
				return builder_->insert_f64(name, static_cast<double>(value));
			} else if constexpr (std::is_convertible_v<T const &, std::string_view>) {
				return builder_->insert_string(name, std::string_view{value});
			} else {
				return builder_->insert(name, value);
			}
		}();
		(void)remember(result);
		return result;
	}

	template<class F>
	std::expected<void, JsonError> object(std::string_view name, F &&fn);

	template<class F>
	std::expected<void, JsonError> array(std::string_view name, F &&fn);
};

class ArrayWriter {
	ArrayBuilder *builder_{};
	std::optional<JsonError> error_{};

	template<class Expected>
	[[nodiscard]] bool remember(
		Expected const &result) {
		if (!result && !error_) {
			error_ = result.error();
		}
		return result.has_value();
	}

public:
	explicit ArrayWriter(
		ArrayBuilder &builder) noexcept
		: builder_{&builder} {}

	[[nodiscard]] std::optional<JsonError> const &error() const noexcept { return error_; }

	template<class T>
	std::expected<void, JsonError> operator ()(
		T const &value) {
		if (error_) {
			return std::unexpected(*error_);
		}
		auto result = [&]() {
			using Clean = std::remove_cvref_t<T>;
			if constexpr (std::same_as<Clean, bool>) {
				return builder_->append_bool(value);
			} else if constexpr (std::is_integral_v<Clean> && std::is_signed_v<Clean>) {
				return builder_->append_i64(static_cast<std::int64_t>(value));
			} else if constexpr (std::is_integral_v<Clean> && std::is_unsigned_v<Clean>) {
				return builder_->append_u64(static_cast<std::uint64_t>(value));
			} else if constexpr (std::is_floating_point_v<Clean>) {
				return builder_->append_f64(static_cast<double>(value));
			} else if constexpr (std::is_convertible_v<T const &, std::string_view>) {
				return builder_->append_string(std::string_view{value});
			} else {
				return builder_->append(value);
			}
		}();
		(void)remember(result);
		return result;
	}

	template<class F>
	std::expected<void, JsonError> object(
		F &&fn) {
		if (error_) {
			return std::unexpected(*error_);
		}
		auto child = builder_->append_object();
		if (!remember(child)) {
			return std::unexpected(*error_);
		}
		ObjectWriter writer{*child};
		std::invoke(std::forward<F>(fn), writer);
		if (writer.error()) {
			error_ = *writer.error();
			return std::unexpected(*error_);
		}
		std::move(*child).commit();
		return {};
	}

	template<class F>
	std::expected<void, JsonError> array(
		F &&fn) {
		if (error_) {
			return std::unexpected(*error_);
		}
		auto child = builder_->append_array();
		if (!remember(child)) {
			return std::unexpected(*error_);
		}
		ArrayWriter writer{*child};
		std::invoke(std::forward<F>(fn), writer);
		if (writer.error()) {
			error_ = *writer.error();
			return std::unexpected(*error_);
		}
		std::move(*child).commit();
		return {};
	}
};

template<class F>
std::expected<void, JsonError> ObjectWriter::object(
	std::string_view name,
	F &&fn) {
	if (error_) {
		return std::unexpected(*error_);
	}
	auto child = builder_->insert_object(name);
	if (!remember(child)) {
		return std::unexpected(*error_);
	}
	ObjectWriter writer{*child};
	std::invoke(std::forward<F>(fn), writer);
	if (writer.error()) {
		error_ = *writer.error();
		return std::unexpected(*error_);
	}
	std::move(*child).commit();
	return {};
}

template<class F>
std::expected<void, JsonError> ObjectWriter::array(
	std::string_view name,
	F &&fn) {
	if (error_) {
		return std::unexpected(*error_);
	}
	auto child = builder_->insert_array(name);
	if (!remember(child)) {
		return std::unexpected(*error_);
	}
	ArrayWriter writer{*child};
	std::invoke(std::forward<F>(fn), writer);
	if (writer.error()) {
		error_ = *writer.error();
		return std::unexpected(*error_);
	}
	std::move(*child).commit();
	return {};
}

export template<class F>
[[nodiscard]] std::expected<Document, JsonError> object(
	F &&fn) {
	auto builder = value_builder();
	auto root = builder.begin_object();
	if (!root) {
		return std::unexpected(std::move(root).error());
	}
	ObjectWriter writer{*root};
	std::invoke(std::forward<F>(fn), writer);
	if (writer.error()) {
		return std::unexpected(*writer.error());
	}
	std::move(*root).commit();
	return std::move(builder).finish();
}

export template<class F>
[[nodiscard]] std::expected<Document, JsonError> array(
	F &&fn) {
	auto builder = value_builder();
	auto root = builder.begin_array();
	if (!root) {
		return std::unexpected(std::move(root).error());
	}
	ArrayWriter writer{*root};
	std::invoke(std::forward<F>(fn), writer);
	if (writer.error()) {
		return std::unexpected(*writer.error());
	}
	std::move(*root).commit();
	return std::move(builder).finish();
}

export [[nodiscard]] std::expected<Document, JsonError> merge_patch(NodeRef target, NodeRef patch);
export [[nodiscard]] std::expected<Document, JsonError> merge_patch(Document const &target, Document const &patch);

export enum class JsonPatchOp {
	add,
	remove,
	replace,
	move,
	copy,
	test,
};
export struct JsonPatchOptions {
	std::size_t max_operations{1024};
	std::size_t max_pointer_depth{128};
	bool reject_duplicate_object_members{true};
	bool allow_missing_remove{false};
};
export [[nodiscard]] std::expected<Document, JsonError>
apply_patch(NodeRef target, NodeRef patch, JsonPatchOptions opts = {});
export [[nodiscard]] std::expected<Document, JsonError>
apply_patch(Document const &target, Document const &patch, JsonPatchOptions opts = {});
export [[nodiscard]] std::expected<void, JsonError> validate_patch(NodeRef patch, JsonPatchOptions opts = {});
// Internal helpers: encode a value of type T into a shared BuilderState,
// returning the resulting node index. Rolls back on failure.
// Used by ArrayBuilder::append<T> and ObjectBuilder::insert<T>.
namespace detail {

template<class T>
std::expected<std::size_t, JsonError> encode_into(
	BuilderState *st,
	T const &value) {
	std::size_t const nodes_saved = st->store.nodes.size();
	std::size_t const arena_saved = st->built_input.size();
	std::size_t const arr_saved = st->store.array_children.size();
	std::size_t const obj_saved = st->store.object_members.size();
	bool const root_set_saved = st->root_set;
	bool const child_active_saved = st->child_active;
	std::size_t const active_depth_saved = st->active_depth;

	st->root_set = false;
	st->child_active = false;
	st->active_depth = 0;
	ValueBuilder vb{st};
	auto ok = encode_dispatch<T>(vb, value);
	if (!ok) {
		st->store.nodes.resize(nodes_saved);
		st->built_input.resize(arena_saved);
		st->store.array_children.resize(arr_saved);
		st->store.object_members.resize(obj_saved);
		st->root_set = root_set_saved;
		st->child_active = child_active_saved;
		st->active_depth = active_depth_saved;
		return std::unexpected(std::move(ok).error());
	}
	std::size_t const node_idx = st->root_node;
	st->root_set = root_set_saved;
	st->child_active = child_active_saved;
	st->active_depth = active_depth_saved;
	return node_idx;
}

} // namespace detail
// ---------------------------------------------------------------------------
// Nullable<T>
// ---------------------------------------------------------------------------

export template<class T>
class Nullable {
	std::optional<T> val_;

public:
	constexpr Nullable() noexcept = default;
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Nullable(
		nullptr_t) noexcept {}
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Nullable(
		T value)
		: val_{std::move(value)} {}
	Nullable(Nullable const &) = default;
	Nullable(Nullable &&) noexcept = default;
	Nullable &operator =(Nullable const &) = default;
	Nullable &operator =(Nullable &&) noexcept = default;
	[[nodiscard]] constexpr bool is_null() const noexcept { return !val_.has_value(); }
	[[nodiscard]] constexpr bool has_value() const noexcept { return val_.has_value(); }
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
	[[nodiscard]] constexpr decltype(auto) value(
		this auto &&self) {
		return *std::forward<decltype(self)>(self).val_;
	}
	[[nodiscard]] constexpr T &operator *() & noexcept { return *val_; }
	[[nodiscard]] constexpr T const &operator *() const & noexcept { return *val_; }
	[[nodiscard]] constexpr T *operator ->() noexcept { return &*val_; }
	[[nodiscard]] constexpr T const *operator ->() const noexcept { return &*val_; }
	template<class U>
		requires std::convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) const & {
		return val_ ? *val_ : static_cast<T>(std::forward<U>(fallback));
	}
	template<class U>
		requires std::convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) && {
		return val_ ? std::move(*val_) : static_cast<T>(std::forward<U>(fallback));
	}
	bool friend operator ==(
		Nullable const &a,
		Nullable const &b)
		requires std::equality_comparable<T>
	{
		return a.val_ == b.val_;
	}
	auto friend operator <=>(
		Nullable const &a,
		Nullable const &b)
		requires std::three_way_comparable<T>
	{
		return a.val_ <=> b.val_;
	}
};

} // namespace conflux::json

template<class T>
struct std::hash<conflux::json::Nullable<T>> {
	std::size_t operator ()(
		conflux::json::Nullable<T> const &n) const noexcept {
		if (!n.has_value()) {
			return 0;
		}
		return std::hash<T>{}(n.value());
	}
};

namespace conflux::json {

// ---------------------------------------------------------------------------
// JsonCodec / JsonMembers / decode
// ---------------------------------------------------------------------------
namespace detail {

template<class T>
concept codec_decodes_node = requires(NodeRef root) {
	{ JsonCodec<T>::decode(root) } -> std::same_as<std::expected<T, JsonError>>;
};

template<class T>
concept codec_decodes_node_with_options = requires(NodeRef root, JsonDecodeOptions const &opts) {
	{ JsonCodec<T>::decode(root, opts) } -> std::same_as<std::expected<T, JsonError>>;
};

template<class T>
concept codec_decodes_reader_event_with_options =
	requires(JsonReader &reader, JsonReader::Event event, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch) {
		{ JsonCodec<T>::decode(reader, event, opts, scratch) } -> std::same_as<std::expected<T, JsonError>>;
	};

template<ParseMode Mode, class T>
concept codec_decodes_reader_event_mode_with_options =
	requires(JsonReader &reader, JsonReader::Event event, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch) {
		{
			JsonCodec<T>::template decode<Mode>(reader, event, opts, scratch)
		} -> std::same_as<std::expected<T, JsonError>>;
	};

template<class T>
concept codec_encodes_value = requires(ValueBuilder &builder, T const &value) {
	{ JsonCodec<T>::encode(builder, value) } -> std::same_as<std::expected<void, JsonError>>;
};

template<class T>
struct has_codec_spec<T>
	: std::bool_constant<codec_encodes_value<T> && (codec_decodes_node<T> || codec_decodes_node_with_options<T>)> {};

template<class T>
std::expected<T, JsonError> decode_codec(
	NodeRef root,
	JsonDecodeOptions const &opts) {
	if constexpr (codec_decodes_node_with_options<T>) {
		return JsonCodec<T>::decode(root, opts);
	} else {
		return JsonCodec<T>::decode(root);
	}
}
template<class T>
struct has_members_spec<T, std::void_t<decltype(conflux::json::JsonMembers<T>::members())>>
	: std::bool_constant<std::default_initializable<T>> {};
// Overload set: extract JsonMember from plain or constrained member entry.
template<class T, class M>
[[nodiscard]] constexpr conflux::json::JsonMember<T, M> const &jm_member(
	conflux::json::JsonMember<T, M> const &jm) noexcept {
	return jm;
}
template<class T, class M>
[[nodiscard]] constexpr conflux::json::JsonMember<T, M> const &jm_member(
	std::tuple<conflux::json::JsonMember<T, M>, conflux::json::JsonConstraintFn<M>> const &t) noexcept {
	return get<0>(t);
}
// Overload set: extract constraint fn-ptr (nullptr = none).
template<class T, class M>
[[nodiscard]] constexpr conflux::json::JsonConstraintFn<M> jm_constraint(
	conflux::json::JsonMember<T, M> const &) noexcept {
	return nullptr;
}
template<class T, class M>
[[nodiscard]] constexpr conflux::json::JsonConstraintFn<M> jm_constraint(
	std::tuple<conflux::json::JsonMember<T, M>, conflux::json::JsonConstraintFn<M>> const &t) noexcept {
	return get<1>(t);
}

template<class T>
struct json_contains_borrowed_view_impl;

template<class Entry>
struct json_member_entry_value_type {
	using type = void;
};
template<class T, class M>
struct json_member_entry_value_type<conflux::json::JsonMember<T, M>> {
	using type = M;
};
template<class T, class M>
struct json_member_entry_value_type<std::tuple<conflux::json::JsonMember<T, M>, conflux::json::JsonConstraintFn<M>>> {
	using type = M;
};
template<class Entry>
using json_member_entry_value_type_t = typename json_member_entry_value_type<std::remove_cvref_t<Entry>>::type;

template<class MembersTuple, std::size_t... Is>
consteval bool json_member_tuple_contains_borrowed_view(
	std::index_sequence<Is...>) {
	return (
		...
		|| json_contains_borrowed_view_impl<
			std::remove_cvref_t<json_member_entry_value_type_t<std::tuple_element_t<Is, MembersTuple>>>>::value);
}

template<class T>
consteval bool json_members_contain_borrowed_view() {
	if constexpr (has_members_spec<T>::value) {
		using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
		return json_member_tuple_contains_borrowed_view<MembersTuple>(
			std::make_index_sequence<std::tuple_size_v<MembersTuple>>{});
	} else {
		return false;
	}
}

template<class T>
struct json_contains_borrowed_view_impl
	: std::bool_constant<
		  conflux::json::JsonBorrowedViewFields<std::remove_cvref_t<T>>::value
		  || json_members_contain_borrowed_view<std::remove_cvref_t<T>>()> {};
template<>
struct json_contains_borrowed_view_impl<std::string_view> : std::true_type {};
template<class T, std::size_t Extent>
struct json_contains_borrowed_view_impl<std::span<T, Extent>> : std::true_type {};
template<class T>
struct json_contains_borrowed_view_impl<std::optional<T>> : json_contains_borrowed_view_impl<std::remove_cvref_t<T>> {};
template<class T>
struct json_contains_borrowed_view_impl<Nullable<T>> : json_contains_borrowed_view_impl<std::remove_cvref_t<T>> {};
template<class T, class Alloc>
struct json_contains_borrowed_view_impl<std::vector<T, Alloc>>
	: json_contains_borrowed_view_impl<std::remove_cvref_t<T>> {};
template<class T, std::size_t N>
struct json_contains_borrowed_view_impl<std::array<T, N>> : json_contains_borrowed_view_impl<std::remove_cvref_t<T>> {};

} // namespace detail

export template<class T>
inline constexpr bool json_contains_borrowed_view_v =
	detail::json_contains_borrowed_view_impl<std::remove_cvref_t<T>>::value;

// Built-in specializations declared here, defined below.
template<>
struct JsonCodec<bool>;
template<>
struct JsonCodec<std::int64_t>;
template<>
struct JsonCodec<std::uint64_t>;
template<>
struct JsonCodec<double>;
template<class Traits, class Alloc>
struct JsonCodec<std::basic_string<char, Traits, Alloc>>;
template<>
struct JsonCodec<std::string_view>;
template<class T>
struct JsonCodec<std::optional<T>>;
template<class T>
struct JsonCodec<Nullable<T>>;
template<class T, class Alloc>
struct JsonCodec<std::vector<T, Alloc>>;
template<class T, std::size_t N>
struct JsonCodec<std::array<T, N>>;
template<class A, class B>
struct JsonCodec<std::pair<A, B>>;
template<class... Ts>
struct JsonCodec<std::tuple<Ts...>>;
template<class T, class Compare, class Alloc>
struct JsonCodec<std::map<std::string, T, Compare, Alloc>>;
template<class T, class Hash, class KeyEqual, class Alloc>
struct JsonCodec<std::unordered_map<std::string, T, Hash, KeyEqual, Alloc>>;

export template<class T>
std::expected<T, JsonError> decode(NodeRef root, JsonDecodeOptions const &opts = {});

export template<class T>
std::expected<T, JsonError> decode(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
std::expected<T, JsonError>
decode_direct(JsonReader &reader, JsonDecodeOptions const &opts = {}, JsonDecodeScratch *scratch = nullptr);
export template<class T>
std::expected<T, JsonError> decode_next(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
std::expected<T, JsonError> decode_full(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
std::expected<T, JsonError>
decode_full(std::string_view input, JsonParseOptions const &parse_opts = {}, JsonDecodeOptions const &decode_opts = {});
export template<class T>
std::expected<void, JsonError> decode_full_into(
	T &out,
	std::string_view input,
	JsonParseOptions const &parse_opts = {},
	JsonDecodeOptions const &decode_opts = {});
export template<class T>
std::expected<void, JsonError> write_json_direct(std::string &out, T const &value, JsonDumpOptions const &opts = {});
export template<class T>
std::expected<std::string, JsonError> dump_direct(T const &value, JsonDumpOptions const &opts = {});
export template<class T>
std::expected<T, JsonError> decode(
	Document const &d,
	JsonDecodeOptions const &opts = {}) {
	return decode<T>(d.root(), opts);
}

template<class T>
std::expected<T, JsonError> NodeRef::as(
	JsonDecodeOptions const &opts) const {
	return decode<T>(*this, opts);
}

template<class T>
std::expected<T, JsonError> ObjectView::required(
	std::string_view name,
	JsonDecodeOptions const &opts) const {
	auto node = member(name);
	if (!node) {
		return std::unexpected(std::move(node).error());
	}
	return decode<T>(*node, opts);
}

template<class T>
std::expected<std::optional<T>, JsonError> ObjectView::optional(
	std::string_view name,
	JsonDecodeOptions const &opts) const {
	auto node = find_member(name);
	if (!node) {
		return std::optional<T>{};
	}
	auto value = decode<T>(*node, opts);
	if (!value) {
		return std::unexpected(std::move(value).error());
	}
	return std::optional<T>{std::move(*value)};
}

template<class T>
std::expected<T, JsonError> Document::get(
	std::string_view pointer,
	JsonDecodeOptions const &opts) const {
	auto node = root().at_pointer(pointer);
	if (!node) {
		return std::unexpected(std::move(node).error());
	}
	return decode<T>(*node, opts);
}
// Built-in JsonCodec specializations.

template<>
struct JsonCodec<bool> {
	static std::expected<bool, JsonError> decode(
		NodeRef n) {
		return n.as_bool();
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		bool v) {
		return b.set_bool(v);
	}
	static constexpr std::string_view type_name() { return "bool"; }
};
template<>
struct JsonCodec<std::int64_t> {
	static std::expected<std::int64_t, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return std::unexpected(std::move(num).error());
		}
		return num->to_i64();
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::int64_t v) {
		return b.set_i64(v);
	}
	static constexpr std::string_view type_name() { return "i64"; }
};
template<>
struct JsonCodec<std::uint64_t> {
	static std::expected<std::uint64_t, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return std::unexpected(std::move(num).error());
		}
		return num->to_u64();
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::uint64_t v) {
		return b.set_u64(v);
	}
	static constexpr std::string_view type_name() { return "u64"; }
};
template<>
struct JsonCodec<double> {
	static std::expected<double, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return std::unexpected(std::move(num).error());
		}
		return num->to_f64();
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		double v) {
		return b.set_f64(v);
	}
	static constexpr std::string_view type_name() { return "double"; }
};
template<class Traits, class Alloc>
struct JsonCodec<std::basic_string<char, Traits, Alloc>> {
	using String = std::basic_string<char, Traits, Alloc>;

	static std::expected<String, JsonError> decode(
		NodeRef n) {
		auto sv = n.as_string();
		if (!sv) {
			return std::unexpected(std::move(sv).error());
		}
		return String{*sv};
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		String const &v) {
		return b.set_string(std::string_view{v.data(), v.size()});
	}
	static constexpr std::string_view type_name() { return "string"; }
};
template<>
struct JsonCodec<std::string_view> {
	static std::expected<std::string_view, JsonError> decode(
		NodeRef n) {
		return n.as_string();
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::string_view v) {
		return b.set_string(v);
	}
	static constexpr std::string_view type_name() { return "std::string_view"; }
};
template<class T>
struct JsonCodec<std::optional<T>> {
	static std::expected<std::optional<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			if constexpr (detail::is_nullable_type<T>::value) {
				auto v = conflux::json::decode<T>(n);
				if (!v) {
					return std::unexpected(std::move(v).error());
				}
				return std::optional<T>{std::move(*v)};
			} else {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::wrong_kind,
						.expected_kind = JsonKind::null,
						.actual_kind = JsonKind::null,
						.message = "explicit JSON null is not accepted for std::optional<T>; use Nullable<T> for "
								   "nullable fields"});
			}
		}
		auto v = conflux::json::decode<T>(n);
		if (!v) {
			return std::unexpected(std::move(v).error());
		}
		return std::optional<T>{std::move(*v)};
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::optional<T> const &v) {
		if (!v) {
			return b.set_null();
		}
		return JsonCodec<T>::encode(b, *v);
	}
	static constexpr std::string_view type_name() { return "Opt"; }
};
template<class T>
struct JsonCodec<Nullable<T>> {
	static std::expected<Nullable<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			return Nullable<T>{};
		}
		auto v = conflux::json::decode<T>(n);
		if (!v) {
			return std::unexpected(std::move(v).error());
		}
		return Nullable<T>{std::move(*v)};
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		Nullable<T> const &v) {
		if (v.is_null()) {
			return b.set_null();
		}
		return JsonCodec<T>::encode(b, v.value());
	}
	static constexpr std::string_view type_name() { return "Nullable"; }
};
namespace detail {

template<class Vec>
std::expected<Vec, JsonError> decode_array_elements(
	ArrayView const &arr) {
	using T = typename Vec::value_type;
	Vec result;
	result.reserve(arr.size());
	for (std::size_t i = 0; i < arr.size(); ++i) {
		auto elem = arr.element(i);
		if (!elem) {
			return std::unexpected(std::move(elem).error());
		}
		auto v = conflux::json::decode<T>(*elem);
		if (!v) {
			JsonPath prefix;
			prefix.push_index(i);
			return std::unexpected(std::move(v).error().with_prefix(prefix));
		}
		result.push_back(std::move(*v));
	}
	return result;
}

} // namespace detail
template<class T, class Alloc>
struct JsonCodec<std::vector<T, Alloc>> {
	using Vec = std::vector<T, Alloc>;

	static std::expected<Vec, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		return detail::decode_array_elements<Vec>(*arr);
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		Vec const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return std::unexpected(std::move(arr_res).error());
		}
		auto &arr = *arr_res;
		for (auto const &elem: v) {
			if (auto ok = arr.template append<T>(elem); !ok) {
				return std::unexpected(std::move(ok).error());
			}
		}
		std::move(arr).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "V"; }
};
template<class T, std::size_t N>
struct JsonCodec<std::array<T, N>> {
	static std::expected<std::array<T, N>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		if (arr->size() != N) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = std::string{type_name()},
					.container_size = N,
					.message = std::format("std::expected array of length {}, got {}", N, arr->size())});
		}
		std::array<T, N> result{};
		for (std::size_t i = 0; i < N; ++i) {
			auto elem = arr->element(i);
			if (!elem) {
				return std::unexpected(std::move(elem).error());
			}
			auto v = conflux::json::decode<T>(*elem);
			if (!v) {
				JsonPath prefix;
				prefix.push_index(i);
				return std::unexpected(std::move(v).error().with_prefix(prefix));
			}
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			result[i] = std::move(*v);
		}
		return result;
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::array<T, N> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return std::unexpected(std::move(arr_res).error());
		}
		auto &arr = *arr_res;
		for (std::size_t i = 0; i < N; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			if (auto ok = arr.template append<T>(v[i]); !ok) {
				return std::unexpected(std::move(ok).error());
			}
		}
		std::move(arr).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "array"; }
};
template<class A, class B>
struct JsonCodec<std::pair<A, B>> {
	static std::expected<std::pair<A, B>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		if (arr->size() != 2) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = std::string{type_name()},
					.container_size = 2UZ,
					.message = std::format("std::expected array of length 2, got {}", arr->size())});
		}
		auto e0 = arr->element(0);
		if (!e0) {
			return std::unexpected(std::move(e0).error());
		}
		auto first = conflux::json::decode<A>(*e0);
		if (!first) {
			JsonPath prefix;
			prefix.push_index(0);
			return std::unexpected(std::move(first).error().with_prefix(prefix));
		}
		auto e1 = arr->element(1);
		if (!e1) {
			return std::unexpected(std::move(e1).error());
		}
		auto second = conflux::json::decode<B>(*e1);
		if (!second) {
			JsonPath prefix;
			prefix.push_index(1);
			return std::unexpected(std::move(second).error().with_prefix(prefix));
		}
		return std::pair<A, B>{std::move(*first), std::move(*second)};
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::pair<A, B> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return std::unexpected(std::move(arr_res).error());
		}
		auto &arr = *arr_res;
		if (auto ok = arr.template append<A>(v.first); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (auto ok = arr.template append<B>(v.second); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		std::move(arr).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "P"; }
};
template<class... Ts>
struct JsonCodec<std::tuple<Ts...>> {
	static std::expected<std::tuple<Ts...>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		constexpr std::size_t N = sizeof...(Ts);
		if (arr->size() != N) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = std::string{type_name()},
					.container_size = N,
					.message = std::format("std::expected array of length {}, got {}", N, arr->size())});
		}
		std::tuple<Ts...> result{};
		bool ok = true;
		JsonError first_err;
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(([&]<std::size_t I>() {
				 if (!ok) {
					 return;
				 }
				 auto elem = arr->element(I);
				 if (!elem) {
					 ok = false;
					 first_err = std::move(elem).error();
					 return;
				 }
				 auto v = conflux::json::decode<std::tuple_element_t<I, std::tuple<Ts...>>>(*elem);
				 if (!v) {
					 ok = false;
					 JsonPath prefix;
					 prefix.push_index(I);
					 first_err = std::move(v).error().with_prefix(prefix);
					 return;
				 }
				 get<I>(result) = std::move(*v);
			 }.template operator ()<Is>()),
			 ...);
		}(std::make_index_sequence<N>{});
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		return result;
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		std::tuple<Ts...> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return std::unexpected(std::move(arr_res).error());
		}
		auto &arr = *arr_res;
		bool ok = true;
		JsonError first_err;
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(([&]<std::size_t I>() {
				 if (!ok) {
					 return;
				 }
				 auto res = arr.template append<std::tuple_element_t<I, std::tuple<Ts...>>>(get<I>(v));
				 if (!res) {
					 ok = false;
					 first_err = std::move(res).error();
				 }
			 }.template operator ()<Is>()),
			 ...);
		}(std::make_index_sequence<sizeof...(Ts)>{});
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		std::move(arr).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "Tup"; }
};
template<class T, class Compare, class Alloc>
struct JsonCodec<std::map<std::string, T, Compare, Alloc>> {
	using Map = std::map<std::string, T, Compare, Alloc>;
	static std::expected<Map, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		Map result;
		for (auto const &[name, val]: obj->members()) {
			auto v = conflux::json::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return std::unexpected(std::move(v).error().with_prefix(prefix));
			}
			result.emplace(std::string{name}, std::move(*v));
		}
		return result;
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		Map const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto &obj = *obj_res;
		for (auto const &[key, val]: v) {
			if (auto ok = obj.template insert<T>(key, val); !ok) {
				return std::unexpected(std::move(ok).error());
			}
		}
		std::move(obj).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "M"; }
};
template<class T, class Hash, class KeyEqual, class Alloc>
struct JsonCodec<std::unordered_map<std::string, T, Hash, KeyEqual, Alloc>> {
	using Map = std::unordered_map<std::string, T, Hash, KeyEqual, Alloc>;
	static std::expected<Map, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		Map result;
		result.reserve(obj->size());
		for (auto const &[name, val]: obj->members()) {
			auto v = conflux::json::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return std::unexpected(std::move(v).error().with_prefix(prefix));
			}
			result.emplace(std::string{name}, std::move(*v));
		}
		return result;
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		Map const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto &obj = *obj_res;
		for (auto const &[key, val]: v) {
			if (auto ok = obj.template insert<T>(key, val); !ok) {
				return std::unexpected(std::move(ok).error());
			}
		}
		std::move(obj).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "UM"; }
};
// ---------------------------------------------------------------------------
// JsonReader-based decode
// ---------------------------------------------------------------------------

namespace detail {

template<ParseMode Mode>
[[nodiscard]] inline std::expected<void, JsonError> skip_remaining_reader(
	JsonReader &r,
	JsonReader::Event ev) {
	return r.skip_remaining_value_impl<Mode>(ev);
}

template<ParseMode Mode, class Vector>
[[nodiscard]] inline std::expected<void, JsonError> reserve_vector_from_remaining_array(
	Vector &out,
	JsonReader &reader) {
	auto count = reader.count_remaining_array_elements_impl<Mode>();
	if (!count) {
		return std::unexpected(std::move(count).error());
	}
	out.reserve(*count);
	return {};
}

template<ParseMode Mode, class Map>
[[nodiscard]] inline std::expected<void, JsonError> reserve_map_from_remaining_object(
	Map &out,
	JsonReader &reader) {
	if constexpr (requires(Map &m, std::size_t n) { m.reserve(n); }) {
		auto count = reader.count_remaining_object_members_impl<Mode>();
		if (!count) {
			return std::unexpected(std::move(count).error());
		}
		out.reserve(*count);
	}
	return {};
}

template<ParseMode Mode, class T>
std::expected<T, JsonError>
decode_from_event(JsonReader &r, JsonReader::Event ev, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch);
template<ParseMode Mode, class T>
std::expected<T, JsonError> decode_from_event(
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts) {
	JsonDecodeScratch scratch;
	return decode_from_event<Mode, T>(r, ev, opts, &scratch);
}

[[nodiscard]] inline std::expected<std::string_view, JsonError> key_view_from_token(
	JsonStringToken const &token,
	JsonDecodeScratch &scratch) {
	if (auto borrowed = token.unescaped_borrow()) {
		return *borrowed;
	}
	auto const needed = token.max_decoded_size();
	if (needed <= scratch.key_inline.size()) {
		return token.decode_into(std::span<char>{scratch.key_inline.data(), scratch.key_inline.size()});
	}
	scratch.key_overflow.resize(needed);
	return token.decode_into(std::span<char>{scratch.key_overflow.data(), scratch.key_overflow.size()});
}

template<class String>
[[nodiscard]] inline std::expected<void, JsonError>
decode_string_into(String &out, JsonStringToken const &token, JsonDecodeScratch *scratch = nullptr);

template<class String>
[[nodiscard]] inline std::expected<String, JsonError> string_from_token(
	JsonStringToken const &token,
	JsonDecodeScratch *scratch = nullptr) {
	String out;
	if (auto ok = decode_string_into(out, token, scratch); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return out;
}

[[nodiscard]] inline bool found_bit_is_set(
	std::uint64_t inline_bits,
	std::span<std::uint64_t const> overflow_bits,
	std::size_t idx) noexcept {
	if (idx < 64) {
		return (inline_bits & (std::uint64_t{1} << idx)) != 0;
	}
	auto const word = idx / 64;
	auto const bit = idx % 64;
	return word < overflow_bits.size() && (overflow_bits[word] & (std::uint64_t{1} << bit)) != 0;
}

inline void set_found_bit(
	std::uint64_t &inline_bits,
	std::pmr::vector<std::uint64_t> &overflow_bits,
	std::size_t member_count,
	std::size_t idx) {
	if (member_count <= 64) {
		inline_bits |= std::uint64_t{1} << idx;
		return;
	}
	auto const words = (member_count + 63) / 64;
	if (overflow_bits.size() < words) {
		overflow_bits.assign(words, 0);
	}
	auto const word = idx / 64;
	auto const bit = idx % 64;
	overflow_bits[word] |= std::uint64_t{1} << bit;
}

struct JsonPresenceBits {
	static constexpr std::size_t kInlineWords = 4;
	std::array<std::uint64_t, kInlineWords> inline_words{};
	std::pmr::vector<std::uint64_t> overflow;
	std::size_t word_count{};

	explicit JsonPresenceBits(
		std::pmr::memory_resource *resource)
		: overflow{resource != nullptr ? resource : std::pmr::get_default_resource()} {}

	void reset(
		std::size_t member_count) {
		word_count = (member_count + 63U) / 64U;
		if (word_count <= kInlineWords) {
			std::ranges::fill(inline_words, 0);
			overflow.clear();
		} else {
			overflow.assign(word_count, 0);
		}
	}

	[[nodiscard]] bool test(
		std::size_t idx) const noexcept {
		auto const word = idx / 64U;
		auto const bit = idx % 64U;
		if (word_count <= kInlineWords) {
			return (inline_words[word] & (std::uint64_t{1} << bit)) != 0;
		}
		return word < overflow.size() && (overflow[word] & (std::uint64_t{1} << bit)) != 0;
	}

	void set(
		std::size_t idx) {
		auto const word = idx / 64U;
		auto const bit = idx % 64U;
		if (word_count <= kInlineWords) {
			inline_words[word] |= std::uint64_t{1} << bit;
			return;
		}
		overflow[word] |= std::uint64_t{1} << bit;
	}
};

struct JsonInlinePresenceBits {
	std::uint64_t bits{};

	explicit JsonInlinePresenceBits(
		std::pmr::memory_resource *) noexcept {}

	void reset(
		std::size_t) noexcept {
		bits = 0;
	}

	[[nodiscard]] bool test(
		std::size_t idx) const noexcept {
		return (bits & (std::uint64_t{1} << idx)) != 0;
	}

	void set(
		std::size_t idx) noexcept {
		bits |= std::uint64_t{1} << idx;
	}
};

[[nodiscard]] inline JsonError duplicate_member_error(
	std::string_view name) {
	return JsonError{
		.stage = JsonStage::decode,
		.code = JsonIssueCode::duplicate_member,
		.member_name = std::string{name},
		.message = std::format("duplicate member: {}", name)};
}

template<class String>
[[nodiscard]] inline std::expected<void, JsonError> decode_string_into(
	String &out,
	JsonStringToken const &token,
	JsonDecodeScratch *scratch) {
	if (auto borrowed = token.unescaped_borrow()) {
		out.assign(borrowed->data(), borrowed->size());
		return {};
	}
	std::size_t const needed = token.max_decoded_size();
	if (scratch != nullptr && needed <= scratch->string_inline.size()) {
		auto res = token.decode_into(std::span<char>{scratch->string_inline.data(), scratch->string_inline.size()});
		if (!res) {
			out.clear();
			return std::unexpected(std::move(res).error());
		}
		out.assign(res->data(), res->size());
		return {};
	}
	out.clear();
	out.resize(needed);
	auto res = token.decode_into(std::span<char>{out.data(), out.size()});
	if (!res) {
		out.clear();
		return std::unexpected(std::move(res).error());
	}
	out.resize(res->size());
	return {};
}

template<ParseMode Mode, class T>
std::expected<void, JsonError>
decode_into(T &out, JsonReader &r, JsonReader::Event ev, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch);
template<ParseMode Mode, class T>
std::expected<void, JsonError> decode_members_from_event_into(
	T &result,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch);

template<ParseMode Mode, class Map>
[[nodiscard]] std::expected<void, JsonError>
decode_map_from_reader_into(Map &out, JsonReader &r, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch);

template<ParseMode Mode, class T>
std::expected<void, JsonError> decode_next_object_value_into(
	T &out,
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	if constexpr (std::same_as<T, bool>) {
		return r.next_object_bool_value_impl<Mode>(out);
	} else if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>) {
		return r.next_object_number_value_impl<Mode>(out);
	} else if constexpr (is_basic_string_of_char_v<T>) {
		JsonDecodeScratch local_scratch;
		JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;
		auto view = r.next_object_string_value_view_impl<Mode>(decode_scratch);
		if (!view) {
			return std::unexpected(std::move(view).error());
		}
		out.assign(view->data(), view->size());
		return {};
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		auto is_null = r.try_next_object_null_value_impl<Mode>();
		if (!is_null) {
			return std::unexpected(std::move(is_null).error());
		}
		if (*is_null) {
			if constexpr (is_nullable_type<Inner>::value) {
				out.emplace();
			} else {
				out.reset();
			}
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			out.emplace();
			auto decoded = decode_next_object_value_into<Mode, Inner>(*out, r, opts, scratch);
			if (!decoded) {
				out.reset();
				return std::unexpected(std::move(decoded).error());
			}
			return {};
		} else {
			auto ev = r.next_object_value_event_impl<Mode>();
			if (!ev) {
				out.reset();
				return std::unexpected(std::move(ev).error());
			}
			auto decoded = decode_from_event<Mode, Inner>(r, *ev, opts, scratch);
			if (!decoded) {
				out.reset();
				return std::unexpected(std::move(decoded).error());
			}
			out.emplace(std::move(*decoded));
			return {};
		}
	} else if constexpr (is_nullable_type<T>::value) {
		using Inner = nullable_inner_t<T>;
		auto is_null = r.try_next_object_null_value_impl<Mode>();
		if (!is_null) {
			return std::unexpected(std::move(is_null).error());
		}
		if (*is_null) {
			out = T{};
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			Inner value{};
			auto decoded = decode_next_object_value_into<Mode, Inner>(value, r, opts, scratch);
			if (!decoded) {
				out = T{};
				return std::unexpected(std::move(decoded).error());
			}
			out = T{std::move(value)};
			return {};
		} else {
			auto ev = r.next_object_value_event_impl<Mode>();
			if (!ev) {
				out = T{};
				return std::unexpected(std::move(ev).error());
			}
			auto decoded = decode_from_event<Mode, Inner>(r, *ev, opts, scratch);
			if (!decoded) {
				out = T{};
				return std::unexpected(std::move(decoded).error());
			}
			out = T{std::move(*decoded)};
			return {};
		}
	} else if constexpr (is_vector_of_v<T>) {
		using E = typename T::value_type;
		auto opened = r.next_object_array_value_impl<Mode>();
		if (!opened) {
			return std::unexpected(std::move(opened).error());
		}
		out.clear();
		if constexpr (std::floating_point<E>) {
			return r.decode_floating_array_into<Mode>(out);
		} else if constexpr (std::integral<E> && !std::same_as<E, bool>) {
			return r.decode_integral_array_into<Mode>(out);
		} else if constexpr (is_basic_string_of_char_v<E>) {
			JsonDecodeScratch local_scratch;
			JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;
			return r.decode_string_array_into<Mode>(out, decode_scratch);
		} else if constexpr (is_fixed_numeric_array_v<E>) {
			return r.decode_fixed_numeric_array_vector_into<Mode>(out);
		} else {
			return decode_into<Mode, T>(out, r, JsonReader::Event::begin_array, opts, scratch);
		}
	} else if constexpr (is_std_array_v<T> || is_pair_v<T> || is_tuple_of_v<T>) {
		auto opened = r.next_object_array_value_impl<Mode>();
		if (!opened) {
			return std::unexpected(std::move(opened).error());
		}
		return decode_into<Mode, T>(out, r, JsonReader::Event::begin_array, opts, scratch);
	} else if constexpr (is_map_type_v<T> || is_unordered_map_type_v<T>) {
		auto opened = r.next_object_object_value_impl<Mode>();
		if (!opened) {
			return std::unexpected(std::move(opened).error());
		}
		return decode_map_from_reader_into<Mode>(out, r, opts, scratch);
	} else if constexpr (has_members_spec<T>::value) {
		auto opened = r.next_object_object_value_impl<Mode>();
		if (!opened) {
			return std::unexpected(std::move(opened).error());
		}
		return decode_members_from_event_into<Mode, T>(out, r, JsonReader::Event::begin_object, opts, scratch);
	} else {
		auto ev = r.next_object_value_event_impl<Mode>();
		if (!ev) {
			return std::unexpected(std::move(ev).error());
		}
		return decode_into<Mode, T>(out, r, *ev, opts, scratch);
	}
}

inline constexpr std::size_t kJsonMemberLinearLookupLimit = 4;

[[nodiscard]] constexpr std::uint64_t json_member_name_hash(
	std::string_view name) noexcept {
	std::uint64_t h = 1469598103934665603ULL;
	for (char c: name) {
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ULL;
	}
	return h;
}

[[nodiscard]] constexpr bool json_member_raw_name_fast_path_safe(
	std::string_view name) noexcept {
	for (char ch: name) {
		auto const c = static_cast<unsigned char>(ch);
		if (c < 0x20U || c == '"' || c == '\\' || c >= 0x80U) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] constexpr std::size_t json_member_lookup_capacity(
	std::size_t member_count) noexcept {
	std::size_t capacity = 1;
	while (capacity < member_count * 2) {
		capacity <<= 1;
	}
	return capacity;
}

template<class T>
using JsonMemberDecodeDirectFn =
	std::expected<void, JsonError> (*)(T &, JsonReader &, JsonDecodeOptions const &, JsonDecodeScratch *);

template<class T>
struct JsonMemberLookupEntry {
	std::string_view name{};
	std::uint64_t hash{};
	std::size_t index{};
	bool raw_name_safe{};
	JsonMemberDecodeDirectFn<T> decode_direct_strict{};
	JsonMemberDecodeDirectFn<T> decode_direct_json5{};
	bool occupied{};
};

template<ParseMode Mode, class T, std::size_t I>
std::expected<void, JsonError> decode_member_direct_by_static_index(
	T &result,
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	auto const members = conflux::json::JsonMembers<T>::members();
	auto const &entry = get<I>(members);
	auto const &m = jm_member(entry);
	using M = std::remove_reference_t<decltype(result.*m.pointer)>;
	auto decoded = decode_next_object_value_into<Mode, M>(result.*m.pointer, r, opts, scratch);
	if (!decoded) {
		return std::unexpected(std::move(decoded).error());
	}
	auto cfn = jm_constraint(entry);
	if (cfn != nullptr) {
		if (auto cr = cfn(result.*m.pointer); !cr) {
			auto err = std::move(cr).error();
			err.member_name = std::string{m.name};
			return std::unexpected(std::move(err));
		}
	}
	return {};
}

template<class T, std::size_t I>
[[nodiscard]] JsonMemberLookupEntry<T> make_json_member_lookup_entry() {
	auto const members = conflux::json::JsonMembers<T>::members();
	auto const &entry = get<I>(members);
	auto const &m = jm_member(entry);
	return JsonMemberLookupEntry<T>{
		.name = m.name,
		.hash = json_member_name_hash(m.name),
		.index = I,
		.raw_name_safe = json_member_raw_name_fast_path_safe(m.name),
		.decode_direct_strict = &decode_member_direct_by_static_index<ParseMode::strict, T, I>,
		.decode_direct_json5 = &decode_member_direct_by_static_index<ParseMode::json5, T, I>,
		.occupied = true};
}

template<class T, std::size_t... Is>
[[nodiscard]] auto make_json_member_lookup_slots_impl(
	std::index_sequence<Is...>) {
	constexpr std::size_t member_count = sizeof...(Is);
	std::array<JsonMemberLookupEntry<T>, json_member_lookup_capacity(member_count)> slots{};
	auto insert = [&](JsonMemberLookupEntry<T> entry) {
		auto pos = static_cast<std::size_t>(entry.hash) & (slots.size() - 1);
		while (slots[pos].occupied) {
			pos = (pos + 1) & (slots.size() - 1);
		}
		slots[pos] = entry;
	};
	(insert(make_json_member_lookup_entry<T, Is>()), ...);
	return slots;
}

template<class T, std::size_t... Is>
[[nodiscard]] auto make_json_member_ordered_entries_impl(
	std::index_sequence<Is...>) {
	return std::array<JsonMemberLookupEntry<T>, sizeof...(Is)>{make_json_member_lookup_entry<T, Is>()...};
}

template<class T>
inline auto const json_member_ordered_entries_v = [] {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	return make_json_member_ordered_entries_impl<T>(std::make_index_sequence<member_count>{});
}();

template<class T>
[[nodiscard]] auto const &json_member_ordered_entries() noexcept {
	return json_member_ordered_entries_v<T>;
}

template<class T>
[[nodiscard]] auto const &json_member_lookup_slots() {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	static auto const slots = make_json_member_lookup_slots_impl<T>(std::make_index_sequence<member_count>{});
	return slots;
}

template<class T>
[[nodiscard]] JsonMemberLookupEntry<T> const *find_json_member_lookup_entry(
	std::string_view key) {
	auto const &slots = json_member_lookup_slots<T>();
	std::uint64_t const hash = json_member_name_hash(key);
	auto pos = static_cast<std::size_t>(hash) & (slots.size() - 1);
	for (std::size_t probe = 0; probe < slots.size(); ++probe) {
		auto const &slot = slots[pos];
		if (!slot.occupied) {
			return nullptr;
		}
		if (slot.hash == hash && slot.name == key) {
			return &slot;
		}
		pos = (pos + 1) & (slots.size() - 1);
	}
	return nullptr;
}

template<class T>
[[nodiscard]] bool is_json_member_name(
	std::string_view key) {
	constexpr auto members = conflux::json::JsonMembers<T>::members();
	using MembersTuple = std::remove_cvref_t<decltype(members)>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	if constexpr (member_count > kJsonMemberLinearLookupLimit) {
		return find_json_member_lookup_entry<T>(key) != nullptr;
	} else {
		bool found = false;
		apply([&](auto const &...ms) { ((found = found || key == jm_member(ms).name), ...); }, members);
		return found;
	}
}

template<ParseMode Mode, class PresenceBits, class DecodeValue>
std::expected<void, JsonError> decode_known_member_value(
	JsonReader &r,
	PresenceBits &presence,
	std::size_t idx,
	std::string_view name,
	DecodeValue &&decode_value) {
	bool const already_found = presence.test(idx);
	if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::reject) {
		return std::unexpected(duplicate_member_error(name));
	}
	if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::first_wins) {
		if (auto skipped = r.skip_next_object_value_impl<Mode>(); !skipped) {
			return std::unexpected(std::move(skipped).error());
		}
		return {};
	}
	if (!already_found) {
		presence.set(idx);
	}
	return decode_value();
}

template<ParseMode Mode, class Map>
[[nodiscard]] std::expected<void, JsonError> decode_map_from_reader_into(
	Map &out,
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Vt = typename Map::mapped_type;
	out.clear();
	if (auto reserved = reserve_map_from_remaining_object<Mode>(out, r); !reserved) {
		return std::unexpected(std::move(reserved).error());
	}
	auto const duplicate_policy = r.parse_options().duplicate_key;
	while (true) {
		auto key_token = r.next_object_key_token_impl<Mode>();
		if (!key_token) {
			return std::unexpected(std::move(key_token).error());
		}
		if (!*key_token) {
			return {};
		}
		auto key_res = string_from_token<std::string>(**key_token, scratch);
		if (!key_res) {
			return std::unexpected(std::move(key_res).error());
		}
		std::string key = std::move(*key_res);
		auto existing = out.find(key);
		bool const duplicate = existing != out.end();
		if (duplicate && duplicate_policy == DuplicateKeyPolicy::reject) {
			return std::unexpected(duplicate_member_error(key));
		}
		if (duplicate && duplicate_policy == DuplicateKeyPolicy::first_wins) {
			if (auto skipped = r.skip_next_object_value_impl<Mode>(); !skipped) {
				return std::unexpected(std::move(skipped).error());
			}
			continue;
		}
		if (duplicate) {
			if constexpr (std::default_initializable<Vt>) {
				Vt val{};
				auto decoded = decode_next_object_value_into<Mode, Vt>(val, r, opts, scratch);
				if (!decoded) {
					return std::unexpected(std::move(decoded).error());
				}
				if constexpr (std::assignable_from<Vt &, Vt>) {
					existing->second = std::move(val);
				} else {
					out.erase(existing);
					out.emplace(std::move(key), std::move(val));
				}
			} else {
				auto ve = r.next_object_value_event_impl<Mode>();
				if (!ve) {
					return std::unexpected(std::move(ve).error());
				}
				auto val = decode_from_event<Mode, Vt>(r, *ve, opts, scratch);
				if (!val) {
					return std::unexpected(std::move(val).error());
				}
				out.erase(existing);
				out.emplace(std::move(key), std::move(*val));
			}
			continue;
		}
		if constexpr (std::default_initializable<Vt> && requires(Map &m, std::string k) {
						  m.try_emplace(std::move(k));
					  }) {
			auto [inserted_it, _] = out.try_emplace(std::move(key));
			auto decoded = decode_next_object_value_into<Mode, Vt>(inserted_it->second, r, opts, scratch);
			if (!decoded) {
				out.erase(inserted_it);
				return std::unexpected(std::move(decoded).error());
			}
		} else {
			auto ve = r.next_object_value_event_impl<Mode>();
			if (!ve) {
				return std::unexpected(std::move(ve).error());
			}
			auto val = decode_from_event<Mode, Vt>(r, *ve, opts, scratch);
			if (!val) {
				return std::unexpected(std::move(val).error());
			}
			out.emplace(std::move(key), std::move(*val));
		}
	}
}

template<ParseMode Mode, class T>
std::expected<T, JsonError> decode_with_reader(
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch = nullptr) {
	auto ne = r.next_impl<Mode>();
	if (!ne) {
		return std::unexpected(std::move(ne).error());
	}
	if (!*ne) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::unexpected_eof,
				.message = "std::unexpected end of input"});
	}
	JsonDecodeScratch local_scratch;
	return decode_from_event<Mode, T>(r, **ne, opts, scratch != nullptr ? scratch : &local_scratch);
}

template<class T>
std::expected<T, JsonError> decode_with_reader(
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch = nullptr) {
	if (r.parse_options().mode == ParseMode::strict) {
		return decode_with_reader<ParseMode::strict, T>(r, opts, scratch);
	}
	return decode_with_reader<ParseMode::json5, T>(r, opts, scratch);
}

template<ParseMode Mode, class T>
std::expected<T, JsonError> decode_full_with_reader(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	auto value = decode_with_reader<Mode, T>(reader, opts);
	if (!value) {
		return std::unexpected(std::move(value).error());
	}
	auto next = reader.next_impl<Mode>();
	if (!next) {
		return std::unexpected(std::move(next).error());
	}
	if (*next) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source = JsonSourceLocation{.offset = reader.value_start_pos()},
				.message = "trailing JSON value after document root"});
	}
	return std::move(value);
}

template<class T>
std::expected<T, JsonError> decode_full_with_reader(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	if (reader.parse_options().mode == ParseMode::strict) {
		return decode_full_with_reader<ParseMode::strict, T>(reader, opts);
	}
	return decode_full_with_reader<ParseMode::json5, T>(reader, opts);
}

template<ParseMode Mode, class T>
std::expected<void, JsonError> decode_members_from_event_into(
	T &result,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	if (ev != Ev::begin_object) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::wrong_kind,
				.message = "std::expected object"});
	}
	auto const members = conflux::json::JsonMembers<T>::members();
	using MembersTuple = std::remove_cvref_t<decltype(members)>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	bool ok = true;
	JsonError first_err;
	JsonDecodeScratch local_scratch;
	JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;
	using PresenceBits = std::conditional_t<(member_count <= 64U), JsonInlinePresenceBits, JsonPresenceBits>;
	PresenceBits presence{decode_scratch.resource};
	presence.reset(member_count);
	std::size_t ordered_cursor = 0;

	while (ok) {
		std::string_view key_name{};
		bool matched = false;
		if constexpr (member_count != 0) {
			auto const &ordered_members = json_member_ordered_entries<T>();
			if (ordered_cursor < ordered_members.size()) {
				auto const &entry = ordered_members[ordered_cursor];
				auto key_match = r.next_object_key_match_impl<Mode>(entry.name, decode_scratch, entry.raw_name_safe);
				if (!key_match) {
					ok = false;
					first_err = std::move(key_match).error();
					break;
				}
				if (!key_match->has_key) {
					break;
				}
				key_name = key_match->key;
				if (key_match->matched) {
					matched = true;
					++ordered_cursor;
					auto decoded = decode_known_member_value<Mode>(r, presence, entry.index, entry.name, [&] {
						if constexpr (Mode == ParseMode::strict) {
							return entry.decode_direct_strict(result, r, opts, &decode_scratch);
						} else {
							return entry.decode_direct_json5(result, r, opts, &decode_scratch);
						}
					});
					if (!decoded) {
						ok = false;
						first_err = std::move(decoded).error();
					}
				}
			} else {
				auto key_view = r.next_object_key_view_impl<Mode>(decode_scratch);
				if (!key_view) {
					ok = false;
					first_err = std::move(key_view).error();
					break;
				}
				if (!*key_view) {
					break;
				}
				key_name = **key_view;
			}
		} else {
			auto key_view = r.next_object_key_view_impl<Mode>(decode_scratch);
			if (!key_view) {
				ok = false;
				first_err = std::move(key_view).error();
				break;
			}
			if (!*key_view) {
				break;
			}
			key_name = **key_view;
		}
		if (!matched) {
			if constexpr (member_count > kJsonMemberLinearLookupLimit) {
				if (auto const *entry = find_json_member_lookup_entry<T>(key_name); entry != nullptr) {
					matched = true;
					if (entry->index >= ordered_cursor) {
						ordered_cursor = entry->index + 1U;
					}
					auto decoded = decode_known_member_value<Mode>(r, presence, entry->index, entry->name, [&] {
						if constexpr (Mode == ParseMode::strict) {
							return entry->decode_direct_strict(result, r, opts, &decode_scratch);
						} else {
							return entry->decode_direct_json5(result, r, opts, &decode_scratch);
						}
					});
					if (!decoded) {
						ok = false;
						first_err = std::move(decoded).error();
					}
				}
			} else {
				apply(
					[&](auto const &...ms) {
						std::size_t idx = 0;
						(([&](auto const &entry) {
							 if (matched || !ok) {
								 ++idx;
								 return;
							 }
							 auto const &m = jm_member(entry);
							 if (key_name == m.name) {
								 matched = true;
								 if (idx >= ordered_cursor) {
									 ordered_cursor = idx + 1U;
								 }
								 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
								 auto decoded = decode_known_member_value<Mode>(r, presence, idx, m.name, [&] {
									 auto value_decoded = decode_next_object_value_into<Mode, M>(
										 result.*m.pointer,
										 r,
										 opts,
										 &decode_scratch);
									 if (!value_decoded) {
										 return std::expected<void, JsonError>{
											 std::unexpected(std::move(value_decoded).error())};
									 }
									 auto cfn = jm_constraint(entry);
									 if (cfn != nullptr) {
										 if (auto cr = cfn(result.*m.pointer); !cr) {
											 auto err = std::move(cr).error();
											 err.member_name = std::string{m.name};
											 return std::expected<void, JsonError>{std::unexpected(std::move(err))};
										 }
									 }
									 return std::expected<void, JsonError>{};
								 });
								 if (!decoded) {
									 ok = false;
									 first_err = std::move(decoded).error();
								 }
							 }
							 ++idx;
						 })(ms),
						 ...);
					},
					members);
			}
		}

		if (!matched && ok) {
			if (opts.unknown_members == UnknownMemberPolicy::reject) {
				ok = false;
				first_err = JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.member_name = std::string{key_name},
					.message = std::format("unknown member: {}", key_name)};
			} else {
				auto skipped = r.skip_next_object_value_impl<Mode>();
				if (!skipped) {
					ok = false;
					first_err = std::move(skipped).error();
				}
			}
		}
	}

	if (!ok) {
		return std::unexpected(std::move(first_err));
	}

	apply(
		[&](auto const &...ms) {
			std::size_t idx = 0;
			(([&](auto const &entry) {
				 if (!ok) {
					 ++idx;
					 return;
				 }
				 auto const &m = jm_member(entry);
				 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
				 bool const found = presence.test(idx);
				 if (!found && !is_optional<M>::value) {
					 ok = false;
					 first_err = JsonError{
						 .stage = JsonStage::decode,
						 .code = JsonIssueCode::missing_member,
						 .member_name = std::string{m.name},
						 .message = std::format("missing member: {}", m.name)};
				 }
				 ++idx;
			 })(ms),
			 ...);
		},
		members);

	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	return {};
}

template<ParseMode Mode, class T>
std::expected<T, JsonError> decode_from_event(
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;

	if constexpr (std::same_as<T, bool>) {
		if (ev != Ev::bool_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected bool"});
		}
		return r.bool_val();
	} else if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>) {
		if (ev != Ev::number_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected number"});
		}
		return r.number_val().template get_as<T>();
	} else if constexpr (is_basic_string_of_char_v<T>) {
		if (ev != Ev::string_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected string"});
		}
		return string_from_token<T>(r.string_token(), scratch);
	} else if constexpr (std::same_as<T, std::string_view>) {
		static_assert(
			!std::same_as<T, std::string_view>,
			"decode<string_view>(JsonReader&) is deleted; use std::string");
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (ev == Ev::null_value) {
			return T{};
		}
		auto v = decode_from_event<Mode, Inner>(r, ev, opts, scratch);
		if (!v) {
			return std::unexpected(std::move(v).error());
		}
		return T{std::move(*v)};
	} else if constexpr (is_nullable_type<T>::value) {
		if (ev == Ev::null_value) {
			return T{};
		}
		using Inner = nullable_inner_t<T>;
		auto v = decode_from_event<Mode, Inner>(r, ev, opts, scratch);
		if (!v) {
			return std::unexpected(std::move(v).error());
		}
		return T{std::move(*v)};
	} else if constexpr (is_vector_of_v<T>) {
		using E = typename T::value_type;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		T result;
		if constexpr (std::floating_point<E>) {
			if (auto decoded = r.decode_floating_array_into<Mode>(result); !decoded) {
				return std::unexpected(std::move(decoded).error());
			}
			return result;
		} else if constexpr (std::integral<E> && !std::same_as<E, bool>) {
			if (auto decoded = r.decode_integral_array_into<Mode>(result); !decoded) {
				return std::unexpected(std::move(decoded).error());
			}
			return result;
		} else if constexpr (is_basic_string_of_char_v<E>) {
			JsonDecodeScratch local_scratch;
			JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;
			if (auto decoded = r.decode_string_array_into<Mode>(result, decode_scratch); !decoded) {
				return std::unexpected(std::move(decoded).error());
			}
			return result;
		} else if constexpr (is_fixed_numeric_array_v<E>) {
			if (auto decoded = r.decode_fixed_numeric_array_vector_into<Mode>(result); !decoded) {
				return std::unexpected(std::move(decoded).error());
			}
			return result;
		}
		if (auto reserved = reserve_vector_from_remaining_array<Mode>(result, r); !reserved) {
			return std::unexpected(std::move(reserved).error());
		}
		while (true) {
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return result;
			}
			if constexpr (std::default_initializable<E>) {
				auto &slot = result.emplace_back();
				auto decoded = decode_into<Mode, E>(slot, r, **ne, opts, scratch);
				if (!decoded) {
					return std::unexpected(std::move(decoded).error());
				}
			} else {
				auto elem = decode_from_event<Mode, E>(r, **ne, opts, scratch);
				if (!elem) {
					return std::unexpected(std::move(elem).error());
				}
				result.push_back(std::move(*elem));
			}
		}
	} else if constexpr (is_std_array_v<T>) {
		using E = typename T::value_type;
		constexpr std::size_t N = std::tuple_size_v<T>;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		T result;
		if constexpr (is_fixed_numeric_array_v<T>) {
			if (auto decoded = r.decode_fixed_numeric_array_into<Mode>(result); !decoded) {
				return std::unexpected(std::move(decoded).error());
			}
			return result;
		}
		for (std::size_t i = 0; i < N; ++i) {
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = std::format("std::expected array of length {}", N)});
			}
			auto elem = decode_from_event<Mode, E>(r, **ne, opts, scratch);
			if (!elem) {
				return std::unexpected(std::move(elem).error());
			}
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			result[i] = std::move(*elem);
		}
		auto ne = r.next_impl<Mode>();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = std::format("std::expected array of length {}", N)});
		}
		return result;
	} else if constexpr (is_pair_v<T>) {
		using FA = typename T::first_type;
		using FB = typename T::second_type;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		T result;
		{
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in pair"});
			}
			auto v = decode_from_event<Mode, FA>(r, **ne, opts, scratch);
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			result.first = std::move(*v);
		}
		{
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in pair"});
			}
			auto v = decode_from_event<Mode, FB>(r, **ne, opts, scratch);
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			result.second = std::move(*v);
		}
		{
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne || **ne != Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = "std::expected pair of length 2"});
			}
		}
		return result;
	} else if constexpr (is_tuple_of_v<T>) {
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		T result;
		bool ok = true;
		JsonError first_err;
		constexpr std::size_t N = std::tuple_size_v<T>;
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(([&]() {
				 if (!ok) {
					 return;
				 }
				 auto ne = r.next_impl<Mode>();
				 if (!ne) {
					 ok = false;
					 first_err = std::move(ne).error();
					 return;
				 }
				 if (!*ne) {
					 ok = false;
					 first_err = JsonError{
						 .stage = JsonStage::decode,
						 .code = JsonIssueCode::unexpected_eof,
						 .message = "EOF in tuple"};
					 return;
				 }
				 if (**ne == Ev::end_array) {
					 ok = false;
					 first_err = JsonError{
						 .stage = JsonStage::decode,
						 .code = JsonIssueCode::invalid_value,
						 .message = std::format("std::expected tuple of length {}", N)};
					 return;
				 }
				 using E = std::tuple_element_t<Is, T>;
				 auto v = decode_from_event<Mode, E>(r, **ne, opts, scratch);
				 if (!v) {
					 ok = false;
					 first_err = std::move(v).error();
					 return;
				 }
				 get<Is>(result) = std::move(*v);
			 })(),
			 ...);
		}(std::make_index_sequence<N>{});
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		auto ne = r.next_impl<Mode>();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = std::format("std::expected tuple of length {}", N)});
		}
		return result;
	} else if constexpr (is_map_type_v<T> || is_unordered_map_type_v<T>) {
		if (ev != Ev::begin_object) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected object"});
		}
		T result;
		if (auto decoded = decode_map_from_reader_into<Mode>(result, r, opts, scratch); !decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		return result;
	} else if constexpr (std::same_as<T, Document>) {
		std::size_t const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader<Mode>(r, ev); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		std::string_view const slice = r.input().substr(start, r.pos() - start);
		return conflux::json::parse(slice, r.parse_options());
	} else if constexpr (has_members_spec<T>::value) {
		T result{};
		if (auto ok = decode_members_from_event_into<Mode, T>(result, r, ev, opts, scratch); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		return result;
	} else if constexpr (codec_decodes_reader_event_mode_with_options<Mode, T>) {
		return JsonCodec<T>::template decode<Mode>(r, ev, opts, scratch);
	} else if constexpr (codec_decodes_reader_event_with_options<T>) {
		return JsonCodec<T>::decode(r, ev, opts, scratch);
	} else if constexpr (has_codec_spec<T>::value) {
		// Generic fallback: re-parse as DOM and delegate to JsonCodec<T>::decode.
		// Used by any type with a custom JsonCodec that has no dedicated streaming branch.
		std::size_t const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader<Mode>(r, ev); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		std::string_view const slice = r.input().substr(start, r.pos() - start);
		auto doc = conflux::json::parse(slice, r.parse_options());
		if (!doc) {
			return std::unexpected(std::move(doc).error());
		}
		return decode_codec<T>(doc->root(), opts);
	} else {
		static_assert(!std::same_as<T, T>, "No JsonReader support for type T");
	}
}

template<ParseMode Mode, class T>
std::expected<void, JsonError> decode_into(
	T &out,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	if constexpr (std::same_as<T, bool>) {
		if (ev != Ev::bool_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected bool"});
		}
		out = r.bool_val();
		return {};
	} else if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>) {
		if (ev != Ev::number_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected number"});
		}
		auto value = r.number_val().template get_as<T>();
		if (!value) {
			return std::unexpected(std::move(value).error());
		}
		out = *value;
		return {};
	} else if constexpr (is_basic_string_of_char_v<T>) {
		if (ev != Ev::string_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected string"});
		}
		return decode_string_into(out, r.string_token(), scratch);
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (ev == Ev::null_value) {
			out.reset();
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			out.emplace();
			auto ok = decode_into<Mode, Inner>(*out, r, ev, opts, scratch);
			if (!ok) {
				out.reset();
				return std::unexpected(std::move(ok).error());
			}
			return {};
		} else {
			auto decoded = decode_from_event<Mode, Inner>(r, ev, opts, scratch);
			if (!decoded) {
				out.reset();
				return std::unexpected(std::move(decoded).error());
			}
			out.emplace(std::move(*decoded));
			return {};
		}
	} else if constexpr (is_nullable_type<T>::value) {
		using Inner = nullable_inner_t<T>;
		if (ev == Ev::null_value) {
			out = T{};
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			Inner value{};
			auto ok = decode_into<Mode, Inner>(value, r, ev, opts, scratch);
			if (!ok) {
				out = T{};
				return std::unexpected(std::move(ok).error());
			}
			out = T{std::move(value)};
			return {};
		} else {
			auto decoded = decode_from_event<Mode, Inner>(r, ev, opts, scratch);
			if (!decoded) {
				out = T{};
				return std::unexpected(std::move(decoded).error());
			}
			out = T{std::move(*decoded)};
			return {};
		}
	} else if constexpr (is_vector_of_v<T>) {
		using E = typename T::value_type;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		out.clear();
		if constexpr (std::floating_point<E>) {
			return r.decode_floating_array_into<Mode>(out);
		} else if constexpr (std::integral<E> && !std::same_as<E, bool>) {
			return r.decode_integral_array_into<Mode>(out);
		} else if constexpr (is_basic_string_of_char_v<E>) {
			JsonDecodeScratch local_scratch;
			JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;
			return r.decode_string_array_into<Mode>(out, decode_scratch);
		} else if constexpr (is_fixed_numeric_array_v<E>) {
			return r.decode_fixed_numeric_array_vector_into<Mode>(out);
		}
		if (auto reserved = reserve_vector_from_remaining_array<Mode>(out, r); !reserved) {
			return std::unexpected(std::move(reserved).error());
		}
		while (true) {
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return {};
			}
			if constexpr (std::default_initializable<E>) {
				auto &slot = out.emplace_back();
				auto decoded = decode_into<Mode, E>(slot, r, **ne, opts, scratch);
				if (!decoded) {
					return std::unexpected(std::move(decoded).error());
				}
			} else {
				auto decoded = decode_from_event<Mode, E>(r, **ne, opts, scratch);
				if (!decoded) {
					return std::unexpected(std::move(decoded).error());
				}
				out.push_back(std::move(*decoded));
			}
		}
	} else if constexpr (is_std_array_v<T>) {
		using E = typename T::value_type;
		constexpr std::size_t N = std::tuple_size_v<T>;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		if constexpr (is_fixed_numeric_array_v<T>) {
			return r.decode_fixed_numeric_array_into<Mode>(out);
		}
		for (std::size_t i = 0; i < N; ++i) {
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = std::format("std::expected array of length {}", N)});
			}
			auto decoded = decode_into<Mode, E>(out[i], r, **ne, opts, scratch);
			if (!decoded) {
				return std::unexpected(std::move(decoded).error());
			}
		}
		auto ne = r.next_impl<Mode>();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = std::format("std::expected array of length {}", N)});
		}
		return {};
	} else if constexpr (is_pair_v<T>) {
		using FA = typename T::first_type;
		using FB = typename T::second_type;
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		{
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne || **ne == Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = "std::expected pair of length 2"});
			}
			auto decoded = decode_into<Mode, FA>(out.first, r, **ne, opts, scratch);
			if (!decoded) {
				return std::unexpected(std::move(decoded).error());
			}
		}
		{
			auto ne = r.next_impl<Mode>();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne || **ne == Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = "std::expected pair of length 2"});
			}
			auto decoded = decode_into<Mode, FB>(out.second, r, **ne, opts, scratch);
			if (!decoded) {
				return std::unexpected(std::move(decoded).error());
			}
		}
		auto ne = r.next_impl<Mode>();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = "std::expected pair of length 2"});
		}
		return {};
	} else if constexpr (is_tuple_of_v<T>) {
		if (ev != Ev::begin_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected array"});
		}
		bool ok = true;
		JsonError first_err;
		constexpr std::size_t N = std::tuple_size_v<T>;
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(([&]() {
				 if (!ok) {
					 return;
				 }
				 auto ne = r.next_impl<Mode>();
				 if (!ne) {
					 ok = false;
					 first_err = std::move(ne).error();
					 return;
				 }
				 if (!*ne || **ne == Ev::end_array) {
					 ok = false;
					 first_err = JsonError{
						 .stage = JsonStage::decode,
						 .code = JsonIssueCode::invalid_value,
						 .message = std::format("std::expected tuple of length {}", N)};
					 return;
				 }
				 using E = std::tuple_element_t<Is, T>;
				 auto decoded = decode_into<Mode, E>(get<Is>(out), r, **ne, opts, scratch);
				 if (!decoded) {
					 ok = false;
					 first_err = std::move(decoded).error();
				 }
			 })(),
			 ...);
		}(std::make_index_sequence<N>{});
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		auto ne = r.next_impl<Mode>();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = std::format("std::expected tuple of length {}", N)});
		}
		return {};
	} else if constexpr (is_map_type_v<T> || is_unordered_map_type_v<T>) {
		if (ev != Ev::begin_object) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected object"});
		}
		return decode_map_from_reader_into<Mode>(out, r, opts, scratch);
	} else if constexpr (has_members_spec<T>::value) {
		return decode_members_from_event_into<Mode, T>(out, r, ev, opts, scratch);
	} else {
		auto decoded = decode_from_event<Mode, T>(r, ev, opts, scratch);
		if (!decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		out = std::move(*decoded);
		return {};
	}
}

// ---------------------------------------------------------------------------
// Fast-path direct decoder
//
// A cursor-based strict-JSON decoder used by decode_borrowed/decode_full when
// the input is a complete in-memory document parsed in strict mode with
// default-or-explicit limits. It avoids the JsonReader event machinery, fat
// JsonError returns on the hot path, and per-byte line/column tracking.
//
// Error policy: the fast path never produces a JsonError. On any malformed,
// limit-violating, policy-violating, or unsupported input it bails out, and
// the caller re-runs the existing JsonReader-based decoder which produces
// byte-identical diagnostics. Failed decodes pay double parse cost; valid
// documents (the overwhelmingly common case) pay none of the diagnostic cost.
// ---------------------------------------------------------------------------

namespace fastpath {

// Fast-path result: success, "bail to slow path", or an authoritative error.
//
// `error` is only used for diagnostics that carry no positional information
// (no line/column/path) and are therefore byte-identical to what the slow
// path would produce: duplicate members, unknown members under the reject
// policy, and missing required members. Everything else bails.
enum class FpStatus : std::uint8_t {
	ok,
	bail,
	error,
};

// Filled when FpStatus::error is returned; converted to JsonError once at the
// document entry point.
struct FpError {
	JsonIssueCode code{};
	std::string_view member_name{};
};

struct FpCursor {
	char const *p;
	char const *end;
	std::uint32_t depth{0};
	FpError error{}; // valid only when a call returned FpStatus::error

	[[nodiscard]] [[gnu::always_inline]] inline bool at_end() const noexcept { return p >= end; }
	[[nodiscard]] [[gnu::always_inline]] inline std::size_t remaining() const noexcept {
		return static_cast<std::size_t>(end - p);
	}

	[[gnu::always_inline]] inline void skip_ws() noexcept {
		// Fast single-branch check for the no-whitespace case.
		if (p < end && static_cast<unsigned char>(*p) > 0x20U) {
			return;
		}
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
			++p;
		}
	}
};

// SWAR digit-run scan reused from the reader (operates on raw pointers).
[[nodiscard]] inline std::size_t fp_scan_digits(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	if constexpr (std::endian::native == std::endian::little) {
		constexpr std::uint64_t kLow = 0x3030303030303030ULL;
		constexpr std::uint64_t kHigh = 0x3939393939393939ULL;
		constexpr std::uint64_t kMsb = 0x8080808080808080ULL;
		while (i + sizeof(std::uint64_t) <= n) {
			std::uint64_t word{};
			std::memcpy(&word, p + i, sizeof(word));
			std::uint64_t const below = (word - kLow) & ~word & kMsb;
			std::uint64_t const above = ((word + (0x7f7f7f7f7f7f7f7fULL - kHigh)) | word) & kMsb;
			std::uint64_t const bad = below | above;
			if (bad != 0U) {
				return i + static_cast<std::size_t>(__builtin_ctzll(bad) >> 3U);
			}
			i += sizeof(std::uint64_t);
		}
	}
	while (i < n && p[i] >= '0' && p[i] <= '9') {
		++i;
	}
	return i;
}

// Single-pass signed/unsigned integer parse. Fuses validation and value
// accumulation; rejects leading zeros, fraction/exponent forms, and overflow
// by bailing (slow path classifies the precise error).
template<class T>
	requires(std::integral<T> && !std::same_as<T, bool>)
[[nodiscard]] inline FpStatus fp_parse_integer(
	FpCursor &c,
	T &out) noexcept {
	char const *p = c.p;
	char const *const end = c.end;
	bool neg = false;
	if (p < end && *p == '-') {
		if constexpr (std::unsigned_integral<T>) {
			return FpStatus::bail;
		}
		neg = true;
		++p;
	}
	if (p >= end || *p < '0' || *p > '9') {
		return FpStatus::bail;
	}
	// Leading zero: only valid if the number is exactly "0".
	if (*p == '0') {
		if (p + 1 < end && p[1] >= '0' && p[1] <= '9') {
			return FpStatus::bail;
		}
		// "0." / "0e" are non-integer forms -> bail to slow path for the
		// proper invalid_number diagnostic.
		if (p + 1 < end && (p[1] == '.' || p[1] == 'e' || p[1] == 'E')) {
			return FpStatus::bail;
		}
		out = T{0};
		c.p = p + 1;
		return FpStatus::ok;
	}
	std::uint64_t limit{};
	if constexpr (std::signed_integral<T>) {
		limit = neg ? static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + std::uint64_t{1} :
					  static_cast<std::uint64_t>(std::numeric_limits<T>::max());
	} else {
		limit = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
	}
	std::uint64_t mag = 0;
	std::size_t ndig = 0;
	std::size_t const rem = static_cast<std::size_t>(end - p);
	while (ndig < rem && ndig < 18U && p[ndig] >= '0' && p[ndig] <= '9') {
		mag = mag * 10U + static_cast<std::uint64_t>(p[ndig] - '0');
		++ndig;
	}
	if (ndig < rem && p[ndig] >= '0' && p[ndig] <= '9') {
		do {
			std::uint64_t const d = static_cast<std::uint64_t>(p[ndig] - '0');
			if (mag > (limit - d) / 10U) {
				return FpStatus::bail;
			}
			mag = mag * 10U + d;
			++ndig;
		} while (ndig < rem && p[ndig] >= '0' && p[ndig] <= '9');
	} else if (mag > limit) {
		return FpStatus::bail;
	}
	p += ndig;
	// Integer target cannot accept fraction/exponent forms.
	if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) {
		return FpStatus::bail;
	}
	if constexpr (std::signed_integral<T>) {
		if (neg) {
			if (mag == static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + std::uint64_t{1}) {
				out = std::numeric_limits<T>::min();
			} else {
				out = static_cast<T>(-static_cast<T>(mag));
			}
			c.p = p;
			return FpStatus::ok;
		}
	}
	out = static_cast<T>(mag);
	c.p = p;
	return FpStatus::ok;
}

// Powers of ten exactly representable as doubles (10^0 .. 10^22).
inline constexpr std::array<double, 23> kFpPow10{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
												 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

// Single-pass floating-point parse. Locates and validates the lexeme in one
// scan, accumulating the mantissa as it goes. For values where Clinger's fast
// path applies (mantissa <= 2^53, decimal exponent within +/-22), the result
// of double(mantissa) * / 10^e is exactly correctly rounded and from_chars is
// skipped entirely. Everything else falls back to from_chars on the located
// lexeme.
template<class T>
	requires std::floating_point<T>
[[nodiscard]] [[gnu::always_inline]] inline FpStatus fp_parse_floating(
	FpCursor &c,
	T &out) noexcept {
	char const *p = c.p;
	char const *const end = c.end;
	char const *const start = p;
	bool const neg = p < end && *p == '-';
	if (neg) {
		++p;
	}
	if (p >= end || *p < '0' || *p > '9') {
		return FpStatus::bail;
	}
	if (std::same_as<T, double> && *p == '0' && p + 2 < end && p[1] == '.') {
		char const *q = p + 2;
		std::uint64_t mant = 0;
		std::size_t digits = 0;
		while (q < end && *q >= '0' && *q <= '9' && digits < 17U) {
			mant = mant * 10U + static_cast<std::uint64_t>(*q - '0');
			++q;
			++digits;
		}
		if (digits == 0U) {
			return FpStatus::bail;
		}
		bool const too_many_digits = q < end && *q >= '0' && *q <= '9';
		bool const has_exponent = q < end && (*q == 'e' || *q == 'E');
		if (!too_many_digits && !has_exponent && mant <= (std::uint64_t{1} << 53U)) {
			double v = static_cast<double>(mant) / kFpPow10[digits];
			out = static_cast<T>(neg ? -v : v);
			c.p = q;
			return FpStatus::ok;
		}
	}
	bool const starts_zero = *p == '0';

	std::uint64_t mant = 0;
	std::size_t total_digits = 0;
	bool exact = true;
	char const *const int_start = p;
	if (p + 1 < end && p[1] == '.') {
		mant = static_cast<std::uint64_t>(*p - '0');
		total_digits = 1;
		++p;
	} else {
		char const *const int_accum_end = p + std::min<std::size_t>(17U, static_cast<std::size_t>(end - p));
		while (p < int_accum_end && *p >= '0' && *p <= '9') {
			mant = mant * 10U + static_cast<std::uint64_t>(*p - '0');
			++total_digits;
			++p;
		}
		if (p < end && *p >= '0' && *p <= '9') {
			exact = false;
			p += fp_scan_digits(p, static_cast<std::size_t>(end - p));
		}
	}
	std::size_t const int_len = static_cast<std::size_t>(p - int_start);
	if (starts_zero && int_len > 1) {
		return FpStatus::bail;
	}
	if (std::same_as<T, double>
		&& (p >= end || (*p != '.' && *p != 'e' && *p != 'E'))
		&& exact
		&& total_digits <= 17U
		&& mant <= (std::uint64_t{1} << 53U)) {
		out = static_cast<T>(neg ? -static_cast<double>(mant) : static_cast<double>(mant));
		c.p = p;
		return FpStatus::ok;
	}

	std::size_t frac_len = 0;
	if (p < end && *p == '.') {
		++p;
		if (p >= end || *p < '0' || *p > '9') {
			return FpStatus::bail;
		}
		char const *const frac_start = p;
		char const *const frac_accum_end =
			p + std::min<std::size_t>(17U - total_digits, static_cast<std::size_t>(end - p));
		while (p < frac_accum_end && *p >= '0' && *p <= '9') {
			mant = mant * 10U + static_cast<std::uint64_t>(*p - '0');
			++total_digits;
			++p;
		}
		if (p < end && *p >= '0' && *p <= '9') {
			exact = false;
			p += fp_scan_digits(p, static_cast<std::size_t>(end - p));
		}
		frac_len = static_cast<std::size_t>(p - frac_start);
	}

	std::int64_t exp_val = 0;
	bool exp_overlong = false;
	if (p < end && (*p == 'e' || *p == 'E')) [[unlikely]] {
		++p;
		bool exp_neg = false;
		if (p < end && (*p == '+' || *p == '-')) {
			exp_neg = *p == '-';
			++p;
		}
		char const *const exp_start = p;
		if (p >= end || *p < '0' || *p > '9') {
			return FpStatus::bail;
		}
		do {
			if (static_cast<std::size_t>(p - exp_start) < 4U) {
				exp_val = exp_val * 10 + (*p - '0');
			} else {
				exp_overlong = true;
			}
			++p;
		} while (p < end && *p >= '0' && *p <= '9');
		if (!exp_overlong) {
			if (exp_neg) {
				exp_val = -exp_val;
			}
		}
	}

	if (static_cast<std::size_t>(p - start) > kMaxNumberLexemeLen) {
		return FpStatus::bail;
	}

	// Clinger fast path (exact only when the target is double).
	std::int64_t const eff_exp = exp_val - static_cast<std::int64_t>(frac_len);
	if (std::same_as<T, double> && !exp_overlong && exact && total_digits <= 17U && eff_exp >= -22 && eff_exp <= 22)
		[[likely]] {
		if (mant <= (std::uint64_t{1} << 53U)) {
			double v = static_cast<double>(mant);
			if (eff_exp == 0) {
				out = static_cast<T>(neg ? -v : v);
				c.p = p;
				return FpStatus::ok;
			}
			if (eff_exp < 0) {
				v /= kFpPow10[static_cast<std::size_t>(-eff_exp)];
			} else {
				v *= kFpPow10[static_cast<std::size_t>(eff_exp)];
			}
			out = static_cast<T>(neg ? -v : v);
			c.p = p;
			return FpStatus::ok;
		}
	}

	// from_chars fallback for long mantissas / extreme exponents.
	T value{};
	auto const [ptr, ec] = std::from_chars(start, p, value, std::chars_format::general);
	if (ec != std::errc{} || ptr != p || !std::isfinite(value)) {
		return FpStatus::bail;
	}
	out = value;
	c.p = p;
	return FpStatus::ok;
}

// String body scan. Returns ok and sets [body_begin, body_len) for strings
// without escapes/UTF-8-validation needs; bails on escapes, control chars,
// or non-ASCII so the slow path handles full validation and unescaping.
//
// Escaped strings are handled by fp_parse_string_owned below, which decodes
// simple escapes inline and bails only on \uXXXX and UTF-8 validation needs.
struct FpStringView {
	char const *data;
	std::size_t size;
};

[[nodiscard]] inline std::size_t fp_scan_str_until_special(
	char const *p,
	std::size_t n) noexcept {
	constexpr std::size_t kScalarLimit = 32U;
	if (n <= kScalarLimit) {
		for (std::size_t i = 0; i < n; ++i) {
			unsigned char const ch = static_cast<unsigned char>(p[i]);
			if (ch < 0x20U || ch == '"' || ch == '\\' || ch >= 0x80U) {
				return i;
			}
		}
		return n;
	}
	return detail::simd::scan_str_until_special(p, n);
}

[[nodiscard]] inline FpStatus fp_scan_plain_string(
	FpCursor &c,
	FpStringView &out,
	std::size_t max_string) noexcept {
	// Caller has consumed the opening quote.
	char const *p = c.p;
	std::size_t const n = c.remaining();
	std::size_t const skip = fp_scan_str_until_special(p, n);
	if (skip >= n || p[skip] != '"' || skip > max_string) {
		return FpStatus::bail;
	}
	out = FpStringView{.data = p, .size = skip};
	c.p = p + skip + 1;
	return FpStatus::ok;
}

[[nodiscard]] inline bool fp_match_plain_key(
	FpCursor &c,
	std::string_view expected,
	std::size_t max_string) noexcept {
	if (expected.size() > max_string || c.remaining() <= expected.size()) {
		return false;
	}
	if (std::memcmp(c.p, expected.data(), expected.size()) != 0) {
		return false;
	}
	if (c.p[expected.size()] != '"') {
		return false;
	}
	c.p += expected.size() + 1U;
	return true;
}

[[nodiscard]] inline std::uint32_t fp_hex_digit(
	char const ch) noexcept {
	if (ch >= '0' && ch <= '9') {
		return static_cast<std::uint32_t>(ch - '0');
	}
	if (ch >= 'a' && ch <= 'f') {
		return static_cast<std::uint32_t>(ch - 'a') + 10U;
	}
	if (ch >= 'A' && ch <= 'F') {
		return static_cast<std::uint32_t>(ch - 'A') + 10U;
	}
	return 16U;
}

[[nodiscard]] inline std::optional<std::uint32_t> fp_hex4(
	char const *p) noexcept {
	std::uint32_t v = 0;
	for (std::size_t i = 0; i < 4; ++i) {
		std::uint32_t const d = fp_hex_digit(p[i]);
		if (d >= 16U) {
			return std::nullopt;
		}
		v = (v << 4U) | d;
	}
	return v;
}

[[nodiscard]] inline std::optional<char> fp_hex2_byte(
	char const *p) noexcept {
	std::uint32_t const hi = fp_hex_digit(p[0]);
	std::uint32_t const lo = fp_hex_digit(p[1]);
	if ((hi | lo) >= 16U) {
		return std::nullopt;
	}
	return static_cast<char>((hi << 4U) | lo);
}

// Owned-string decode with inline escape handling (including \uXXXX and
// surrogate pairs). Bails on non-ASCII bytes (UTF-8 validation stays in the
// slow path) and on invalid escapes.
template<class String>
[[nodiscard]] inline FpStatus fp_parse_string_owned(
	FpCursor &c,
	String &out,
	std::size_t max_string) noexcept {
	char const *const body_start = c.p;
	char const *p = c.p;
	char const *const end = c.end;
	out.clear();
	for (;;) {
		std::size_t const n = static_cast<std::size_t>(end - p);
		std::size_t run = 0;
		// Escape-dense strings: skip the SIMD call when the next byte is
		// already special.
		if (n != 0
			&& static_cast<unsigned char>(*p) >= 0x20U
			&& *p != '"'
			&& *p != '\\'
			&& static_cast<unsigned char>(*p) < 0x80U) {
			run = fp_scan_str_until_special(p, n);
		}
		if (run >= n) {
			return FpStatus::bail;
		}
		char const special = p[run];
		if (special == '"') {
			out.append(p, run);
			// Reader semantics: max_string_size bounds the raw body length;
			// decode additionally bounds the decoded length.
			if (out.size() > max_string || static_cast<std::size_t>(p + run - body_start) > max_string) {
				return FpStatus::bail;
			}
			c.p = p + run + 1;
			return FpStatus::ok;
		}
		if (special != '\\') {
			// Control char or non-ASCII: slow path validates and reports.
			return FpStatus::bail;
		}
		out.append(p, run);
		p += run + 1; // consume backslash
		if (p >= end) {
			return FpStatus::bail;
		}
		char decoded{};
		bool simple = true;
		switch (*p) {
		case '"' : decoded = '"'; break;
		case '\\': decoded = '\\'; break;
		case '/' : decoded = '/'; break;
		case 'b' : decoded = '\b'; break;
		case 'f' : decoded = '\f'; break;
		case 'n' : decoded = '\n'; break;
		case 'r' : decoded = '\r'; break;
		case 't' : decoded = '\t'; break;
		case 'u':
			{
				simple = false;
				++p;
				if (p + 4 > end) {
					return FpStatus::bail;
				}
				if (p[0] == '0' && p[1] == '0') {
					auto const byte = fp_hex2_byte(p + 2);
					if (!byte) {
						return FpStatus::bail;
					}
					out.push_back(*byte);
					p += 4;
					break;
				}
				auto cp_opt = fp_hex4(p);
				if (!cp_opt) {
					return FpStatus::bail;
				}
				std::uint32_t cp = *cp_opt;
				p += 4;
				if (cp >= 0xD800U && cp <= 0xDBFFU) {
					// High surrogate: a \uXXXX low surrogate must follow.
					if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
						return FpStatus::bail;
					}
					auto lo_opt = fp_hex4(p + 2);
					if (!lo_opt || *lo_opt < 0xDC00U || *lo_opt > 0xDFFFU) {
						return FpStatus::bail;
					}
					cp = 0x10000U + ((cp - 0xD800U) << 10U) + (*lo_opt - 0xDC00U);
					p += 6;
				} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
					return FpStatus::bail;
				}
				// Append UTF-8.
				if (cp < 0x80U) {
					out.push_back(static_cast<char>(cp));
				} else if (cp < 0x800U) {
					char const buf[2]{static_cast<char>(0xC0U | (cp >> 6U)), static_cast<char>(0x80U | (cp & 0x3FU))};
					out.append(buf, 2);
				} else if (cp < 0x10000U) {
					char const buf[3]{
						static_cast<char>(0xE0U | (cp >> 12U)),
						static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)),
						static_cast<char>(0x80U | (cp & 0x3FU))};
					out.append(buf, 3);
				} else {
					char const buf[4]{
						static_cast<char>(0xF0U | (cp >> 18U)),
						static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)),
						static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)),
						static_cast<char>(0x80U | (cp & 0x3FU))};
					out.append(buf, 4);
				}
				break;
			}
		default: return FpStatus::bail;
		}
		if (simple) {
			out.push_back(decoded);
			++p;
		}
		if (out.size() > max_string) {
			return FpStatus::bail;
		}
	}
}

// Resolved limits/policies for one fast-path decode.
struct FpLimits {
	std::size_t max_string;
	std::uint32_t max_depth;
	DuplicateKeyPolicy duplicate_key;
	UnknownMemberPolicy unknown_members;
};

// Skip any well-formed value (used for ignored unknown members). Bails on
// anything suspicious; depth-checked.
[[nodiscard]] inline FpStatus fp_skip_value(FpCursor &c,
											FpLimits const &lim) noexcept; // forward

[[nodiscard]] inline FpStatus fp_skip_string(
	FpCursor &c,
	std::size_t max_string) noexcept {
	// Caller consumed the opening quote. Skips past the closing quote while
	// validating escapes (the reader validates skipped values too, so the
	// fast path must not accept what the slow path would reject). Bails on
	// \uXXXX escapes and non-ASCII (full validation is the slow path's job).
	char const *p = c.p;
	char const *const end = c.end;
	char const *const body_start = p;
	for (;;) {
		std::size_t const n = static_cast<std::size_t>(end - p);
		std::size_t const run = detail::simd::scan_str_until_special(p, n);
		if (run >= n) {
			return FpStatus::bail;
		}
		char const special = p[run];
		p += run;
		if (special == '"') {
			// Reader limit semantics: raw body length (between quotes) is
			// what max_string_size bounds.
			if (static_cast<std::size_t>(p - body_start) > max_string) {
				return FpStatus::bail;
			}
			c.p = p + 1;
			return FpStatus::ok;
		}
		if (special == '\\') {
			if (p + 2 > end) {
				return FpStatus::bail;
			}
			char const esc = p[1];
			if (esc != '"'
				&& esc != '\\'
				&& esc != '/'
				&& esc != 'b'
				&& esc != 'f'
				&& esc != 'n'
				&& esc != 'r'
				&& esc != 't') {
				// \uXXXX or invalid escape: slow path validates and reports.
				return FpStatus::bail;
			}
			p += 2;
			continue;
		}
		// Control char or non-ASCII inside ignored string: bail so the slow
		// path runs full validation (it may legitimately reject).
		return FpStatus::bail;
	}
}

[[nodiscard]] inline FpStatus fp_skip_number(
	FpCursor &c) noexcept {
	char const *p = c.p;
	char const *const end = c.end;
	if (p < end && *p == '-') {
		++p;
	}
	if (p >= end || *p < '0' || *p > '9') {
		return FpStatus::bail;
	}
	bool const starts_zero = *p == '0';
	++p;
	if (starts_zero && p < end && *p >= '0' && *p <= '9') {
		return FpStatus::bail;
	}
	p += fp_scan_digits(p, static_cast<std::size_t>(end - p));
	if (p < end && *p == '.') {
		++p;
		if (p >= end || *p < '0' || *p > '9') {
			return FpStatus::bail;
		}
		p += fp_scan_digits(p, static_cast<std::size_t>(end - p));
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		++p;
		if (p < end && (*p == '+' || *p == '-')) {
			++p;
		}
		if (p >= end || *p < '0' || *p > '9') {
			return FpStatus::bail;
		}
		p += fp_scan_digits(p, static_cast<std::size_t>(end - p));
	}
	if (static_cast<std::size_t>(p - c.p) > kMaxNumberLexemeLen) {
		return FpStatus::bail;
	}
	c.p = p;
	return FpStatus::ok;
}

[[nodiscard]] inline FpStatus fp_skip_value(
	FpCursor &c,
	FpLimits const &lim) noexcept {
	c.skip_ws();
	if (c.at_end()) {
		return FpStatus::bail;
	}
	char const ch = *c.p;
	if (ch == '"') {
		++c.p;
		return fp_skip_string(c, lim.max_string);
	}
	if (ch == '-' || (ch >= '0' && ch <= '9')) {
		return fp_skip_number(c);
	}
	if (ch == 't') {
		if (c.remaining() < 4 || std::memcmp(c.p, "true", 4) != 0) {
			return FpStatus::bail;
		}
		c.p += 4;
		return FpStatus::ok;
	}
	if (ch == 'f') {
		if (c.remaining() < 5 || std::memcmp(c.p, "false", 5) != 0) {
			return FpStatus::bail;
		}
		c.p += 5;
		return FpStatus::ok;
	}
	if (ch == 'n') {
		if (c.remaining() < 4 || std::memcmp(c.p, "null", 4) != 0) {
			return FpStatus::bail;
		}
		c.p += 4;
		return FpStatus::ok;
	}
	if (ch == '{') {
		if (c.depth + 1 > lim.max_depth) {
			return FpStatus::bail;
		}
		++c.depth;
		++c.p;
		c.skip_ws();
		if (!c.at_end() && *c.p == '}') {
			++c.p;
			--c.depth;
			return FpStatus::ok;
		}
		for (;;) {
			c.skip_ws();
			if (c.at_end() || *c.p != '"') {
				return FpStatus::bail;
			}
			++c.p;
			if (fp_skip_string(c, lim.max_string) != FpStatus::ok) {
				return FpStatus::bail;
			}
			c.skip_ws();
			if (c.at_end() || *c.p != ':') {
				return FpStatus::bail;
			}
			++c.p;
			if (fp_skip_value(c, lim) != FpStatus::ok) {
				return FpStatus::bail;
			}
			c.skip_ws();
			if (c.at_end()) {
				return FpStatus::bail;
			}
			if (*c.p == ',') {
				++c.p;
				continue;
			}
			if (*c.p == '}') {
				++c.p;
				--c.depth;
				return FpStatus::ok;
			}
			return FpStatus::bail;
		}
	}
	if (ch == '[') {
		if (c.depth + 1 > lim.max_depth) {
			return FpStatus::bail;
		}
		++c.depth;
		++c.p;
		c.skip_ws();
		if (!c.at_end() && *c.p == ']') {
			++c.p;
			--c.depth;
			return FpStatus::ok;
		}
		for (;;) {
			if (fp_skip_value(c, lim) != FpStatus::ok) {
				return FpStatus::bail;
			}
			c.skip_ws();
			if (c.at_end()) {
				return FpStatus::bail;
			}
			if (*c.p == ',') {
				++c.p;
				continue;
			}
			if (*c.p == ']') {
				++c.p;
				--c.depth;
				return FpStatus::ok;
			}
			return FpStatus::bail;
		}
	}
	return FpStatus::bail;
}

// ---------------------------------------------------------------------------
// Supported-type trait: the fast path only handles a closed set of member
// types; everything else bails to the JsonReader-based decoder.
// ---------------------------------------------------------------------------

template<class T>
struct fp_supported : std::false_type {};
template<>
struct fp_supported<bool> : std::true_type {};
template<class T>
	requires(std::integral<T> && !std::same_as<T, bool>)
struct fp_supported<T> : std::true_type {};
template<class T>
	requires std::floating_point<T>
struct fp_supported<T> : std::true_type {};
template<class Traits, class Alloc>
struct fp_supported<std::basic_string<char, Traits, Alloc>> : std::true_type {};
template<class T>
struct fp_supported<std::optional<T>> : fp_supported<std::remove_cvref_t<T>> {};
template<class T, class Alloc>
struct fp_supported<std::vector<T, Alloc>> : fp_supported<std::remove_cvref_t<T>> {};
template<class T, std::size_t N>
struct fp_supported<std::array<T, N>>
	: std::bool_constant<(std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>> {};

template<class T>
consteval bool fp_members_all_supported() {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	if constexpr (member_count > 64U) {
		return false;
	} else {
		return []<std::size_t... Is>(std::index_sequence<Is...>) {
			return (
				fp_supported<
					std::remove_cvref_t<json_member_entry_value_type_t<std::tuple_element_t<Is, MembersTuple>>>>::value
				&& ...);
		}(std::make_index_sequence<member_count>{});
	}
}

template<class T>
	requires(has_members_spec<T>::value && !has_codec_spec<T>::value)
struct fp_supported<T> : std::bool_constant<fp_members_all_supported<T>()> {};

template<class T>
inline constexpr bool fp_supported_v = fp_supported<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// Fast key lookup: open-addressing table keyed on the first 8 bytes of the
// member name (as a little-endian uint64) plus its length. One unaligned
// load + multiply-shift hash + probe replaces the per-character FNV hash.
// ---------------------------------------------------------------------------

[[nodiscard]] consteval std::uint64_t fp_key_prefix64_ct(
	std::string_view name) noexcept {
	std::uint64_t v = 0;
	std::size_t const n = name.size() < 8U ? name.size() : 8U;
	for (std::size_t i = 0; i < n; ++i) {
		v |= static_cast<std::uint64_t>(static_cast<unsigned char>(name[i])) << (i * 8U);
	}
	return v;
}

// Runtime prefix load: reads 8 bytes when safely within the buffer, masks to
// the key length. Falls back to a byte loop near the end of the input.
[[nodiscard]] inline std::uint64_t fp_key_prefix64(
	char const *key,
	std::size_t len,
	char const *input_end) noexcept {
	std::uint64_t v = 0;
	if (key + 8 <= input_end) {
		std::memcpy(&v, key, 8);
		if (len < 8U) {
			v &= (std::uint64_t{1} << (len * 8U)) - 1U;
		}
	} else {
		std::size_t const n = len < 8U ? len : 8U;
		for (std::size_t i = 0; i < n; ++i) {
			v |= static_cast<std::uint64_t>(static_cast<unsigned char>(key[i])) << (i * 8U);
		}
	}
	return v;
}

struct FpKeySlot {
	std::uint64_t prefix{};
	std::uint32_t len{};
	std::uint32_t index{}; // member index + 1; 0 = empty
};

[[nodiscard]] consteval std::size_t fp_key_table_capacity(
	std::size_t member_count) noexcept {
	std::size_t cap = 4;
	while (cap < member_count * 2U) {
		cap <<= 1U;
	}
	return cap;
}

inline constexpr std::uint64_t kFpKeyHashMul = 0x9E3779B97F4A7C15ULL;

template<class T>
struct FpKeyTable {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	static constexpr std::size_t kMemberCount = std::tuple_size_v<MembersTuple>;
	static constexpr std::size_t kCapacity = fp_key_table_capacity(kMemberCount);
	static constexpr std::size_t kMask = kCapacity - 1U;
	std::array<FpKeySlot, kCapacity> slots{};
	bool valid{true}; // false if any two keys collide on (prefix, len) - then memcmp is needed

	consteval FpKeyTable() {
		auto const members = conflux::json::JsonMembers<T>::members();
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(insert(jm_member(get<Is>(members)).name, Is), ...);
		}(std::make_index_sequence<kMemberCount>{});
	}

	consteval void insert(
		std::string_view name,
		std::size_t index) {
		std::uint64_t const prefix = fp_key_prefix64_ct(name);
		// Two members sharing (prefix, len) cannot be distinguished without a
		// full memcmp; mark the table unusable (extremely rare: requires
		// names identical in the first 8 bytes and equal length > 8).
		for (auto const &s: slots) {
			if (s.index != 0 && s.prefix == prefix && s.len == name.size()) {
				valid = false;
			}
		}
		std::size_t pos = ((prefix * kFpKeyHashMul) >> 32U) & kMask;
		while (slots[pos].index != 0) {
			pos = (pos + 1U) & kMask;
		}
		slots[pos] = FpKeySlot{
			.prefix = prefix,
			.len = static_cast<std::uint32_t>(name.size()),
			.index = static_cast<std::uint32_t>(index + 1U)};
	}

	// Returns member index, or kMemberCount if not found.
	[[nodiscard]] std::size_t find(
		char const *key,
		std::size_t len,
		char const *input_end,
		std::array<std::string_view, kMemberCount> const &names) const noexcept {
		std::uint64_t const prefix = fp_key_prefix64(key, len, input_end);
		std::size_t pos = ((prefix * kFpKeyHashMul) >> 32U) & kMask;
		for (;;) {
			FpKeySlot const &s = slots[pos];
			if (s.index == 0) {
				return kMemberCount;
			}
			if (s.prefix == prefix && s.len == len) {
				std::size_t const idx = s.index - 1U;
				// Names longer than 8 bytes need a tail comparison.
				if (len <= 8U || std::memcmp(names[idx].data() + 8, key + 8, len - 8U) == 0) {
					return idx;
				}
			}
			pos = (pos + 1U) & kMask;
		}
	}
};

template<class T>
inline constexpr FpKeyTable<T> fp_key_table_v{};

template<class T>
inline constexpr auto fp_member_names_v = [] {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	auto const members = conflux::json::JsonMembers<T>::members();
	return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
		return std::array<std::string_view, member_count>{jm_member(get<Is>(members)).name...};
	}(std::make_index_sequence<member_count>{});
}();

// ---------------------------------------------------------------------------
// Value decode dispatch
// ---------------------------------------------------------------------------

template<class E>
[[nodiscard]] consteval std::size_t fp_vector_min_bytes_per_element() noexcept {
	if constexpr (is_fixed_numeric_array_v<E>) {
		return std::tuple_size_v<E> * 2U + 1U;
	} else if constexpr (std::integral<E> && !std::same_as<E, bool>) {
		return 2U;
	} else if constexpr (std::floating_point<E>) {
		return 4U;
	} else if constexpr (std::same_as<E, bool>) {
		return 5U;
	} else if constexpr (is_basic_string_of_char_v<E>) {
		return 4U;
	} else {
		return 8U;
	}
}

template<class E>
[[nodiscard]] inline std::size_t fp_vector_initial_reserve(
	std::size_t remaining) noexcept {
	constexpr std::size_t kMaxInitialReserveBytes = [] {
		if constexpr (is_fixed_numeric_array_v<E>) {
			return 512U;
		} else if constexpr (
			(std::integral<E> && !std::same_as<E, bool>) || std::floating_point<E> || std::same_as<E, bool>) {
			return 64U;
		} else if constexpr (is_basic_string_of_char_v<E>) {
			return 128U;
		} else {
			return 512U;
		}
	}();
	constexpr std::size_t kMaxInitialReserve =
		std::max<std::size_t>(4U, kMaxInitialReserveBytes / std::max<std::size_t>(sizeof(E), 1U));
	std::size_t const estimated = remaining / fp_vector_min_bytes_per_element<E>();
	return std::min<std::size_t>(kMaxInitialReserve, std::max<std::size_t>(4U, estimated));
}

template<class T>
[[nodiscard]] FpStatus fp_decode_struct(T &out, FpCursor &c, FpLimits const &lim) noexcept;

template<class T>
[[nodiscard]] [[gnu::always_inline]] inline FpStatus fp_decode_value(
	T &out,
	FpCursor &c,
	FpLimits const &lim) noexcept {
	c.skip_ws();
	if (c.at_end()) {
		return FpStatus::bail;
	}
	if constexpr (std::same_as<T, bool>) {
		if (*c.p == 't') {
			if (c.remaining() < 4 || std::memcmp(c.p, "true", 4) != 0) {
				return FpStatus::bail;
			}
			c.p += 4;
			out = true;
			return FpStatus::ok;
		}
		if (*c.p == 'f') {
			if (c.remaining() < 5 || std::memcmp(c.p, "false", 5) != 0) {
				return FpStatus::bail;
			}
			c.p += 5;
			out = false;
			return FpStatus::ok;
		}
		return FpStatus::bail;
	} else if constexpr (std::integral<T> && !std::same_as<T, bool>) {
		return fp_parse_integer<T>(c, out);
	} else if constexpr (std::floating_point<T>) {
		return fp_parse_floating<T>(c, out);
	} else if constexpr (is_basic_string_of_char_v<T>) {
		if (*c.p != '"') {
			return FpStatus::bail;
		}
		++c.p;
		return fp_parse_string_owned(c, out, lim.max_string);
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (*c.p == 'n') {
			if (c.remaining() < 4 || std::memcmp(c.p, "null", 4) != 0) {
				return FpStatus::bail;
			}
			c.p += 4;
			out.reset();
			return FpStatus::ok;
		}
		out.emplace();
		if (FpStatus const st = fp_decode_value<Inner>(*out, c, lim); st != FpStatus::ok) {
			out.reset();
			return st;
		}
		return FpStatus::ok;
	} else if constexpr (is_vector_of_v<T>) {
		using E = typename T::value_type;
		if (*c.p != '[') {
			return FpStatus::bail;
		}
		if (c.depth + 1 > lim.max_depth) {
			return FpStatus::bail;
		}
		++c.depth;
		++c.p;
		std::size_t string_reuse_index = 0;
		if constexpr (!is_basic_string_of_char_v<E>) {
			out.clear();
		}
		c.skip_ws();
		if (!c.at_end() && *c.p == ']') {
			++c.p;
			--c.depth;
			out.clear();
			return FpStatus::ok;
		}
		if (out.capacity() == 0) {
			out.reserve(fp_vector_initial_reserve<E>(c.remaining()));
		}
		for (;;) {
			if constexpr (
				(std::integral<E> && !std::same_as<E, bool>) || std::floating_point<E> || is_fixed_numeric_array_v<E>) {
				if (c.p >= c.end) {
					return FpStatus::bail;
				}
				if (static_cast<unsigned char>(*c.p) <= 0x20U) {
					c.skip_ws();
					if (c.p >= c.end) {
						return FpStatus::bail;
					}
				}
				E &slot = out.emplace_back();
				FpStatus st{};
				if constexpr (std::floating_point<E>) {
					st = fp_parse_floating<E>(c, slot);
				} else if constexpr (std::integral<E> && !std::same_as<E, bool>) {
					st = fp_parse_integer<E>(c, slot);
				} else {
					st = fp_decode_value<E>(slot, c, lim);
				}
				if (st != FpStatus::ok) {
					out.pop_back();
					return st;
				}
			} else {
				E *slot{};
				if constexpr (is_basic_string_of_char_v<E>) {
					if (string_reuse_index < out.size()) {
						slot = &out[string_reuse_index];
					} else {
						slot = &out.emplace_back();
					}
					++string_reuse_index;
				} else {
					slot = &out.emplace_back();
				}
				if (FpStatus const st = fp_decode_value<E>(*slot, c, lim); st != FpStatus::ok) {
					return st;
				}
			}
			if (c.at_end()) {
				return FpStatus::bail;
			}
			if (static_cast<unsigned char>(*c.p) <= 0x20U) {
				c.skip_ws();
				if (c.at_end()) {
					return FpStatus::bail;
				}
			}
			if (*c.p == ',') {
				++c.p;
				continue;
			}
			if (*c.p == ']') {
				++c.p;
				--c.depth;
				if constexpr (is_basic_string_of_char_v<E>) {
					out.resize(string_reuse_index);
				}
				return FpStatus::ok;
			}
			return FpStatus::bail;
		}
	} else if constexpr (is_fixed_numeric_array_v<T>) {
		using E = typename T::value_type;
		constexpr std::size_t N = std::tuple_size_v<T>;
		if (*c.p != '[') {
			return FpStatus::bail;
		}
		if (c.depth + 1 > lim.max_depth) {
			return FpStatus::bail;
		}
		++c.depth;
		++c.p;
		for (std::size_t i = 0; i < N; ++i) {
			if (c.p >= c.end) {
				return FpStatus::bail;
			}
			if (static_cast<unsigned char>(*c.p) <= 0x20U) {
				c.skip_ws();
				if (c.p >= c.end) {
					return FpStatus::bail;
				}
			}
			FpStatus st{};
			if constexpr (std::floating_point<E>) {
				st = fp_parse_floating<E>(c, out[i]);
			} else {
				st = fp_parse_integer<E>(c, out[i]);
			}
			if (st != FpStatus::ok) {
				return FpStatus::bail;
			}
			if (c.p >= c.end) {
				return FpStatus::bail;
			}
			if (static_cast<unsigned char>(*c.p) <= 0x20U) {
				c.skip_ws();
				if (c.p >= c.end) {
					return FpStatus::bail;
				}
			}
			if (i + 1U < N) {
				if (*c.p != ',') {
					return FpStatus::bail;
				}
				++c.p;
			}
		}
		if (c.p >= c.end || *c.p != ']') {
			return FpStatus::bail;
		}
		++c.p;
		--c.depth;
		return FpStatus::ok;
	} else if constexpr (has_members_spec<T>::value) {
		return fp_decode_struct<T>(out, c, lim);
	} else {
		return FpStatus::bail;
	}
}

// ---------------------------------------------------------------------------
// Struct (JsonMembers) decode with declaration-order cursor heuristic
// ---------------------------------------------------------------------------

template<class T, std::size_t I>
[[nodiscard]] [[gnu::always_inline]] inline FpStatus fp_decode_member_at(
	T &out,
	FpCursor &c,
	FpLimits const &lim) noexcept {
	auto const members = conflux::json::JsonMembers<T>::members();
	auto const &entry = get<I>(members);
	auto const &m = jm_member(entry);
	using M = std::remove_reference_t<decltype(out.*m.pointer)>;
	if (FpStatus const st = fp_decode_value<M>(out.*m.pointer, c, lim); st != FpStatus::ok) {
		return st;
	}
	auto cfn = jm_constraint(entry);
	if (cfn != nullptr) {
		if (auto cr = cfn(out.*m.pointer); !cr) {
			// Constraint violation: slow path re-runs and reports it.
			return FpStatus::bail;
		}
	}
	return FpStatus::ok;
}

template<class T, std::size_t I = 0>
[[nodiscard]] inline FpStatus fp_decode_member_by_index(
	std::size_t idx,
	T &out,
	FpCursor &c,
	FpLimits const &lim) noexcept {
	constexpr std::size_t N =
		std::tuple_size_v<std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>>;
	if constexpr (I >= N) {
		return FpStatus::bail;
	} else {
		if (idx == I) {
			return fp_decode_member_at<T, I>(out, c, lim);
		}
		return fp_decode_member_by_index<T, I + 1U>(idx, out, c, lim);
	}
}

template<class T>
struct FpMemberMeta {
	std::string_view name;
	FpStatus (*decode)(T &, FpCursor &, FpLimits const &) noexcept;
	bool required;
};

template<class T, std::size_t... Is>
[[nodiscard]] consteval auto fp_make_member_meta(
	std::index_sequence<Is...>) {
	auto const members = conflux::json::JsonMembers<T>::members();
	using MembersTuple = std::remove_cvref_t<decltype(members)>;
	return std::array<FpMemberMeta<T>, sizeof...(Is)>{
		FpMemberMeta<T>{
						.name = jm_member(get<Is>(members)).name,
						.decode = &fp_decode_member_at<T, Is>,
						.required = !is_optional<
				std::remove_cvref_t<json_member_entry_value_type_t<std::tuple_element_t<Is, MembersTuple>>>>::value,
						}
        ...
    };
}

template<class T>
inline constexpr auto fp_member_meta_v = [] {
	using MembersTuple = std::remove_cvref_t<decltype(conflux::json::JsonMembers<T>::members())>;
	constexpr std::size_t member_count = std::tuple_size_v<MembersTuple>;
	return fp_make_member_meta<T>(std::make_index_sequence<member_count>{});
}();

template<class T>
inline constexpr std::uint64_t fp_required_presence_mask_v = [] {
	std::uint64_t mask = 0;
	for (std::size_t i = 0; i < fp_member_meta_v<T>.size(); ++i) {
		if (fp_member_meta_v<T>[i].required) {
			mask |= std::uint64_t{1} << i;
		}
	}
	return mask;
}();

template<class T>
[[nodiscard]] FpStatus fp_decode_struct(
	T &out,
	FpCursor &c,
	FpLimits const &lim) noexcept {
	auto const &meta = fp_member_meta_v<T>;
	constexpr std::size_t N = fp_member_meta_v<T>.size();

	if (c.at_end() || *c.p != '{') {
		return FpStatus::bail;
	}
	if (c.depth + 1 > lim.max_depth) {
		return FpStatus::bail;
	}
	++c.depth;
	++c.p;

	std::uint64_t presence = 0;
	// Adaptive prediction: documents usually present members in a fixed
	// pattern (declaration order, reverse, strided). Predict next = prev +
	// stride; on a miss fall back to the prefix-hash table and re-learn the
	// stride. First key predicts member 0.
	std::size_t prev_idx = N;
	std::ptrdiff_t stride = 1;
	bool first = true;

	for (;;) {
		c.skip_ws();
		if (c.at_end()) {
			return FpStatus::bail;
		}
		if (*c.p == '}') {
			++c.p;
			--c.depth;
			break;
		}
		if (!first) {
			if (*c.p != ',') {
				return FpStatus::bail;
			}
			++c.p;
			c.skip_ws();
			if (c.at_end()) {
				return FpStatus::bail;
			}
		}
		first = false;

		// --- key ---
		if (*c.p != '"') {
			return FpStatus::bail;
		}
		++c.p;
		std::size_t idx = N;
		FpStringView key{};
		std::string_view key_name{};
		std::ptrdiff_t const predicted = prev_idx == N ? 0 : static_cast<std::ptrdiff_t>(prev_idx) + stride;
		if (predicted >= 0
			&& predicted < static_cast<std::ptrdiff_t>(N)
			&& fp_match_plain_key(c, meta[static_cast<std::size_t>(predicted)].name, lim.max_string)) [[likely]] {
			idx = static_cast<std::size_t>(predicted);
			key_name = meta[idx].name;
		} else {
			if (fp_scan_plain_string(c, key, lim.max_string) != FpStatus::ok) {
				// Escaped or non-ASCII key: slow path handles decode + match.
				return FpStatus::bail;
			}
			key_name = std::string_view{key.data, key.size};
		}

		c.skip_ws();
		if (c.at_end() || *c.p != ':') {
			return FpStatus::bail;
		}
		++c.p;

		// --- match ---
		if (idx == N) {
			if constexpr (N > kJsonMemberLinearLookupLimit) {
				if constexpr (fp_key_table_v<T>.valid) {
					idx = fp_key_table_v<T>.find(key.data, key.size, c.end, fp_member_names_v<T>);
				} else {
					if (auto const *e = find_json_member_lookup_entry<T>(key_name); e != nullptr) {
						idx = e->index;
					}
				}
			} else {
				for (std::size_t i = 0; i < N; ++i) {
					if (key_name == meta[i].name) {
						idx = i;
						break;
					}
				}
			}
		}

		if (idx == N) {
			// Unknown member. Both outcomes are positional-info-free, so the
			// reject diagnostic is produced authoritatively here.
			if (lim.unknown_members == UnknownMemberPolicy::reject) {
				c.error = FpError{.code = JsonIssueCode::invalid_value, .member_name = key_name};
				return FpStatus::error;
			}
			if (fp_skip_value(c, lim) != FpStatus::ok) {
				return FpStatus::bail;
			}
			continue;
		}

		if (prev_idx != N) {
			stride = static_cast<std::ptrdiff_t>(idx) - static_cast<std::ptrdiff_t>(prev_idx);
		}
		prev_idx = idx;

		// --- duplicates ---
		std::uint64_t const bit = std::uint64_t{1} << idx;
		if ((presence & bit) != 0U) [[unlikely]] {
			if (lim.duplicate_key == DuplicateKeyPolicy::reject) {
				c.error = FpError{.code = JsonIssueCode::duplicate_member, .member_name = meta[idx].name};
				return FpStatus::error;
			}
			if (lim.duplicate_key == DuplicateKeyPolicy::first_wins) {
				if (fp_skip_value(c, lim) != FpStatus::ok) {
					return FpStatus::bail;
				}
				continue;
			}
			// last_wins: fall through and decode over the previous value.
		}
		presence |= bit;

		// --- value ---
		FpStatus const st = [&]() noexcept {
			if constexpr (N == 1U) {
				return fp_decode_member_at<T, 0U>(out, c, lim);
			} else if constexpr (N <= 16U) {
				return fp_decode_member_by_index<T>(idx, out, c, lim);
			} else {
				return meta[idx].decode(out, c, lim);
			}
		}();
		if (st != FpStatus::ok) {
			return st;
		}
	}

	if ((presence & fp_required_presence_mask_v<T>) != fp_required_presence_mask_v<T>) {
		for (std::size_t i = 0; i < N; ++i) {
			if (meta[i].required && (presence & (std::uint64_t{1} << i)) == 0U) {
				c.error = FpError{.code = JsonIssueCode::missing_member, .member_name = meta[i].name};
				return FpStatus::error;
			}
		}
	}
	return FpStatus::ok;
}

// ---------------------------------------------------------------------------
// Document entry point
// ---------------------------------------------------------------------------

template<class T>
[[nodiscard]] FpStatus fp_decode_document(
	T &out,
	FpError &out_error,
	std::string_view input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) noexcept {
	// Only strict mode; JSON5 always uses the reader.
	if (parse_opts.mode != ParseMode::strict) {
		return FpStatus::bail;
	}
	// Limits: resolve to concrete values; "unlimited" maps to SIZE_MAX.
	std::size_t max_input = kDefaultMaxInput;
	if (parse_opts.max_input_size.is_unlimited()) {
		max_input = std::numeric_limits<std::size_t>::max();
	} else if (auto v = parse_opts.max_input_size.explicit_value()) {
		max_input = *v;
	}
	if (input.size() > max_input) {
		return FpStatus::bail;
	}
	FpLimits lim{
		.max_string = kDefaultMaxString,
		.max_depth = static_cast<std::uint32_t>(kDefaultMaxDepth),
		.duplicate_key = parse_opts.duplicate_key,
		.unknown_members = decode_opts.unknown_members,
	};
	if (parse_opts.max_string_size.is_unlimited()) {
		lim.max_string = std::numeric_limits<std::size_t>::max();
	} else if (auto v = parse_opts.max_string_size.explicit_value()) {
		lim.max_string = *v;
	}
	if (parse_opts.max_depth.is_unlimited()) {
		lim.max_depth = std::numeric_limits<std::uint32_t>::max();
	} else if (auto v = parse_opts.max_depth.explicit_value()) {
		if (*v > std::numeric_limits<std::uint32_t>::max()) {
			lim.max_depth = std::numeric_limits<std::uint32_t>::max();
		} else {
			lim.max_depth = static_cast<std::uint32_t>(*v);
		}
	}

	FpCursor c{.p = input.data(), .end = input.data() + input.size()};
	c.skip_ws();
	FpStatus const st = fp_decode_value<T>(out, c, lim);
	if (st != FpStatus::ok) {
		out_error = c.error;
		return st;
	}
	// No trailing garbage allowed for full-document decode.
	c.skip_ws();
	if (!c.at_end()) {
		return FpStatus::bail;
	}
	return FpStatus::ok;
}

} // namespace fastpath

[[nodiscard]] [[gnu::cold, gnu::noinline]] inline std::expected<void, JsonError> decode_fast_path_error(
	fastpath::FpError const &fast_error) {
	std::string_view const name = fast_error.member_name;
	if (fast_error.code == JsonIssueCode::duplicate_member) {
		return std::unexpected(detail::duplicate_member_error(name));
	}
	if (fast_error.code == JsonIssueCode::missing_member) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::missing_member,
				.member_name = std::string{name},
				.message = std::format("missing member: {}", name)});
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::decode,
			.code = JsonIssueCode::invalid_value,
			.member_name = std::string{name},
			.message = std::format("unknown member: {}", name)});
}

} // namespace detail
export template<class T>
std::expected<T, JsonError> decode(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	return decode_full<T>(reader, opts);
}
export template<class T>
std::expected<T, JsonError> decode_direct(
	JsonReader &reader,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	return detail::decode_with_reader<T>(reader, opts, scratch);
}
export template<class T>
std::expected<T, JsonError> decode_next(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	return detail::decode_with_reader<T>(reader, opts);
}
export template<class T>
std::expected<T, JsonError> decode_full(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	return detail::decode_full_with_reader<T>(reader, opts);
}

namespace detail {

template<ParseMode Mode, class T>
std::expected<void, JsonError> decode_full_into_slow(
	T &out,
	std::string_view input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) {
	JsonReader reader{input, parse_opts};
	auto ne = reader.next_impl<Mode>();
	if (!ne) {
		return std::unexpected(std::move(ne).error());
	}
	if (!*ne) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::unexpected_eof,
				.message = "std::unexpected end of input"});
	}
	JsonDecodeScratch scratch;
	if (auto decoded = decode_into<Mode, T>(out, reader, **ne, decode_opts, &scratch); !decoded) {
		return decoded;
	}
	auto next = reader.next_impl<Mode>();
	if (!next) {
		return std::unexpected(std::move(next).error());
	}
	if (*next) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source = JsonSourceLocation{.offset = reader.value_start_pos()},
				.message = "trailing JSON value after document root"});
	}
	return {};
}

} // namespace detail

export template<class T>
std::expected<void, JsonError> decode_full_into(
	T &out,
	std::string_view input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) {
	// Fast path: cursor-based strict-JSON decode for supported member sets.
	// On any malformed/unsupported/limit-violating input it bails and the
	// JsonReader-based decoder below produces the authoritative result and
	// diagnostics. Duplicate/unknown/missing member rejections (which carry
	// no positional diagnostics) are produced directly.
	if constexpr (detail::fastpath::fp_supported_v<T>) {
		if (parse_opts.mode == ParseMode::strict) {
			detail::fastpath::FpError fast_error{};
			auto const st = detail::fastpath::fp_decode_document<T>(out, fast_error, input, parse_opts, decode_opts);
			if (st == detail::fastpath::FpStatus::ok) [[likely]] {
				return {};
			}
			if (st == detail::fastpath::FpStatus::error) [[unlikely]] {
				return detail::decode_fast_path_error(fast_error);
			}
		}
	}
	if (parse_opts.mode == ParseMode::strict) {
		return detail::decode_full_into_slow<ParseMode::strict, T>(out, input, parse_opts, decode_opts);
	}
	return detail::decode_full_into_slow<ParseMode::json5, T>(out, input, parse_opts, decode_opts);
}
export template<class T>
std::expected<T, JsonError> decode_full(
	std::string_view input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) {
	T result{};
	if (auto decoded = decode_full_into<T>(result, input, parse_opts, decode_opts); !decoded) {
		return std::unexpected(std::move(decoded).error());
	}
	return result;
}
export template<class T>
std::expected<T, JsonError> decode_borrowed(
	std::string_view input,
	JsonParseOptions const &parse_opts = {},
	JsonDecodeOptions const &decode_opts = {}) {
	return decode_full<T>(input, parse_opts, decode_opts);
}
export template<class T>
std::expected<T, JsonError> decode_owned(
	std::string_view input,
	JsonParseOptions const &parse_opts = {},
	JsonDecodeOptions const &decode_opts = {}) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (json_contains_borrowed_view_v<Raw>) {
		static_assert(
			!json_contains_borrowed_view_v<Raw>,
			"decode_owned<T> is not lifetime-safe when T contains std::string_view/span fields");
	} else {
		return decode_borrowed<T>(input, parse_opts, decode_opts);
	}
}
namespace detail {

template<class T>
std::expected<T, JsonError> decode_with_frames(
	NodeRef root,
	PathFrameStack &frames,
	JsonDecodeOptions const &opts) {
	if constexpr (has_codec_spec<T>::value) {
		return decode_codec<T>(root, opts);
	} else if constexpr (has_members_spec<T>::value) {
		auto obj = root.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		auto const members = conflux::json::JsonMembers<T>::members();
		using MembersTuple = std::remove_cvref_t<decltype(members)>;
		constexpr std::size_t kMemberCount = std::tuple_size_v<MembersTuple>;
		std::array<NodeRef, kMemberCount> member_values{};
		std::array<bool, kMemberCount> member_seen{};
		bool has_unknown = false;
		JsonError first_unknown;
		for (auto const &member: obj->members()) {
			bool matched = false;
			std::size_t field_idx = 0;
			apply(
				[&](auto const &...entries) {
					(([&] {
						 std::size_t const idx = field_idx++;
						 if (matched) {
							 return;
						 }
						 auto const &m = jm_member(entries);
						 if (member.name != m.name) {
							 return;
						 }
						 matched = true;
						 if (!member_seen[idx]) {
							 member_values[idx] = member.value;
							 member_seen[idx] = true;
						 }
					 }()),
					 ...);
				},
				members);
			if (!matched && opts.unknown_members == UnknownMemberPolicy::reject && !has_unknown) {
				has_unknown = true;
				first_unknown = JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.path = materialize_path(frames),
					.member_name = std::string{member.name},
					.message = std::format("unknown member: {}", member.name)};
			}
		}
		T result{};
		bool ok = true;
		JsonError first_err;
		std::size_t field_idx = 0;
		apply(
			[&](auto const &...entries) {
				(([&](auto const &entry) {
					 if (!ok) {
						 return;
					 }
					 std::size_t const idx = field_idx++;
					 auto const &m = jm_member(entry);
					 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
					 if (!member_seen[idx]) {
						 if constexpr (is_optional<M>::value) {
							 result.*m.pointer = M{};
						 } else {
							 ok = false;
							 first_err = JsonError{
								 .stage = JsonStage::decode,
								 .code = JsonIssueCode::missing_member,
								 .path = materialize_path(frames),
								 .member_name = std::string{m.name},
								 .message = std::format("missing member: {}", m.name)};
						 }
						 return;
					 }
					 frames.push_back({PathFrame::Kind::member, m.name, 0});
					 auto decoded = decode_with_frames<M>(member_values[idx], frames, opts);
					 if (!decoded) {
						 ok = false;
						 first_err = std::move(decoded).error();
						 if (first_err.path.empty()) {
							 first_err.path = materialize_path(frames);
						 }
						 frames.pop_back();
						 return;
					 }
					 frames.pop_back();
					 result.*m.pointer = std::move(*decoded);
					 auto cfn = jm_constraint(entry);
					 if (cfn != nullptr) {
						 if (auto cr = cfn(result.*m.pointer); !cr) {
							 ok = false;
							 first_err = std::move(cr).error();
							 first_err.member_name = std::string{m.name};
							 if (first_err.path.empty()) {
								 first_err.path = materialize_path(frames);
							 }
						 }
					 }
				 })(entries),
				 ...);
			},
			members);
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		if (has_unknown) {
			return std::unexpected(std::move(first_unknown));
		}
		return result;
	} else {
		static_assert(false, "No JsonCodec<T> or conflux::json::JsonMembers<T> found for T");
	}
}

} // namespace detail
// decode<T> dispatch
export template<class T>
std::expected<T, JsonError> decode(
	NodeRef root,
	JsonDecodeOptions const &opts) {
	detail::PathFrameStack frames;
	return detail::decode_with_frames<T>(root, frames, opts);
}
// ---------------------------------------------------------------------------
// Generic builder methods — defined after codec specializations
// ---------------------------------------------------------------------------

template<class T>
	requires has_json_codec<T>
std::expected<void, JsonError> ArrayBuilder::append(
	T const &value) {
	if (!arr_check_active(frame_)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	auto node_or = detail::encode_into<T>(st, value);
	if (!node_or) {
		return std::unexpected(std::move(node_or).error());
	}
	frame_.local_children.push_back(*node_or);
	return {};
}
template<class T>
	requires has_json_codec<T>
std::expected<void, JsonError> ObjectBuilder::insert(
	std::string_view name,
	T const &value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	// Spec: duplicate-name rejection happens before dispatching to JsonCodec<T>::encode.
	if (frame_.dup_check.contains(name)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = std::string{name},
				.message = std::format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	auto node_or = detail::encode_into<T>(st, value);
	if (!node_or) {
		return std::unexpected(std::move(node_or).error());
	}
	return do_insert_node(name, *node_or);
}
namespace detail {

// Encode dispatch: mirrors the decode<T> dispatch logic.
template<class T>
std::expected<void, JsonError> encode_dispatch(
	ValueBuilder &b,
	T const &value) {
	if constexpr (has_codec_spec<T>::value) {
		return JsonCodec<T>::encode(b, value);
	} else if constexpr (has_members_spec<T>::value) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto &obj = *obj_res;
		auto const members = conflux::json::JsonMembers<T>::members();
		bool ok = true;
		JsonError first_err;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &m) {
					 if (!ok) {
						 return;
					 }
					 using M = std::remove_cvref_t<decltype(value.*m.pointer)>;
					 auto res = obj.template insert<M>(m.name, value.*m.pointer);
					 if (!res) {
						 ok = false;
						 first_err = std::move(res).error();
					 }
				 })(ms),
				 ...);
			},
			members);
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		std::move(obj).commit();
		return {};
	} else {
		static_assert(false, "No JsonCodec<T> or conflux::json::JsonMembers<T> found for T");
	}
}

} // namespace detail
template<class T>
	requires has_json_codec<T>
std::expected<void, JsonError> ValueBuilder::set(
	T const &value) {
	if (auto ok = check_can_set(); !ok) {
		return ok;
	}
	return detail::encode_dispatch<T>(*this, value);
}

namespace detail {

template<class Out>
void direct_append_u_escape(
	Out &out,
	std::uint32_t cp) {
	static constexpr std::array<char, 16> kHex =
		{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	out += "\\u";
	out += kHex[(cp >> 12U) & 0x0FU];
	out += kHex[(cp >> 8U) & 0x0FU];
	out += kHex[(cp >> 4U) & 0x0FU];
	out += kHex[cp & 0x0FU];
}

template<class Out>
void direct_dump_string(
	Out &out,
	std::string_view sv,
	bool ascii_only) {
	out += '"';
	std::size_t run_start = 0;
	auto flush_run = [&](std::size_t end) {
		if (end > run_start) {
			out.append(sv.data() + run_start, end - run_start);
		}
	};
	for (std::size_t i = 0; i < sv.size();) {
		auto const c = static_cast<unsigned char>(sv[i]);
		if (c != '"' && c != '\\' && c >= 0x20U && (!ascii_only || c < 0x80U)) {
			++i;
			continue;
		}
		flush_run(i);
		switch (c) {
		case '"':
			out += "\\\"";
			++i;
			break;
		case '\\':
			out += "\\\\";
			++i;
			break;
		case '\b':
			out += "\\b";
			++i;
			break;
		case '\f':
			out += "\\f";
			++i;
			break;
		case '\n':
			out += "\\n";
			++i;
			break;
		case '\r':
			out += "\\r";
			++i;
			break;
		case '\t':
			out += "\\t";
			++i;
			break;
		default:
			if (c < 0x20U) {
				direct_append_u_escape(out, c);
				++i;
			} else if (ascii_only && c >= 0x80U) {
				std::uint32_t cp = 0;
				std::size_t seq = 0;
				if (c < 0xE0U) {
					cp = c & 0x1FU;
					seq = 2;
				} else if (c < 0xF0U) {
					cp = c & 0x0FU;
					seq = 3;
				} else {
					cp = c & 0x07U;
					seq = 4;
				}
				for (std::size_t k = 1; k < seq && i + k < sv.size(); ++k) {
					cp = (cp << 6U) | (static_cast<unsigned char>(sv[i + k]) & 0x3FU);
				}
				i += std::min(seq, sv.size() - i);
				if (cp < 0x10000U) {
					direct_append_u_escape(out, cp);
				} else {
					cp -= 0x10000U;
					direct_append_u_escape(out, 0xD800U | (cp >> 10U));
					direct_append_u_escape(out, 0xDC00U | (cp & 0x3FFU));
				}
			}
		}
		run_start = i;
	}
	flush_run(sv.size());
	out += '"';
}

template<class Out>
void direct_indent(
	Out &out,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (!opts.pretty) {
		return;
	}
	out += '\n';
	out.append(static_cast<std::size_t>(depth) * opts.indent, opts.indent_char);
}

template<class Out, class T>
std::expected<void, JsonError>
direct_write_value(Out &out, T const &value, JsonDumpOptions const &opts, unsigned depth);

template<class Out, class T>
std::expected<void, JsonError> direct_write_array_like(
	Out &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	out += '[';
	bool first = true;
	for (auto const &elem: value) {
		if (!first) {
			out += ',';
		}
		if (opts.pretty) {
			direct_indent(out, opts, depth + 1);
		}
		if (auto ok = direct_write_value(out, elem, opts, depth + 1); !ok) {
			return ok;
		}
		first = false;
	}
	if (opts.pretty && !first) {
		direct_indent(out, opts, depth);
	}
	out += ']';
	return {};
}

template<class Out, class Tuple, std::size_t... Is>
std::expected<void, JsonError> direct_write_tuple_impl(
	Out &out,
	Tuple const &value,
	JsonDumpOptions const &opts,
	unsigned depth,
	std::index_sequence<Is...>) {
	out += '[';
	bool ok = true;
	bool first = true;
	JsonError first_err;
	(([&]<std::size_t I>() {
		 if (!ok) {
			 return;
		 }
		 if (!first) {
			 out += ',';
		 }
		 if (opts.pretty) {
			 direct_indent(out, opts, depth + 1);
		 }
		 if (auto res = direct_write_value(out, get<I>(value), opts, depth + 1); !res) {
			 ok = false;
			 first_err = std::move(res).error();
			 return;
		 }
		 first = false;
	 }.template operator ()<Is>()),
	 ...);
	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	if (opts.pretty && !first) {
		direct_indent(out, opts, depth);
	}
	out += ']';
	return {};
}

template<class Out, class Tuple>
std::expected<void, JsonError> direct_write_tuple_like(
	Out &out,
	Tuple const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	using TupleRaw = std::remove_cvref_t<Tuple>;
	return direct_write_tuple_impl(out, value, opts, depth, std::make_index_sequence<std::tuple_size_v<TupleRaw>>{});
}

template<class Out, class Map>
std::expected<void, JsonError> direct_write_map_like(
	Out &out,
	Map const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (opts.sort_object_keys) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::dump,
				.code = JsonIssueCode::invalid_value,
				.message = "direct writer does not support sort_object_keys"});
	}
	out += '{';
	bool first = true;
	for (auto const &[key, val]: value) {
		if (!first) {
			out += ',';
		}
		if (opts.pretty) {
			direct_indent(out, opts, depth + 1);
		}
		direct_dump_string(out, key, opts.ascii_only);
		out += opts.pretty ? ": " : ":";
		if (auto res = direct_write_value(out, val, opts, depth + 1); !res) {
			auto err = std::move(res).error();
			err.member_name = key;
			return std::unexpected(std::move(err));
		}
		first = false;
	}
	if (opts.pretty && !first) {
		direct_indent(out, opts, depth);
	}
	out += '}';
	return {};
}

template<class Out, class T>
std::expected<void, JsonError> direct_write_members(
	Out &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (opts.sort_object_keys) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::dump,
				.code = JsonIssueCode::invalid_value,
				.message = "direct writer does not support sort_object_keys"});
	}
	out += '{';
	auto const members = conflux::json::JsonMembers<T>::members();
	bool ok = true;
	bool first = true;
	JsonError first_err;
	apply(
		[&](auto const &...entries) {
			(([&](auto const &entry) {
				 if (!ok) {
					 return;
				 }
				 auto const &m = jm_member(entry);
				 if (!first) {
					 out += ',';
				 }
				 if (opts.pretty) {
					 direct_indent(out, opts, depth + 1);
				 }
				 direct_dump_string(out, m.name, opts.ascii_only);
				 out += opts.pretty ? ": " : ":";
				 if (auto res = direct_write_value(out, value.*m.pointer, opts, depth + 1); !res) {
					 ok = false;
					 first_err = std::move(res).error();
					 first_err.member_name = std::string{m.name};
					 return;
				 }
				 first = false;
			 })(entries),
			 ...);
		},
		members);
	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	if (opts.pretty && !first) {
		direct_indent(out, opts, depth);
	}
	out += '}';
	return {};
}

template<class Out, class T>
std::expected<void, JsonError> direct_write_value(
	Out &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (std::same_as<Raw, bool>) {
		out += value ? "true" : "false";
		return {};
	} else if constexpr (std::same_as<Raw, std::string>) {
		direct_dump_string(out, value, opts.ascii_only);
		return {};
	} else if constexpr (std::same_as<Raw, std::string_view>) {
		direct_dump_string(out, value, opts.ascii_only);
		return {};
	} else if constexpr ((std::integral<Raw> && !std::same_as<Raw, bool>) || std::floating_point<Raw>) {
		if constexpr (std::floating_point<Raw>) {
			if (!std::isfinite(value)) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::dump,
						.code = JsonIssueCode::number_out_of_range,
						.message = "direct writer requires finite number"});
			}
		}
		std::array<char, 64> buf{};
		auto *first = buf.data();
		auto *last = buf.data() + buf.size();
		auto [ptr, ec] = std::to_chars(first, last, value);
		if (ec != std::errc{}) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::dump,
					.code = JsonIssueCode::invalid_number,
					.message = "number formatting failed"});
		}
		out.append(first, static_cast<std::size_t>(ptr - first));
		return {};
	} else if constexpr (is_optional<Raw>::value) {
		if (!value) {
			out += "null";
			return {};
		}
		return direct_write_value(out, *value, opts, depth);
	} else if constexpr (is_nullable_type<Raw>::value) {
		if (!value.has_value()) {
			out += "null";
			return {};
		}
		return direct_write_value(out, value.value(), opts, depth);
	} else if constexpr (is_vector_of_v<Raw> || is_std_array_v<Raw>) {
		return direct_write_array_like(out, value, opts, depth);
	} else if constexpr (is_pair_v<Raw> || is_tuple_of_v<Raw>) {
		return direct_write_tuple_like(out, value, opts, depth);
	} else if constexpr (is_map_type_v<Raw> || is_unordered_map_type_v<Raw>) {
		return direct_write_map_like(out, value, opts, depth);
	} else if constexpr (has_members_spec<Raw>::value) {
		return direct_write_members(out, value, opts, depth);
	} else {
		static_assert(!std::same_as<Raw, Raw>, "No direct JSON writer support for type");
	}
}

template<class T>
[[nodiscard]] std::size_t direct_members_reserve_hint(
	JsonDumpOptions const &opts) {
	auto const members = conflux::json::JsonMembers<T>::members();
	using MembersTuple = std::remove_cvref_t<decltype(members)>;
	constexpr std::size_t kMemberCount = std::tuple_size_v<MembersTuple>;
	std::size_t name_bytes = 0;
	apply([&](auto const &...entries) { ((name_bytes += jm_member(entries).name.size()), ...); }, members);
	std::size_t hint = 2 + name_bytes;
	if constexpr (kMemberCount > 0) {
		hint += kMemberCount * 20U;
		hint += kMemberCount - 1U;
		if (opts.pretty) {
			hint += kMemberCount * (static_cast<std::size_t>(opts.indent) + 2U);
		}
	}
	return hint;
}

template<class Map>
[[nodiscard]] std::size_t direct_map_reserve_hint(
	Map const &value,
	JsonDumpOptions const &opts) {
	std::size_t hint = 2;
	for (auto const &[key, unused]: value) {
		(void)unused;
		hint += key.size() + 20U;
	}
	if (!value.empty()) {
		hint += value.size() - 1U;
		if (opts.pretty) {
			hint += value.size() * (static_cast<std::size_t>(opts.indent) + 3U);
		}
	}
	return hint;
}

template<class Seq>
[[nodiscard]] std::size_t direct_sequence_reserve_hint(
	Seq const &value,
	JsonDumpOptions const &opts) {
	std::size_t const count = value.size();
	std::size_t hint = 2 + count * 16U;
	if (count > 0) {
		hint += count - 1U;
		if (opts.pretty) {
			hint += count * (static_cast<std::size_t>(opts.indent) + 1U);
		}
	}
	return hint;
}

template<class T>
[[nodiscard]] std::size_t direct_root_reserve_hint(
	T const &value,
	JsonDumpOptions const &opts) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (has_members_spec<Raw>::value) {
		return direct_members_reserve_hint<Raw>(opts);
	} else if constexpr (is_map_type_v<Raw> || is_unordered_map_type_v<Raw>) {
		return direct_map_reserve_hint(value, opts);
	} else if constexpr (is_vector_of_v<Raw> || is_std_array_v<Raw>) {
		return direct_sequence_reserve_hint(value, opts);
	} else if constexpr (is_pair_v<Raw>) {
		return 34U + (opts.pretty ? static_cast<std::size_t>(opts.indent) * 2U + 2U : 0U);
	} else if constexpr (is_tuple_of_v<Raw>) {
		return 2U + std::tuple_size_v<Raw> * (16U + (opts.pretty ? static_cast<std::size_t>(opts.indent) + 1U : 0U));
	} else if constexpr (is_basic_string_of_char_v<Raw> || std::same_as<Raw, std::string_view>) {
		return value.size() + 2U;
	} else {
		return 64U;
	}
}

template<class Sink>
class DirectChunkSink {
	static constexpr std::size_t kInlineCapacity = 1024;
	std::remove_reference_t<Sink> *sink_{};
	std::array<char, kInlineCapacity> buffer_{};
	std::size_t used_{};

	void emit_direct(
		std::string_view chunk) {
		if (!chunk.empty()) {
			std::invoke(*sink_, chunk);
		}
	}

	void emit(
		std::string_view chunk) {
		if (chunk.empty()) {
			return;
		}
		if (chunk.size() >= buffer_.size()) {
			flush();
			emit_direct(chunk);
			return;
		}
		if (chunk.size() > buffer_.size() - used_) {
			flush();
		}
		std::ranges::copy(chunk, buffer_.data() + used_);
		used_ += chunk.size();
	}

public:
	explicit DirectChunkSink(
		Sink &sink) noexcept
		: sink_{std::addressof(sink)} {}

	void flush() {
		if (used_ == 0) {
			return;
		}
		emit_direct(std::string_view{buffer_.data(), used_});
		used_ = 0;
	}

	void operator +=(
		char c) {
		if (used_ == buffer_.size()) {
			flush();
		}
		buffer_[used_++] = c;
	}

	void operator +=(
		char const *text) {
		emit(text);
	}

	void append(
		char const *data,
		std::size_t size) {
		emit(std::string_view{data, size});
	}

	void append(
		std::size_t count,
		char ch) {
		while (count > 0) {
			if (used_ == buffer_.size()) {
				flush();
			}
			std::size_t const n = std::min(count, buffer_.size() - used_);
			std::ranges::fill_n(buffer_.data() + used_, static_cast<std::ptrdiff_t>(n), ch);
			used_ += n;
			count -= n;
		}
	}
};

} // namespace detail

export template<class T>
std::expected<void, JsonError> write_json_direct(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (JsonDirectWritable<Raw>) {
		return detail::direct_write_value(out, value, opts, 0);
	} else {
		static_assert(!std::same_as<Raw, Raw>, "No direct JSON writer support for type");
	}
}

export template<class T>
std::expected<std::string, JsonError> dump_direct(
	T const &value,
	JsonDumpOptions const &opts) {
	std::string out;
	using Raw = std::remove_cvref_t<T>;
	if constexpr (JsonDirectWritable<Raw>) {
		out.reserve(detail::direct_root_reserve_hint<Raw>(value, opts));
	}
	if (auto ok = write_json_direct(out, value, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return out;
}

export template<class T, class Sink>
std::expected<void, JsonError> write_json_direct_to(
	T const &value,
	JsonDumpOptions const &opts,
	Sink &&sink) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (JsonDirectWritable<Raw>) {
		detail::DirectChunkSink<Sink> out{sink};
		auto written = detail::direct_write_value(out, value, opts, 0);
		out.flush();
		if (!written) {
			return written;
		}
		return {};
	} else {
		static_assert(!std::same_as<Raw, Raw>, "No direct JSON writer support for type");
	}
}
// ---------------------------------------------------------------------------
// Phase 8.3 — schema_for / validate
// ---------------------------------------------------------------------------

namespace detail {

template<class M>
constexpr std::string_view json_type_name() noexcept {
	using Raw = std::remove_cvref_t<M>;
	if constexpr (std::same_as<Raw, bool>) {
		return "boolean";
	} else if constexpr (
		std::same_as<Raw, std::int64_t>
		|| std::same_as<Raw, std::uint64_t>
		|| std::same_as<Raw, std::int32_t>
		|| std::same_as<Raw, std::uint32_t>
		|| std::same_as<Raw, std::int16_t>
		|| std::same_as<Raw, std::uint16_t>
		|| std::same_as<Raw, std::int8_t>
		|| std::same_as<Raw, std::uint8_t>) {
		return "integer";
	} else if constexpr (std::same_as<Raw, double> || std::same_as<Raw, float>) {
		return "number";
	} else if constexpr (is_basic_string_of_char_v<Raw> || std::same_as<Raw, std::string_view>) {
		return "string";
	} else if constexpr (is_vector_of_v<Raw> || is_std_array_v<Raw>) {
		return "array";
	} else if constexpr (is_map_type_v<Raw> || is_unordered_map_type_v<Raw>) {
		return "object";
	} else if constexpr (has_codec_spec<Raw>::value || has_members_spec<Raw>::value) {
		return "object";
	} else {
		return "any";
	}
}
template<class M>
std::expected<void, JsonError> schema_insert_type(
	ObjectBuilder &obj) {
	using Raw = std::remove_cvref_t<M>;
	if constexpr (is_optional<Raw>::value) {
		using Inner = typename Raw::value_type;
		return obj.insert_string("type", json_type_name<Inner>());
	} else if constexpr (is_nullable_type<Raw>::value) {
		using Inner = nullable_inner_t<Raw>;
		if (auto ok = obj.insert_string("type", json_type_name<Inner>()); !ok) {
			return ok;
		}
		return obj.insert_bool("nullable", true);
	} else {
		return obj.insert_string("type", json_type_name<Raw>());
	}
}

} // namespace detail
export template<class T>
	requires(detail::has_members_spec<T>::value || detail::has_codec_spec<T>::value)
std::expected<Document, JsonError> schema_for() {
	ValueBuilder vb;
	auto obj_r = vb.begin_object();
	if (!obj_r) {
		return std::unexpected(std::move(obj_r).error());
	}
	auto &schema = *obj_r;
	if (auto ok = schema.insert_string("type", "object"); !ok) {
		return std::unexpected(std::move(ok).error());
	}

	if constexpr (detail::has_members_spec<T>::value) {
		auto props_r = schema.insert_object("properties");
		auto &props = *props_r;
		auto const members = conflux::json::JsonMembers<T>::members();
		std::optional<JsonError> first_error;

		apply(
			[&](auto const &...ms) {
				(([&](auto const &entry) {
					 if (first_error) {
						 return;
					 }
					 auto const &m = detail::jm_member(entry);
					 using M = std::remove_reference_t<decltype(std::declval<T>().*m.pointer)>;
					 auto field_r = props.insert_object(m.name);
					 auto &field = *field_r;
					 if (auto ok = detail::schema_insert_type<M>(field); !ok) {
						 first_error = std::move(ok).error();
						 return;
					 }
					 std::move(field).commit();
				 })(ms),
				 ...);
			},
			members);
		if (first_error) {
			return std::unexpected(std::move(*first_error));
		}
		std::move(props).commit();

		auto req_r = schema.insert_array("required");
		auto &req = *req_r;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &entry) {
					 if (first_error) {
						 return;
					 }
					 auto const &m = detail::jm_member(entry);
					 using M = std::remove_reference_t<decltype(std::declval<T>().*m.pointer)>;
					 if constexpr (!detail::is_optional<std::remove_cvref_t<M>>::value) {
						 if (auto ok = req.append_string(m.name); !ok) {
							 first_error = std::move(ok).error();
						 }
					 }
				 })(ms),
				 ...);
			},
			members);
		if (first_error) {
			return std::unexpected(std::move(*first_error));
		}
		std::move(req).commit();
	}

	std::move(schema).commit();
	return std::move(vb).finish();
}
export [[nodiscard]] std::expected<void, JsonError> validate(NodeRef root, NodeRef schema);

// ---------------------------------------------------------------------------
// JsonWritable concept + make_object / make_array factories (Phase 1.4)
// ---------------------------------------------------------------------------

export template<class T>
concept JsonWritable = std::same_as<std::remove_cvref_t<T>, bool>
					|| has_json_codec<std::remove_cvref_t<T>>
					|| std::same_as<std::remove_cvref_t<T>, std::string>
					|| std::convertible_to<std::remove_cvref_t<T>, std::string_view>
					|| (std::integral<std::remove_cvref_t<T>>
						&& !std::same_as<std::remove_cvref_t<T>, bool>
						&& !std::same_as<std::remove_cvref_t<T>, char>
						&& !std::same_as<std::remove_cvref_t<T>, char8_t>
						&& !std::same_as<std::remove_cvref_t<T>, signed char>
						&& !std::same_as<std::remove_cvref_t<T>, unsigned char>
						&& !std::same_as<std::remove_cvref_t<T>, wchar_t>
						&& !std::same_as<std::remove_cvref_t<T>, char16_t>
						&& !std::same_as<std::remove_cvref_t<T>, char32_t>)
					|| std::floating_point<std::remove_cvref_t<T>>;

export template<class P>
concept JsonObjectPair = std::tuple_size<std::remove_cvref_t<P>>::value == 2
					  && std::convertible_to<std::tuple_element_t<0, std::remove_cvref_t<P>>, std::string_view>
					  && JsonWritable<std::tuple_element_t<1, std::remove_cvref_t<P>>>;
// Internal dispatch: encode a JsonWritable value into ObjectBuilder.
namespace detail {

template<class T>
std::expected<void, JsonError> write_writable(
	ObjectBuilder &obj,
	std::string_view name,
	T const &value) {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::same_as<U, bool>) {
		return obj.insert_bool(name, value);
	} else if constexpr (has_json_codec<U>) {
		return obj.template insert<U>(name, value);
	} else if constexpr (std::same_as<U, std::string>) {
		return obj.insert_string(name, std::string_view{value});
	} else if constexpr (std::convertible_to<U, std::string_view>) {
		std::string_view sv = static_cast<std::string_view>(value);
		if constexpr (std::same_as<U, char const *> || std::is_pointer_v<U>) {
			if (sv.data() == nullptr) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_value,
						.member_name = std::string{name},
						.message = std::format("null pointer for member '{}'", name)});
			}
		}
		return obj.insert_string(name, sv);
	} else if constexpr (std::is_signed_v<U>) {
		if constexpr (sizeof(U) < sizeof(std::int64_t)) {
			return obj.insert_i64(name, static_cast<std::int64_t>(value));
		} else {
			if (value < static_cast<U>(std::numeric_limits<std::int64_t>::min())
				|| value > static_cast<U>(std::numeric_limits<std::int64_t>::max())) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::number_out_of_range,
						.member_name = std::string{name},
						.message = std::format("value out of std::int64_t range for member '{}'", name)});
			}
			return obj.insert_i64(name, static_cast<std::int64_t>(value));
		}
	} else if constexpr (std::is_unsigned_v<U>) {
		return obj.insert_u64(name, static_cast<std::uint64_t>(value));
	} else {
		return obj.insert_f64(name, static_cast<double>(value));
	}
}
template<class T>
std::expected<void, JsonError> write_writable_arr(
	ArrayBuilder &arr,
	T const &value) {
	using U = std::remove_cvref_t<T>;
	if constexpr (std::same_as<U, bool>) {
		return arr.append_bool(value);
	} else if constexpr (has_json_codec<U>) {
		return arr.template append<U>(value);
	} else if constexpr (std::same_as<U, std::string>) {
		return arr.append_string(std::string_view{value});
	} else if constexpr (std::convertible_to<U, std::string_view>) {
		std::string_view sv = static_cast<std::string_view>(value);
		if constexpr (std::same_as<U, char const *> || std::is_pointer_v<U>) {
			if (sv.data() == nullptr) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_value,
						.message = "null pointer in array element"});
			}
		}
		return arr.append_string(sv);
	} else if constexpr (std::is_signed_v<U>) {
		if constexpr (sizeof(U) < sizeof(std::int64_t)) {
			return arr.append_i64(static_cast<std::int64_t>(value));
		} else {
			if (value < static_cast<U>(std::numeric_limits<std::int64_t>::min())
				|| value > static_cast<U>(std::numeric_limits<std::int64_t>::max())) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::number_out_of_range,
						.message = "value out of std::int64_t range"});
			}
			return arr.append_i64(static_cast<std::int64_t>(value));
		}
	} else if constexpr (std::is_unsigned_v<U>) {
		return arr.append_u64(static_cast<std::uint64_t>(value));
	} else {
		return arr.append_f64(static_cast<double>(value));
	}
}

} // namespace detail
// Heterogeneous variadic form.
export template<class... Pairs>
	requires(JsonObjectPair<std::remove_cvref_t<Pairs>> && ...)
[[nodiscard]] std::expected<Document, JsonError> make_object(
	Pairs &&...pairs) {
	ValueBuilder vb;
	auto obj_or = vb.begin_object();
	if (!obj_or) {
		return std::unexpected(std::move(obj_or).error());
	}
	auto &obj = *obj_or;
	bool ok = true;
	JsonError first_err;
	(([&](auto &&p) {
		 if (!ok) {
			 return;
		 }
		 std::string_view const key = static_cast<std::string_view>(std::get<0>(p));
		 auto res = detail::write_writable(obj, key, std::get<1>(p));
		 if (!res) {
			 ok = false;
			 first_err = std::move(res).error();
		 }
	 })(std::forward<Pairs>(pairs)),
	 ...);
	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	std::move(obj).commit();
	return std::move(vb).finish();
}
// Homogeneous initializer_list form.
export template<class V>
	requires JsonWritable<V>
[[nodiscard]] std::expected<Document, JsonError> make_object(
	std::initializer_list<std::pair<std::string_view, V>> pairs) {
	ValueBuilder vb;
	auto obj_or = vb.begin_object();
	if (!obj_or) {
		return std::unexpected(std::move(obj_or).error());
	}
	auto &obj = *obj_or;
	for (auto const &[k, v]: pairs) {
		auto res = detail::write_writable(obj, k, v);
		if (!res) {
			return std::unexpected(std::move(res).error());
		}
	}
	std::move(obj).commit();
	return std::move(vb).finish();
}
// Heterogeneous variadic array form.
export template<class... Elems>
	requires(JsonWritable<std::remove_cvref_t<Elems>> && ...)
[[nodiscard]] std::expected<Document, JsonError> make_array(
	Elems &&...elems) {
	ValueBuilder vb;
	auto arr_or = vb.begin_array();
	if (!arr_or) {
		return std::unexpected(std::move(arr_or).error());
	}
	auto &arr = *arr_or;
	bool ok = true;
	JsonError first_err;
	(([&](auto &&e) {
		 if (!ok) {
			 return;
		 }
		 auto res = detail::write_writable_arr(arr, std::forward<decltype(e)>(e));
		 if (!res) {
			 ok = false;
			 first_err = std::move(res).error();
		 }
	 })(std::forward<Elems>(elems)),
	 ...);
	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	std::move(arr).commit();
	return std::move(vb).finish();
}
// Homogeneous initializer_list array form.
export template<class V>
	requires JsonWritable<V>
[[nodiscard]] std::expected<Document, JsonError> make_array(
	std::initializer_list<V> elems) {
	ValueBuilder vb;
	auto arr_or = vb.begin_array();
	if (!arr_or) {
		return std::unexpected(std::move(arr_or).error());
	}
	auto &arr = *arr_or;
	for (auto const &e: elems) {
		auto res = detail::write_writable_arr(arr, e);
		if (!res) {
			return std::unexpected(std::move(res).error());
		}
	}
	std::move(arr).commit();
	return std::move(vb).finish();
}

// ─── Phase 3 — SAX / Event Interface ────────────────────────────────────────

export template<class R>
concept HandlerReturn = std::same_as<R, void> || std::convertible_to<R, std::expected<void, JsonError>>;

export template<class H>
concept JsonHandler = requires(H &h, std::string_view sv, std::int64_t i, std::uint64_t u, double d, bool b) {
	requires HandlerReturn<decltype(h.on_null())>;
	requires HandlerReturn<decltype(h.on_bool(b))>;
	requires HandlerReturn<decltype(h.on_string(sv))>;
	requires HandlerReturn<decltype(h.on_i64(i))>;
	requires HandlerReturn<decltype(h.on_u64(u))>;
	requires HandlerReturn<decltype(h.on_double(d))>;
	requires HandlerReturn<decltype(h.on_begin_object())>;
	requires HandlerReturn<decltype(h.on_key(sv))>;
	requires HandlerReturn<decltype(h.on_end_object())>;
	requires HandlerReturn<decltype(h.on_begin_array())>;
	requires HandlerReturn<decltype(h.on_end_array())>;
};
export struct JsonDefaultHandler {
	std::expected<void, JsonError> on_null() { return {}; }
	std::expected<void, JsonError> on_bool(
		bool) {
		return {};
	}
	std::expected<void, JsonError> on_string(
		std::string_view) {
		return {};
	}
	std::expected<void, JsonError> on_i64(
		std::int64_t) {
		return {};
	}
	std::expected<void, JsonError> on_u64(
		std::uint64_t) {
		return {};
	}
	std::expected<void, JsonError> on_double(
		double) {
		return {};
	}
	std::expected<void, JsonError> on_begin_object() { return {}; }
	std::expected<void, JsonError> on_key(
		std::string_view) {
		return {};
	}
	std::expected<void, JsonError> on_end_object() { return {}; }
	std::expected<void, JsonError> on_begin_array() { return {}; }
	std::expected<void, JsonError> on_end_array() { return {}; }
	// on_number_raw intentionally absent
};
namespace detail {

// Invoke callable, normalize return to std::expected<void,JsonError>.
// Avoids passing void as a function argument.
template<class F>
[[nodiscard]] inline std::expected<void, JsonError> invoke_handler(
	F &&f) {
	using R = decltype(std::forward<F>(f)());
	if constexpr (std::same_as<R, void>) {
		std::forward<F>(f)();
		return {};
	} else {
		std::expected<void, JsonError> e = std::forward<F>(f)();
		return e;
	}
}
// Dispatch a number event to the handler.
// If H provides on_number_raw: call it only (raw bytes, no typed conversion).
// Otherwise dispatch on_i64 / on_u64 / on_double based on value kind.
template<JsonHandler H>
[[nodiscard]] std::expected<void, JsonError> dispatch_number(
	H &h,
	JsonNumberView nv) {
	if constexpr (requires { h.on_number_raw(std::string_view{}); }) {
		static_assert(
			HandlerReturn<decltype(h.on_number_raw(std::string_view{}))>,
			"on_number_raw must return void or std::expected<void,JsonError>");
		return invoke_handler([&] { return h.on_number_raw(nv.lexeme()); });
	} else {
		if (nv.form() == JsonNumberForm::non_integer) {
			auto d = nv.to_f64();
			if (!d) {
				return std::unexpected(std::move(d).error());
			}
			return invoke_handler([&] { return h.on_double(*d); });
		}
		// Integer form: try std::int64_t → std::uint64_t → f64 (huge integer).
		auto iv = nv.to_i64();
		if (iv) {
			return invoke_handler([&] { return h.on_i64(*iv); });
		}
		auto uv = nv.to_u64();
		if (uv) {
			return invoke_handler([&] { return h.on_u64(*uv); });
		}
		auto dv = nv.to_f64();
		if (!dv) {
			return std::unexpected(std::move(dv).error());
		}
		return invoke_handler([&] { return h.on_double(*dv); });
	}
}
// Decode a JsonStringToken to a callback-scoped string_view without heap allocation
// for the common small escaped-key/value case.
template<class Cb>
[[nodiscard]] std::expected<void, JsonError> dispatch_string_cb(
	JsonStringToken const &tok,
	JsonDecodeScratch &scratch,
	Cb &&cb) {
	if (auto borrow = tok.unescaped_borrow()) {
		return std::forward<Cb>(cb)(*borrow);
	}
	auto const needed = tok.max_decoded_size();
	if (needed <= scratch.string_inline.size()) {
		auto decoded = tok.decode_into(std::span<char>{scratch.string_inline.data(), scratch.string_inline.size()});
		if (!decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		return std::forward<Cb>(cb)(*decoded);
	}
	scratch.string_overflow.resize(needed);
	auto decoded = tok.decode_into(std::span<char>{scratch.string_overflow.data(), scratch.string_overflow.size()});
	if (!decoded) {
		return std::unexpected(std::move(decoded).error());
	}
	return std::forward<Cb>(cb)(*decoded);
}

} // namespace detail

namespace detail {

template<ParseMode Mode, JsonHandler H>
[[nodiscard]] std::expected<void, JsonError> parse_sax(
	std::string_view input,
	H &handler,
	JsonParseOptions const &opts) {
	using Ev = JsonReader::Event;
	JsonReader reader{input, opts};
	JsonDecodeScratch scratch;

	for (;;) {
		auto ev_or = reader.next_impl<Mode>();
		if (!ev_or) {
			return std::unexpected(std::move(ev_or).error());
		}
		if (!*ev_or) {
			break; // EOF
		}

		Ev ev = **ev_or;
		std::expected<void, JsonError> res{};

		switch (ev) {
		case Ev::begin_object: res = detail::invoke_handler([&] { return handler.on_begin_object(); }); break;
		case Ev::end_object  : res = detail::invoke_handler([&] { return handler.on_end_object(); }); break;
		case Ev::begin_array : res = detail::invoke_handler([&] { return handler.on_begin_array(); }); break;
		case Ev::end_array   : res = detail::invoke_handler([&] { return handler.on_end_array(); }); break;
		case Ev::key:
			res = detail::dispatch_string_cb(reader.key_token(), scratch, [&](std::string_view sv) {
				return detail::invoke_handler([&] { return handler.on_key(sv); });
			});
			break;
		case Ev::string_value:
			res = detail::dispatch_string_cb(reader.string_token(), scratch, [&](std::string_view sv) {
				return detail::invoke_handler([&] { return handler.on_string(sv); });
			});
			break;
		case Ev::number_value: res = detail::dispatch_number(handler, reader.number_val()); break;
		case Ev::bool_value  : res = detail::invoke_handler([&] { return handler.on_bool(reader.bool_val()); }); break;
		case Ev::null_value  : res = detail::invoke_handler([&] { return handler.on_null(); }); break;
		}

		if (!res) {
			return std::unexpected(std::move(res).error());
		}
	}
	return {};
}

} // namespace detail

export template<JsonHandler H>
[[nodiscard]] std::expected<void, JsonError> parse_sax(
	std::string_view input,
	H &handler,
	JsonParseOptions const &opts = {}) {
	if (opts.mode == ParseMode::strict) {
		return detail::parse_sax<ParseMode::strict>(input, handler, opts);
	}
	return detail::parse_sax<ParseMode::json5>(input, handler, opts);
}
// ─── Phase 7 — Streaming & NDJSON ───────────────────────────────────────────

export class NdjsonRange {
	std::string_view input_;
	JsonParseOptions opts_;

public:
	explicit NdjsonRange(std::string_view input, JsonParseOptions const &opts = {}) noexcept;
	struct Iterator {
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;
		using value_type = std::expected<Document, JsonError>;
		using difference_type = std::ptrdiff_t;
		using pointer = value_type const *;
		using reference = value_type const &;

	private:
		std::string_view remaining_;
		JsonParseOptions opts_;
		std::optional<value_type> cache_;
		void advance_one() noexcept;

		friend class NdjsonRange;
		Iterator(std::string_view remaining, JsonParseOptions const &opts) noexcept;

	public:
		[[nodiscard]] reference operator *() const noexcept;
		[[nodiscard]] pointer operator ->() const noexcept;
		Iterator &operator ++() noexcept;
		void operator ++(int) noexcept;
		[[nodiscard]] bool operator ==(std::default_sentinel_t) const noexcept;
	};
	[[nodiscard]] Iterator begin() const noexcept;
	[[nodiscard]] std::default_sentinel_t end() const noexcept;
};

} // namespace conflux::json

template<>
inline constexpr bool std::ranges::enable_borrowed_range<conflux::json::NdjsonRange> = true;

namespace conflux::json {

static_assert(std::ranges::borrowed_range<NdjsonRange>);
export class JsonAccumulator {
	std::string buf_;
	JsonParseOptions opts_;

public:
	explicit JsonAccumulator(JsonParseOptions const &opts = {}) noexcept;
	[[nodiscard]] std::expected<void, JsonError> feed(std::string_view chunk);
	[[nodiscard]] std::expected<void, JsonError> feed(std::span<std::byte const> chunk);
	[[nodiscard]] std::expected<Document, JsonError> finish();
	void reset() noexcept;
	[[nodiscard]] std::size_t buffered_bytes() const noexcept;
};
// (reflect codec moved to src/json_reflect.cxx — separate module conflux.json.reflect)

} // namespace conflux::json
