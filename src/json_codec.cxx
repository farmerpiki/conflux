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
template<class K, class Vt>
struct is_map_type<std::map<K, Vt>> : std::true_type {};
template<class T>
constexpr bool is_map_type_v = is_map_type<T>::value;
template<class T>
struct is_unordered_map_type : std::false_type {};
template<class K, class Vt>
struct is_unordered_map_type<std::unordered_map<K, Vt>> : std::true_type {};
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

} // namespace detail

export template<class T>
concept has_json_codec = detail::has_codec_spec<T>::value || detail::has_members_spec<T>::value;

export template<class T>
inline constexpr bool has_json_codec_v = has_json_codec<T>;
namespace detail {

template<class T>
struct direct_writable : std::false_type {};
template<>
struct direct_writable<bool> : std::true_type {};
template<>
struct direct_writable<std::int64_t> : std::true_type {};
template<>
struct direct_writable<std::uint64_t> : std::true_type {};
template<>
struct direct_writable<double> : std::true_type {};
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

export namespace conflux::json {

class ArrayWriter;

class ObjectWriter {
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

template<class F>
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

template<class F>
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

} // namespace conflux::json

export [[nodiscard]] std::expected<Document, JsonError> merge_patch(NodeRef target, NodeRef patch);
export [[nodiscard]] std::expected<Document, JsonError> merge_patch(Document const &target, Document const &patch);
export namespace conflux::json {

enum class JsonPatchOp {
	add,
	remove,
	replace,
	move,
	copy,
	test,
};
struct JsonPatchOptions {
	std::size_t max_operations{1024};
	std::size_t max_pointer_depth{128};
	bool reject_duplicate_object_members{true};
	bool allow_missing_remove{false};
};
[[nodiscard]] std::expected<Document, JsonError> apply_patch(NodeRef target, NodeRef patch, JsonPatchOptions opts = {});
[[nodiscard]] std::expected<Document, JsonError>
apply_patch(Document const &target, Document const &patch, JsonPatchOptions opts = {});
[[nodiscard]] std::expected<void, JsonError> validate_patch(NodeRef patch, JsonPatchOptions opts = {});

} // namespace conflux::json
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
	[[nodiscard]] constexpr T &value() & { return *val_; }
	[[nodiscard]] constexpr T const &value() const & { return *val_; }
	[[nodiscard]] constexpr T &&value() && { return std::move(*val_); }
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
template<class T>
struct std::hash<Nullable<T>> {
	std::size_t operator ()(
		Nullable<T> const &n) const noexcept {
		if (!n.has_value()) {
			return 0;
		}
		return std::hash<T>{}(n.value());
	}
};
// ---------------------------------------------------------------------------
// JsonCodec / JsonMembers / decode
// ---------------------------------------------------------------------------
namespace detail {

template<class T>
concept codec_decodes_node = requires(NodeRef root) {
	{ conflux::json::JsonCodec<T>::decode(root) } -> std::same_as<std::expected<T, JsonError>>;
};

template<class T>
concept codec_decodes_node_with_options = requires(NodeRef root, JsonDecodeOptions const &opts) {
	{ conflux::json::JsonCodec<T>::decode(root, opts) } -> std::same_as<std::expected<T, JsonError>>;
};

template<class T>
concept codec_decodes_reader_event_with_options =
	requires(JsonReader &reader, JsonReader::Event event, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch) {
		{
			conflux::json::JsonCodec<T>::decode(reader, event, opts, scratch)
		} -> std::same_as<std::expected<T, JsonError>>;
	};

template<class T>
concept codec_encodes_value = requires(ValueBuilder &builder, T const &value) {
	{ conflux::json::JsonCodec<T>::encode(builder, value) } -> std::same_as<std::expected<void, JsonError>>;
};

template<class T>
struct has_codec_spec<T>
	: std::bool_constant<codec_encodes_value<T> && (codec_decodes_node<T> || codec_decodes_node_with_options<T>)> {};

template<class T>
std::expected<T, JsonError> decode_codec(
	NodeRef root,
	JsonDecodeOptions const &opts) {
	if constexpr (codec_decodes_node_with_options<T>) {
		return conflux::json::JsonCodec<T>::decode(root, opts);
	} else {
		return conflux::json::JsonCodec<T>::decode(root);
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
struct conflux::json::JsonCodec<bool>;
template<>
struct conflux::json::JsonCodec<std::int64_t>;
template<>
struct conflux::json::JsonCodec<std::uint64_t>;
template<>
struct conflux::json::JsonCodec<double>;
template<class Traits, class Alloc>
struct conflux::json::JsonCodec<std::basic_string<char, Traits, Alloc>>;
template<>
struct conflux::json::JsonCodec<std::string_view>;
template<class T>
struct conflux::json::JsonCodec<std::optional<T>>;
template<class T>
struct conflux::json::JsonCodec<Nullable<T>>;
template<class T, class Alloc>
struct conflux::json::JsonCodec<std::vector<T, Alloc>>;
template<class T, std::size_t N>
struct conflux::json::JsonCodec<std::array<T, N>>;
template<class A, class B>
struct conflux::json::JsonCodec<std::pair<A, B>>;
template<class... Ts>
struct conflux::json::JsonCodec<std::tuple<Ts...>>;
template<class T>
struct conflux::json::JsonCodec<std::map<std::string, T>>;
template<class T>
struct conflux::json::JsonCodec<std::unordered_map<std::string, T>>;

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
struct conflux::json::JsonCodec<bool> {
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
struct conflux::json::JsonCodec<std::int64_t> {
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
struct conflux::json::JsonCodec<std::uint64_t> {
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
struct conflux::json::JsonCodec<double> {
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
struct conflux::json::JsonCodec<std::basic_string<char, Traits, Alloc>> {
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
struct conflux::json::JsonCodec<std::string_view> {
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
struct conflux::json::JsonCodec<std::optional<T>> {
	static std::expected<std::optional<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			if constexpr (detail::is_nullable_type<T>::value) {
				auto v = ::decode<T>(n);
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
		auto v = ::decode<T>(n);
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
		return conflux::json::JsonCodec<T>::encode(b, *v);
	}
	static constexpr std::string_view type_name() { return "Opt"; }
};
template<class T>
struct conflux::json::JsonCodec<Nullable<T>> {
	static std::expected<Nullable<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			return Nullable<T>{};
		}
		auto v = ::decode<T>(n);
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
		return conflux::json::JsonCodec<T>::encode(b, v.value());
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
		auto v = ::decode<T>(*elem);
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
struct conflux::json::JsonCodec<std::vector<T, Alloc>> {
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
struct conflux::json::JsonCodec<std::array<T, N>> {
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
			auto v = ::decode<T>(*elem);
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
struct conflux::json::JsonCodec<std::pair<A, B>> {
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
		auto first = ::decode<A>(*e0);
		if (!first) {
			JsonPath prefix;
			prefix.push_index(0);
			return std::unexpected(std::move(first).error().with_prefix(prefix));
		}
		auto e1 = arr->element(1);
		if (!e1) {
			return std::unexpected(std::move(e1).error());
		}
		auto second = ::decode<B>(*e1);
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
struct conflux::json::JsonCodec<std::tuple<Ts...>> {
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
				 auto v = ::decode<std::tuple_element_t<I, std::tuple<Ts...>>>(*elem);
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
template<class T>
struct conflux::json::JsonCodec<std::map<std::string, T>> {
	static std::expected<std::map<std::string, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		std::map<std::string, T> result;
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
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
		std::map<std::string, T> const &v) {
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
template<class T>
struct conflux::json::JsonCodec<std::unordered_map<std::string, T>> {
	static std::expected<std::unordered_map<std::string, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		std::unordered_map<std::string, T> result;
		result.reserve(obj->size());
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
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
		std::unordered_map<std::string, T> const &v) {
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

[[nodiscard]] inline std::expected<void, JsonError> skip_remaining_reader(
	JsonReader &r,
	JsonReader::Event ev) {
	using Ev = JsonReader::Event;
	if (ev == Ev::string_value || ev == Ev::number_value || ev == Ev::bool_value || ev == Ev::null_value) {
		return {};
	}
	int depth = 1;
	while (depth > 0) {
		auto ne = r.next();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::unexpected_eof,
					.message = "EOF while skipping"});
		}
		if (**ne == Ev::begin_object || **ne == Ev::begin_array) {
			++depth;
		} else if (**ne == Ev::end_object || **ne == Ev::end_array) {
			--depth;
		}
	}
	return {};
}

template<class T>
std::expected<T, JsonError>
decode_from_event(JsonReader &r, JsonReader::Event ev, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch);
template<class T>
std::expected<T, JsonError> decode_from_event(
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts) {
	JsonDecodeScratch scratch;
	return decode_from_event<T>(r, ev, opts, &scratch);
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
[[nodiscard]] inline std::expected<String, JsonError> string_from_token(
	JsonStringToken const &token) {
	String out;
	if (auto borrowed = token.unescaped_borrow()) {
		out.assign(borrowed->data(), borrowed->size());
		return out;
	}
	out.resize(token.max_decoded_size());
	auto res = token.decode_into(std::span<char>{out.data(), out.size()});
	if (!res) {
		return std::unexpected(std::move(res).error());
	}
	out.resize(res->size());
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
	std::uint64_t inline_bits{};
	std::pmr::vector<std::uint64_t> overflow;

	explicit JsonPresenceBits(
		std::pmr::memory_resource *resource)
		: overflow{resource != nullptr ? resource : std::pmr::get_default_resource()} {}

	void reset(
		std::size_t member_count) {
		inline_bits = 0;
		if (member_count > 64) {
			overflow.assign((member_count + 63) / 64, 0);
		} else {
			overflow.clear();
		}
	}

	[[nodiscard]] bool test(
		std::size_t idx) const noexcept {
		if (idx < 64) {
			return (inline_bits & (std::uint64_t{1} << idx)) != 0;
		}
		auto const word = idx / 64;
		auto const bit = idx % 64;
		return word < overflow.size() && (overflow[word] & (std::uint64_t{1} << bit)) != 0;
	}

	void set(
		std::size_t idx) {
		if (idx < 64) {
			inline_bits |= std::uint64_t{1} << idx;
			return;
		}
		auto const word = idx / 64;
		auto const bit = idx % 64;
		if (overflow.size() <= word) {
			overflow.resize(word + 1, 0);
		}
		overflow[word] |= std::uint64_t{1} << bit;
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
	JsonStringToken const &token) {
	if (auto borrowed = token.unescaped_borrow()) {
		out.assign(borrowed->data(), borrowed->size());
		return {};
	}
	out.clear();
	out.resize(token.max_decoded_size());
	auto res = token.decode_into(std::span<char>{out.data(), out.size()});
	if (!res) {
		out.clear();
		return std::unexpected(std::move(res).error());
	}
	out.resize(res->size());
	return {};
}

template<class T>
std::expected<void, JsonError>
decode_into(T &out, JsonReader &r, JsonReader::Event ev, JsonDecodeOptions const &opts, JsonDecodeScratch *scratch);

// Real wide-object measurements showed generated lookup wins from 16 fields
// upward, but an 8-field row regressed when the small path was perturbed.
// Keep <=8 on the pre-existing linear decode shape; benchmark before moving.
inline constexpr std::size_t kJsonMemberLinearLookupLimit = 8;

[[nodiscard]] constexpr std::uint64_t json_member_name_hash(
	std::string_view name) noexcept {
	std::uint64_t h = 1469598103934665603ULL;
	for (char c: name) {
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ULL;
	}
	return h;
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
using JsonMemberDecodeFn = std::expected<void, JsonError> (
		*)(T &, JsonReader &, JsonReader::Event, JsonDecodeOptions const &, JsonDecodeScratch *);

template<class T>
struct JsonMemberLookupEntry {
	std::string_view name{};
	std::uint64_t hash{};
	std::size_t index{};
	JsonMemberDecodeFn<T> decode{};
	bool occupied{};
};

template<class T, std::size_t I>
std::expected<void, JsonError> decode_member_by_static_index(
	T &result,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	constexpr auto members = conflux::json::JsonMembers<T>::members();
	auto const &entry = get<I>(members);
	auto const &m = jm_member(entry);
	using M = std::remove_reference_t<decltype(result.*m.pointer)>;
	auto decoded = decode_into<M>(result.*m.pointer, r, ev, opts, scratch);
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
	constexpr auto members = conflux::json::JsonMembers<T>::members();
	auto const &entry = get<I>(members);
	auto const &m = jm_member(entry);
	return JsonMemberLookupEntry<T>{
		.name = m.name,
		.hash = json_member_name_hash(m.name),
		.index = I,
		.decode = &decode_member_by_static_index<T, I>,
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

template<class DecodeValue>
std::expected<void, JsonError> decode_known_member_value(
	JsonReader &r,
	JsonPresenceBits &presence,
	std::size_t idx,
	std::string_view name,
	DecodeValue &&decode_value) {
	bool const already_found = presence.test(idx);
	if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::reject) {
		return std::unexpected(duplicate_member_error(name));
	}
	auto vne = r.next();
	if (!vne) {
		return std::unexpected(std::move(vne).error());
	}
	if (!*vne) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::unexpected_eof,
				.message = "EOF in object value"});
	}
	if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::first_wins) {
		if (auto skipped = skip_remaining_reader(r, **vne); !skipped) {
			return std::unexpected(std::move(skipped).error());
		}
		return {};
	}
	if (!already_found) {
		presence.set(idx);
	}
	return decode_value(**vne);
}

template<class T>
std::expected<void, JsonError> decode_members_from_event_into(
	T &out,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch);

template<class T>
std::expected<T, JsonError> decode_with_reader(
	JsonReader &r,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch = nullptr) {
	auto ne = r.next();
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
	return decode_from_event<T>(r, **ne, opts, scratch != nullptr ? scratch : &local_scratch);
}

template<class T>
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
	JsonPresenceBits presence{decode_scratch.resource};
	presence.reset(member_count);

	while (ok) {
		auto ne = r.next();
		if (!ne) {
			ok = false;
			first_err = std::move(ne).error();
			break;
		}
		if (!*ne) {
			ok = false;
			first_err = JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::unexpected_eof,
				.message = "EOF in object"};
			break;
		}
		if (**ne == Ev::end_object) {
			break;
		}
		if (**ne != Ev::key) {
			ok = false;
			first_err = JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::syntax_error,
				.message = "std::expected key"};
			break;
		}
		auto key_view_res = key_view_from_token(r.key_token(), decode_scratch);
		if (!key_view_res) {
			ok = false;
			first_err = std::move(key_view_res).error();
			break;
		}
		std::string_view const key_name = *key_view_res;

		bool matched = false;
		if constexpr (member_count > kJsonMemberLinearLookupLimit) {
			if (auto const *entry = find_json_member_lookup_entry<T>(key_name); entry != nullptr) {
				matched = true;
				auto decoded = decode_known_member_value(
					r,
					presence,
					entry->index,
					entry->name,
					[&](JsonReader::Event value_event) {
						return entry->decode(result, r, value_event, opts, &decode_scratch);
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
							 bool const already_found = presence.test(idx);
							 if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::reject) {
								 ok = false;
								 first_err = duplicate_member_error(m.name);
								 ++idx;
								 return;
							 }
							 auto vne = r.next();
							 if (!vne || !*vne) {
								 ok = false;
								 first_err = !vne ? std::move(vne).error() :
													JsonError{
														.stage = JsonStage::decode,
														.code = JsonIssueCode::unexpected_eof,
														.message = "EOF in object value"};
								 ++idx;
								 return;
							 }
							 if (already_found && r.parse_options().duplicate_key == DuplicateKeyPolicy::first_wins) {
								 if (auto skipped = skip_remaining_reader(r, **vne); !skipped) {
									 ok = false;
									 first_err = std::move(skipped).error();
								 }
								 ++idx;
								 return;
							 }
							 if (!already_found) {
								 presence.set(idx);
							 }
							 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
							 auto decoded = decode_into<M>(result.*m.pointer, r, **vne, opts, &decode_scratch);
							 if (!decoded) {
								 ok = false;
								 first_err = std::move(decoded).error();
								 ++idx;
								 return;
							 }
							 auto cfn = jm_constraint(entry);
							 if (cfn != nullptr) {
								 if (auto cr = cfn(result.*m.pointer); !cr) {
									 ok = false;
									 first_err = std::move(cr).error();
									 first_err.member_name = std::string{m.name};
								 }
							 }
						 }
						 ++idx;
					 })(ms),
					 ...);
				},
				members);
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
				auto vne = r.next();
				if (!vne) {
					ok = false;
					first_err = std::move(vne).error();
				} else if (!*vne) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object value"};
				} else if (auto skip_res = skip_remaining_reader(r, **vne); !skip_res) {
					ok = false;
					first_err = std::move(skip_res).error();
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

template<class T>
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
	} else if constexpr (std::same_as<T, std::int64_t>) {
		if (ev != Ev::number_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected number"});
		}
		return r.number_val().to_i64();
	} else if constexpr (std::same_as<T, std::uint64_t>) {
		if (ev != Ev::number_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected number"});
		}
		return r.number_val().to_u64();
	} else if constexpr (std::same_as<T, double>) {
		if (ev != Ev::number_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected number"});
		}
		return r.number_val().to_f64();
	} else if constexpr (is_basic_string_of_char_v<T>) {
		if (ev != Ev::string_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected string"});
		}
		return string_from_token<T>(r.string_token());
	} else if constexpr (std::same_as<T, std::string_view>) {
		static_assert(
			!std::same_as<T, std::string_view>,
			"decode<string_view>(JsonReader&) is deleted; use std::string");
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (ev == Ev::null_value) {
			return T{};
		}
		auto v = decode_from_event<Inner>(r, ev, opts, scratch);
		if (!v) {
			return std::unexpected(std::move(v).error());
		}
		return T{std::move(*v)};
	} else if constexpr (is_nullable_type<T>::value) {
		if (ev == Ev::null_value) {
			return T{};
		}
		using Inner = nullable_inner_t<T>;
		auto v = decode_from_event<Inner>(r, ev, opts, scratch);
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
		while (true) {
			auto ne = r.next();
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
			auto elem = decode_from_event<E>(r, **ne, opts, scratch);
			if (!elem) {
				return std::unexpected(std::move(elem).error());
			}
			result.push_back(std::move(*elem));
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
		for (std::size_t i = 0; i < N; ++i) {
			auto ne = r.next();
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
			auto elem = decode_from_event<E>(r, **ne, opts, scratch);
			if (!elem) {
				return std::unexpected(std::move(elem).error());
			}
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			result[i] = std::move(*elem);
		}
		auto ne = r.next();
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
			auto ne = r.next();
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
			auto v = decode_from_event<FA>(r, **ne, opts, scratch);
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			result.first = std::move(*v);
		}
		{
			auto ne = r.next();
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
			auto v = decode_from_event<FB>(r, **ne, opts, scratch);
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			result.second = std::move(*v);
		}
		{
			auto ne = r.next();
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
				 auto ne = r.next();
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
				 auto v = decode_from_event<E>(r, **ne, opts, scratch);
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
		auto ne = r.next();
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
		using Vt = typename T::mapped_type;
		if (ev != Ev::begin_object) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected object"});
		}
		T result;
		while (true) {
			auto ne = r.next();
			if (!ne) {
				return std::unexpected(std::move(ne).error());
			}
			if (!*ne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object"});
			}
			if (**ne == Ev::end_object) {
				return result;
			}
			if (**ne != Ev::key) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::syntax_error,
						.message = "std::expected key"});
			}
			auto key_res = string_from_token<std::string>(r.key_token());
			if (!key_res) {
				return std::unexpected(std::move(key_res).error());
			}
			std::string key = std::move(*key_res);
			auto vne = r.next();
			if (!vne) {
				return std::unexpected(std::move(vne).error());
			}
			if (!*vne) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object value"});
			}
			auto val = decode_from_event<Vt>(r, **vne, opts, scratch);
			if (!val) {
				return std::unexpected(std::move(val).error());
			}
			result.emplace(std::move(key), std::move(*val));
		}
	} else if constexpr (std::same_as<T, Document>) {
		std::size_t const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader(r, ev); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		std::string_view const slice = r.input().substr(start, r.pos() - start);
		return conflux::json::parse(slice);
	} else if constexpr (has_members_spec<T>::value) {
		T result{};
		if (auto ok = decode_members_from_event_into<T>(result, r, ev, opts, scratch); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		return result;
	} else if constexpr (codec_decodes_reader_event_with_options<T>) {
		return conflux::json::JsonCodec<T>::decode(r, ev, opts, scratch);
	} else if constexpr (has_codec_spec<T>::value) {
		// Generic fallback: re-parse as DOM and delegate to conflux::json::JsonCodec<T>::decode.
		// Used by any type with a custom JsonCodec that has no dedicated streaming branch.
		std::size_t const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader(r, ev); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		std::string_view const slice = r.input().substr(start, r.pos() - start);
		auto doc = conflux::json::parse(slice);
		if (!doc) {
			return std::unexpected(std::move(doc).error());
		}
		return decode_codec<T>(doc->root(), opts);
	} else {
		static_assert(!std::same_as<T, T>, "No JsonReader support for type T");
	}
}

template<class T>
std::expected<void, JsonError> decode_into(
	T &out,
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	if constexpr (is_basic_string_of_char_v<T>) {
		if (ev != Ev::string_value) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.message = "std::expected string"});
		}
		return decode_string_into(out, r.string_token());
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (ev == Ev::null_value) {
			out.reset();
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			out.emplace();
			auto ok = decode_into<Inner>(*out, r, ev, opts, scratch);
			if (!ok) {
				out.reset();
				return std::unexpected(std::move(ok).error());
			}
			return {};
		} else {
			auto decoded = decode_from_event<Inner>(r, ev, opts, scratch);
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
			auto ok = decode_into<Inner>(value, r, ev, opts, scratch);
			if (!ok) {
				out = T{};
				return std::unexpected(std::move(ok).error());
			}
			out = T{std::move(value)};
			return {};
		} else {
			auto decoded = decode_from_event<Inner>(r, ev, opts, scratch);
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
		while (true) {
			auto ne = r.next();
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
				auto decoded = decode_into<E>(slot, r, **ne, opts, scratch);
				if (!decoded) {
					return std::unexpected(std::move(decoded).error());
				}
			} else {
				auto decoded = decode_from_event<E>(r, **ne, opts, scratch);
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
		for (std::size_t i = 0; i < N; ++i) {
			auto ne = r.next();
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
			auto decoded = decode_into<E>(out[i], r, **ne, opts, scratch);
			if (!decoded) {
				return std::unexpected(std::move(decoded).error());
			}
		}
		auto ne = r.next();
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
	} else if constexpr (has_members_spec<T>::value) {
		return decode_members_from_event_into<T>(out, r, ev, opts, scratch);
	} else {
		auto decoded = decode_from_event<T>(r, ev, opts, scratch);
		if (!decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		out = std::move(*decoded);
		return {};
	}
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
	auto value = decode_next<T>(reader, opts);
	if (!value) {
		return std::unexpected(std::move(value).error());
	}
	auto next = reader.next();
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
export template<class T>
std::expected<T, JsonError> decode_full(
	std::string_view input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) {
	JsonReader reader{input, parse_opts};
	return decode_full<T>(reader, decode_opts);
}
export template<class T>
std::expected<T, JsonError> decode_borrowed(
	std::string_view input,
	JsonParseOptions const &parse_opts = {},
	JsonDecodeOptions const &decode_opts = {}) {
	JsonReader reader{input, parse_opts};
	return decode_full<T>(reader, decode_opts);
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
	std::vector<PathFrame> &frames,
	JsonDecodeOptions const &opts) {
	if constexpr (has_codec_spec<T>::value) {
		return decode_codec<T>(root, opts);
	} else if constexpr (has_members_spec<T>::value) {
		auto obj = root.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}
		T result{};
		auto const members = conflux::json::JsonMembers<T>::members();
		bool ok = true;
		JsonError first_err;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &entry) {
					 if (!ok) {
						 return;
					 }
					 auto const &m = jm_member(entry);
					 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
					 auto val = obj->find_member(m.name);
					 if (!val) {
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
					 auto decoded = decode_with_frames<M>(*val, frames, opts);
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
				 })(ms),
				 ...);
			},
			members);
		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		if (opts.unknown_members == UnknownMemberPolicy::reject) {
			for (auto const &[name, val]: obj->members()) {
				(void)val;
				if (!is_json_member_name<T>(name)) {
					return std::unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_value,
							.path = materialize_path(frames),
							.member_name = std::string{name},
							.message = std::format("unknown member: {}", name)});
				}
			}
		}
		return result;
	} else {
		static_assert(false, "No conflux::json::JsonCodec<T> or conflux::json::JsonMembers<T> found for T");
	}
}

} // namespace detail
// decode<T> dispatch
export template<class T>
std::expected<T, JsonError> decode(
	NodeRef root,
	JsonDecodeOptions const &opts) {
	std::vector<detail::PathFrame> frames;
	frames.reserve(16);
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
	// Spec: duplicate-name rejection happens before dispatching to conflux::json::JsonCodec<T>::encode.
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
		return conflux::json::JsonCodec<T>::encode(b, value);
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
		static_assert(false, "No conflux::json::JsonCodec<T> or conflux::json::JsonMembers<T> found for T");
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

inline void direct_append_u_escape(
	std::string &out,
	std::uint32_t cp) {
	static constexpr std::array<char, 16> kHex =
		{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	out += "\\u";
	out += kHex[(cp >> 12U) & 0x0FU];
	out += kHex[(cp >> 8U) & 0x0FU];
	out += kHex[(cp >> 4U) & 0x0FU];
	out += kHex[cp & 0x0FU];
}

inline void direct_dump_string(
	std::string &out,
	std::string_view sv,
	bool ascii_only) {
	out += '"';
	for (std::size_t i = 0; i < sv.size();) {
		auto const c = static_cast<unsigned char>(sv[i]);
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
			} else {
				out += static_cast<char>(c);
				++i;
			}
		}
	}
	out += '"';
}

inline void direct_indent(
	std::string &out,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (!opts.pretty) {
		return;
	}
	out += '\n';
	out.append(static_cast<std::size_t>(depth) * opts.indent, opts.indent_char);
}

template<class T>
std::expected<void, JsonError>
direct_write_value(std::string &out, T const &value, JsonDumpOptions const &opts, unsigned depth);

template<class T>
std::expected<void, JsonError> direct_write_array_like(
	std::string &out,
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

template<class T>
std::expected<void, JsonError> direct_write_members(
	std::string &out,
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

template<class T>
std::expected<void, JsonError> direct_write_value(
	std::string &out,
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
	} else if constexpr (has_members_spec<Raw>::value) {
		return direct_write_members(out, value, opts, depth);
	} else {
		static_assert(!std::same_as<Raw, Raw>, "No direct JSON writer support for type");
	}
}

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
	if constexpr (detail::has_members_spec<std::remove_cvref_t<T>>::value) {
		out.reserve(
			std::tuple_size_v<
				std::remove_cvref_t<decltype(conflux::json::JsonMembers<std::remove_cvref_t<T>>::members())>>
			* 16);
	}
	if (auto ok = write_json_direct(out, value, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return out;
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
// Decode a JsonStringToken to string_view or std::string, call cb(std::string_view).
template<class Cb>
[[nodiscard]] std::expected<void, JsonError> dispatch_string_cb(
	JsonStringToken const &tok,
	Cb &&cb) {
	if (auto borrow = tok.unescaped_borrow()) {
		return std::forward<Cb>(cb)(*borrow);
	}
	std::string buf;
	buf.reserve(tok.max_decoded_size());
	auto r = tok.append_decoded_to(buf);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return std::forward<Cb>(cb)(std::string_view{buf});
}

} // namespace detail
export template<JsonHandler H>
[[nodiscard]] std::expected<void, JsonError> parse_sax(
	std::string_view input,
	H &handler,
	JsonParseOptions const &opts = {}) {
	using Ev = JsonReader::Event;
	JsonReader reader{input, opts};

	for (;;) {
		auto ev_or = reader.next();
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
			res = detail::dispatch_string_cb(reader.key_token(), [&](std::string_view sv) {
				return detail::invoke_handler([&] { return handler.on_key(sv); });
			});
			break;
		case Ev::string_value:
			res = detail::dispatch_string_cb(reader.string_token(), [&](std::string_view sv) {
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
// ─── Phase 7 — Streaming & NDJSON ───────────────────────────────────────────

export class NdjsonRange {
	std::string_view input_;
	JsonParseOptions opts_;

public:
	explicit NdjsonRange(std::string_view input, JsonParseOptions const &opts = {}) noexcept;
	struct Iterator {
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
