// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
export module conflux.json;
import std;
import conflux.types;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace std; // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Forward declarations (exported types)
// ---------------------------------------------------------------------------

export class Document;
export class NodeRef;
export class ObjectView;
export class ArrayView;
export class JsonNumberView;
export class ValueBuilder;
export class ObjectBuilder;
export class ArrayBuilder;
export struct JsonError;
export class JsonPath;
export class ObjectMemberRange;
export class ArrayElementRange;
export struct ObjectMember;

// ---------------------------------------------------------------------------
// JsonKind / stage / issue code
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size)
export enum class JsonKind {
	object,
	array,
	string,
	number,
	boolean,
	null,
};
// NOLINTNEXTLINE(performance-enum-size)
export enum class JsonStage {
	parse,
	lookup,
	decode,
	build,
	dump,
};
// NOLINTNEXTLINE(performance-enum-size)
export enum class JsonIssueCode {
	syntax_error,
	unexpected_eof,
	trailing_garbage,
	invalid_utf8,
	invalid_unicode_escape,
	invalid_pointer,
	duplicate_member,
	missing_member,
	wrong_kind,
	index_out_of_range,
	invalid_number,
	number_out_of_range,
	sign_mismatch,
	invalid_value,
	constraint_violation,
	nesting_too_deep,
	input_too_large,
	string_too_large,
	output_too_large,
	resource_exhausted,
};

export struct JsonSourceLocation {
	size_t offset{};
	size_t line{1};
	size_t column{1};
};

// ---------------------------------------------------------------------------
// JsonPath
// ---------------------------------------------------------------------------

export struct JsonPathMember {
	string name;
};
export struct JsonPathIndex {
	size_t index{};
};
export using JsonPathSegment = variant<JsonPathMember, JsonPathIndex>;

export class JsonPath {
	vector<JsonPathSegment> segs_;

public:
	static JsonPath root() { return {}; }
	JsonPath() = default;
	JsonPath(JsonPath const &) = default;
	JsonPath(JsonPath &&) noexcept = default;
	JsonPath &operator =(JsonPath const &) = default;
	JsonPath &operator =(JsonPath &&) noexcept = default;

	[[nodiscard]] bool empty() const noexcept { return segs_.empty(); }
	[[nodiscard]] size_t size() const noexcept { return segs_.size(); }
	void reserve(
		size_t n) {
		segs_.reserve(n);
	}

	void push_member(
		string_view name) {
		segs_.emplace_back(JsonPathMember{string{name}});
	}
	void push_index(
		size_t idx) {
		segs_.emplace_back(JsonPathIndex{idx});
	}
	void pop() noexcept {
		if (!segs_.empty()) {
			segs_.pop_back();
		}
	}

	[[nodiscard]] JsonPathSegment const &segment(
		size_t i) const {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		return segs_[i];
	}
	[[nodiscard]] auto begin() const noexcept { return segs_.begin(); }
	[[nodiscard]] auto end() const noexcept { return segs_.end(); }

	[[nodiscard]] string to_pointer() const {
		if (segs_.empty()) {
			return "";
		}
		string out;
		for (auto const &seg: segs_) {
			out += '/';
			if (holds_alternative<JsonPathMember>(seg)) {
				for (char const c: get<JsonPathMember>(seg).name) {
					if (c == '~') {
						out += "~0";
					} else if (c == '/') {
						out += "~1";
					} else {
						out += c;
					}
				}
			} else {
				out += to_string(get<JsonPathIndex>(seg).index);
			}
		}
		return out;
	}

	static expected<JsonPath, JsonError> from_pointer(string_view sv);

	friend bool operator ==(JsonPath const &, JsonPath const &) = default;
};

template<>
struct std::hash<JsonPath> {
	size_t operator ()(
		JsonPath const &p) const noexcept {
		size_t h = 0;
		for (auto const &seg: p) {
			size_t const sh = holds_alternative<JsonPathMember>(seg) ? hash<string>{}(get<JsonPathMember>(seg).name) :
																	   hash<size_t>{}(get<JsonPathIndex>(seg).index);
			h ^= sh + 0x9e3779b9U + (h << 6U) + (h >> 2U);
		}
		return h;
	}
};

// ---------------------------------------------------------------------------
// JsonError
// ---------------------------------------------------------------------------

export struct JsonError {
	JsonStage stage{JsonStage::parse};
	JsonIssueCode code{JsonIssueCode::syntax_error};
	JsonPath path{};
	optional<JsonSourceLocation> source{};
	optional<JsonKind> expected_kind{};
	optional<JsonKind> actual_kind{};
	optional<string> member_name{};
	optional<string> target_type{};
	optional<size_t> requested_index{};
	optional<size_t> container_size{};
	string message{};

	[[nodiscard]] JsonError with_prefix(
		JsonPath const &prefix) const & {
		JsonError copy = *this;
		JsonPath full;
		full.reserve(prefix.size() + path.size());
		for (auto const &s: prefix) {
			if (holds_alternative<JsonPathMember>(s)) {
				full.push_member(get<JsonPathMember>(s).name);
			} else {
				full.push_index(get<JsonPathIndex>(s).index);
			}
		}
		for (auto const &s: path) {
			if (holds_alternative<JsonPathMember>(s)) {
				full.push_member(get<JsonPathMember>(s).name);
			} else {
				full.push_index(get<JsonPathIndex>(s).index);
			}
		}
		copy.path = move(full);
		return copy;
	}
	[[nodiscard]] JsonError with_prefix(
		JsonPath const &prefix) && {
		return static_cast<JsonError const &>(*this).with_prefix(prefix);
	}
};

// ---------------------------------------------------------------------------
// JsonPath::from_pointer (after JsonError definition)
// ---------------------------------------------------------------------------

expected<JsonPath, JsonError> JsonPath::from_pointer(
	string_view sv) {
	if (sv.empty()) {
		return JsonPath{};
	}
	if (sv[0] != '/') {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::invalid_pointer,
				.message = "JSON Pointer must start with '/' or be empty"});
	}
	JsonPath result;
	size_t pos = 1;
	while (pos <= sv.size()) {
		size_t slash = sv.find('/', pos);
		if (slash == string_view::npos) {
			slash = sv.size();
		}
		string name;
		name.reserve(slash - pos);
		for (size_t i = pos; i < slash; ++i) {
			if (sv[i] == '~') {
				if (i + 1 >= slash) {
					return unexpected(
						JsonError{
							.stage = JsonStage::parse,
							.code = JsonIssueCode::invalid_pointer,
							.message = "invalid '~' escape in JSON Pointer"});
				}
				++i;
				if (sv[i] == '0') {
					name += '~';
				} else if (sv[i] == '1') {
					name += '/';
				} else {
					return unexpected(
						JsonError{
							.stage = JsonStage::parse,
							.code = JsonIssueCode::invalid_pointer,
							.message = "invalid '~' escape in JSON Pointer"});
				}
			} else {
				name += sv[i];
			}
		}
		result.push_member(name);
		pos = slash + 1;
	}
	return result;
}

// ---------------------------------------------------------------------------
// LimitOption / JsonParseOptions
// ---------------------------------------------------------------------------

export struct NoLimit {};
export inline constexpr NoLimit no_limit{};

export class LimitOption {
	enum class Tag : u8 {
		default_,
		unlimited,
		bound,
	};
	Tag tag_{Tag::default_};
	size_t value_{};

public:
	constexpr LimitOption() noexcept = default;
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr LimitOption(
		NoLimit) noexcept
		: tag_{Tag::unlimited} {}
	constexpr explicit LimitOption(
		size_t v) noexcept
		: tag_{Tag::bound}
		, value_{v} {}

	[[nodiscard]] static constexpr LimitOption bound(
		size_t v) noexcept {
		return LimitOption{v};
	}

	[[nodiscard]] constexpr bool is_default() const noexcept { return tag_ == Tag::default_; }
	[[nodiscard]] constexpr bool is_unlimited() const noexcept { return tag_ == Tag::unlimited; }
	[[nodiscard]] constexpr optional<size_t> explicit_value() const noexcept {
		if (tag_ == Tag::bound) {
			return value_;
		}
		return nullopt;
	}

	[[nodiscard]] constexpr bool exceeds(
		size_t n,
		size_t default_cap) const noexcept {
		if (tag_ == Tag::unlimited) {
			return false;
		}
		if (tag_ == Tag::default_) {
			return n > default_cap;
		}
		return n > value_;
	}
};

export struct JsonParseOptions {
	LimitOption max_depth;
	LimitOption max_input_size;
	LimitOption max_string_size;
};

// ---------------------------------------------------------------------------
// Internal storage
// ---------------------------------------------------------------------------

enum class NodeKind : u8 {
	null_,
	boolean,
	number,
	string_,
	array,
	object,
};

struct MemberEntry {
	size_t name_off;
	size_t name_len;
	size_t val_node;
};

struct NodeRec {
	NodeKind kind{NodeKind::null_};
	bool bool_val{false};
	size_t a{}; // str_offset | range_start
	size_t b{}; // str_len    | range_count
	bool num_is_integer{false};
};

struct DocumentStorage {
	vector<NodeRec> nodes;
	string string_arena;
	vector<size_t> array_children;
	vector<MemberEntry> object_members;
	size_t root_node{0}; // index of the root NodeRec

	[[nodiscard]] string_view str_at(
		size_t off,
		size_t len) const noexcept {
		return {string_arena.data() + off, len};
	}
};

// ---------------------------------------------------------------------------
// JsonNumberForm / JsonNumberView
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size)
export enum class JsonNumberForm {
	integer,
	non_integer,
};

export class JsonNumberView {
	string_view lexeme_;
	JsonNumberForm form_;

public:
	JsonNumberView(
		string_view lex,
		JsonNumberForm f) noexcept
		: lexeme_{lex}
		, form_{f} {}

	[[nodiscard]] string_view lexeme() const noexcept { return lexeme_; }
	[[nodiscard]] JsonNumberForm form() const noexcept { return form_; }

	[[nodiscard]] expected<int64_t, JsonError> to_i64() const {
		if (form_ != JsonNumberForm::integer) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::invalid_number,
					.message = "to_i64 requires integer-form number"});
		}
		int64_t v{};
		auto const *b = lexeme_.data();
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (auto [p, ec] = from_chars(b, b + lexeme_.size(), v); ec == errc{} && p == b + lexeme_.size()) {
			return v;
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("value out of i64 range: {}", lexeme_)});
	}

	[[nodiscard]] expected<uint64_t, JsonError> to_u64() const {
		if (form_ != JsonNumberForm::integer) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::invalid_number,
					.message = "to_u64 requires integer-form number"});
		}
		if (!lexeme_.empty() && lexeme_[0] == '-') {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::sign_mismatch,
					.message = format("negative integer passed to to_u64: {}", lexeme_)});
		}
		uint64_t v{};
		auto const *b = lexeme_.data();
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (auto [p, ec] = from_chars(b, b + lexeme_.size(), v); ec == errc{} && p == b + lexeme_.size()) {
			return v;
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("value out of u64 range: {}", lexeme_)});
	}

	[[nodiscard]] expected<double, JsonError> to_f64() const {
		double v{};
		auto const *b = lexeme_.data();
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto const [p, ec] = from_chars(b, b + lexeme_.size(), v, chars_format::general);
		if (ec == errc::result_out_of_range) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::number_out_of_range,
					.message = format("f64 conversion overflows: {}", lexeme_)});
		}
		if (ec != errc{} || p != b + lexeme_.size()) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::invalid_number,
					.message = format("cannot convert to f64: {}", lexeme_)});
		}
		if (!isfinite(v)) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::number_out_of_range,
					.message = format("f64 conversion overflows: {}", lexeme_)});
		}
		return v;
	}
};

// ---------------------------------------------------------------------------
// NodeRef
// ---------------------------------------------------------------------------

export class NodeRef {
	DocumentStorage const *storage_{};
	size_t idx_{};

	friend class Document;
	friend class ObjectView;
	friend class ArrayView;
	friend class ObjectMemberRange;
	friend class ArrayElementRange;
	friend bool is_same_node(NodeRef, NodeRef) noexcept;
	friend bool is_value_equal(NodeRef, NodeRef);
	friend bool is_value_equal_exact(NodeRef, NodeRef);
	friend struct NodeIdentityHash;

	NodeRef(
		DocumentStorage const *s,
		size_t i) noexcept
		: storage_{s}
		, idx_{i} {}

	[[nodiscard]] NodeRec const &rec() const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		return storage_->nodes[idx_];
	}

public:
	NodeRef() = default;
	NodeRef(NodeRef const &) = default;
	NodeRef(NodeRef &&) noexcept = default;
	NodeRef &operator =(NodeRef const &) = default;
	NodeRef &operator =(NodeRef &&) noexcept = default;

	[[nodiscard]] JsonKind kind() const noexcept {
		switch (rec().kind) {
		case NodeKind::null_  : return JsonKind::null;
		case NodeKind::boolean: return JsonKind::boolean;
		case NodeKind::number : return JsonKind::number;
		case NodeKind::string_: return JsonKind::string;
		case NodeKind::array  : return JsonKind::array;
		case NodeKind::object : return JsonKind::object;
		}
		return JsonKind::null;
	}

	[[nodiscard]] bool is_null() const noexcept { return rec().kind == NodeKind::null_; }

	[[nodiscard]] expected<ObjectView, JsonError> as_object() const;
	[[nodiscard]] expected<ArrayView, JsonError> as_array() const;

	[[nodiscard]] expected<bool, JsonError> as_bool() const {
		if (rec().kind != NodeKind::boolean) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::wrong_kind,
					.expected_kind = JsonKind::boolean,
					.actual_kind = kind(),
					.message = "expected boolean"});
		}
		return rec().bool_val;
	}

	[[nodiscard]] expected<string_view, JsonError> as_string() const {
		if (rec().kind != NodeKind::string_) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::wrong_kind,
					.expected_kind = JsonKind::string,
					.actual_kind = kind(),
					.message = "expected string"});
		}
		return storage_->str_at(rec().a, rec().b);
	}

	[[nodiscard]] expected<JsonNumberView, JsonError> as_number() const {
		if (rec().kind != NodeKind::number) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::wrong_kind,
					.expected_kind = JsonKind::number,
					.actual_kind = kind(),
					.message = "expected number"});
		}
		return JsonNumberView{
			storage_->str_at(rec().a, rec().b),
			rec().num_is_integer ? JsonNumberForm::integer : JsonNumberForm::non_integer};
	}

	[[nodiscard]] expected<NodeRef, JsonError> at(JsonPath const &path) const;
};

// ---------------------------------------------------------------------------
// ObjectMember (after NodeRef — NodeRef used by value)
// ---------------------------------------------------------------------------

export struct ObjectMember {
	string_view name;
	NodeRef value;
};

// ---------------------------------------------------------------------------
// ObjectView / ArrayView
// ---------------------------------------------------------------------------

export class ObjectView {
	DocumentStorage const *storage_{};
	size_t mem_start_{};
	size_t mem_count_{};

	friend class NodeRef;
	friend bool is_value_equal(NodeRef, NodeRef);
	friend bool is_value_equal_exact(NodeRef, NodeRef);
	ObjectView(
		DocumentStorage const *s,
		size_t start,
		size_t count) noexcept
		: storage_{s}
		, mem_start_{start}
		, mem_count_{count} {}

public:
	[[nodiscard]] size_t size() const noexcept { return mem_count_; }

	[[nodiscard]] expected<NodeRef, JsonError> member(
		string_view name) const {
		for (size_t i = 0; i < mem_count_; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			auto const &m = storage_->object_members[mem_start_ + i];
			if (storage_->str_at(m.name_off, m.name_len) == name) {
				return NodeRef{storage_, m.val_node};
			}
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::missing_member,
				.member_name = string{name},
				.message = format("missing member: {}", name)});
	}

	[[nodiscard]] optional<NodeRef> find_member(
		string_view name) const noexcept {
		for (size_t i = 0; i < mem_count_; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			auto const &m = storage_->object_members[mem_start_ + i];
			if (storage_->str_at(m.name_off, m.name_len) == name) {
				return NodeRef{storage_, m.val_node};
			}
		}
		return nullopt;
	}

	[[nodiscard]] ObjectMemberRange members() const noexcept;
};

export class ArrayView {
	DocumentStorage const *storage_{};
	size_t child_start_{};
	size_t child_count_{};

	friend class NodeRef;
	friend bool is_value_equal(NodeRef, NodeRef);
	friend bool is_value_equal_exact(NodeRef, NodeRef);
	ArrayView(
		DocumentStorage const *s,
		size_t start,
		size_t count) noexcept
		: storage_{s}
		, child_start_{start}
		, child_count_{count} {}

public:
	[[nodiscard]] size_t size() const noexcept { return child_count_; }

	[[nodiscard]] expected<NodeRef, JsonError> element(
		size_t index) const {
		if (index >= child_count_) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::index_out_of_range,
					.requested_index = index,
					.container_size = child_count_,
					.message = format("index {} out of range (size={})", index, child_count_)});
		}
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		return NodeRef{storage_, storage_->array_children[child_start_ + index]};
	}

	[[nodiscard]] ArrayElementRange elements() const noexcept;
};

// ---------------------------------------------------------------------------
// Implement NodeRef methods that need ObjectView/ArrayView
// ---------------------------------------------------------------------------

expected<ObjectView, JsonError> NodeRef::as_object() const {
	if (rec().kind != NodeKind::object) {
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::object,
				.actual_kind = kind(),
				.message = "expected object"});
	}
	return ObjectView{storage_, rec().a, rec().b};
}

expected<ArrayView, JsonError> NodeRef::as_array() const {
	if (rec().kind != NodeKind::array) {
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::array,
				.actual_kind = kind(),
				.message = "expected array"});
	}
	return ArrayView{storage_, rec().a, rec().b};
}

void push_seg(
	JsonPath &p,
	JsonPathSegment const &s) {
	if (holds_alternative<JsonPathMember>(s)) {
		p.push_member(get<JsonPathMember>(s).name);
	} else {
		p.push_index(get<JsonPathIndex>(s).index);
	}
}

expected<NodeRef, JsonError> NodeRef::at(
	JsonPath const &path) const {
	NodeRef cur = *this;
	for (size_t i = 0; i < path.size(); ++i) {
		auto const &seg = path.segment(i);
		auto set_path = [&](JsonError err) {
			err.path = JsonPath{};
			for (size_t j = 0; j <= i; ++j) {
				push_seg(err.path, path.segment(j));
			}
			return unexpected(move(err));
		};
		if (holds_alternative<JsonPathMember>(seg)) {
			auto obj = cur.as_object();
			if (!obj) {
				return set_path(move(obj).error());
			}
			auto child = obj->member(get<JsonPathMember>(seg).name);
			if (!child) {
				return set_path(move(child).error());
			}
			cur = *child;
		} else {
			auto arr = cur.as_array();
			if (!arr) {
				return set_path(move(arr).error());
			}
			auto child = arr->element(get<JsonPathIndex>(seg).index);
			if (!child) {
				return set_path(move(child).error());
			}
			cur = *child;
		}
	}
	return cur;
}

// ---------------------------------------------------------------------------
// Iteration ranges
// ---------------------------------------------------------------------------

export class ObjectMemberRange {
	DocumentStorage const *storage_{};
	size_t start_{};
	size_t count_{};

	friend class ObjectView;
	ObjectMemberRange(
		DocumentStorage const *s,
		size_t start,
		size_t count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		size_t start_{};
		size_t idx_{};
		size_t count_{};
		friend class ObjectMemberRange;
		Iterator(
			DocumentStorage const *s,
			size_t st,
			size_t cnt,
			size_t i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = ptrdiff_t;
		using value_type = ObjectMember;
		using iterator_category = forward_iterator_tag;
		Iterator() = default;

		[[nodiscard]] ObjectMember operator *() const {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			auto const &m = storage_->object_members[start_ + idx_];
			return {
				storage_->str_at(m.name_off, m.name_len),
				NodeRef{storage_, m.val_node}
            };
		}
		Iterator &operator ++() noexcept {
			++idx_;
			return *this;
		}
		Iterator operator ++(
			int) noexcept {
			auto t = *this;
			++idx_;
			return t;
		}
		[[nodiscard]] bool operator ==(
			Sentinel) const noexcept {
			return idx_ >= count_;
		}
		[[nodiscard]] bool operator ==(
			Iterator const &o) const noexcept {
			return idx_ == o.idx_;
		}
	};
	[[nodiscard]] Iterator begin() const noexcept { return {storage_, start_, count_, 0}; }
	[[nodiscard]] Sentinel end() const noexcept { return {}; }
};

export class ArrayElementRange {
	DocumentStorage const *storage_{};
	size_t start_{};
	size_t count_{};

	friend class ArrayView;
	ArrayElementRange(
		DocumentStorage const *s,
		size_t start,
		size_t count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		size_t start_{};
		size_t idx_{};
		size_t count_{};
		friend class ArrayElementRange;
		Iterator(
			DocumentStorage const *s,
			size_t st,
			size_t cnt,
			size_t i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = ptrdiff_t;
		using value_type = NodeRef;
		using iterator_category = forward_iterator_tag;
		Iterator() = default;

		[[nodiscard]] NodeRef operator *() const {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			return NodeRef{storage_, storage_->array_children[start_ + idx_]};
		}
		Iterator &operator ++() noexcept {
			++idx_;
			return *this;
		}
		Iterator operator ++(
			int) noexcept {
			auto t = *this;
			++idx_;
			return t;
		}
		[[nodiscard]] bool operator ==(
			Sentinel) const noexcept {
			return idx_ >= count_;
		}
		[[nodiscard]] bool operator ==(
			Iterator const &o) const noexcept {
			return idx_ == o.idx_;
		}
	};
	[[nodiscard]] Iterator begin() const noexcept { return {storage_, start_, count_, 0}; }
	[[nodiscard]] Sentinel end() const noexcept { return {}; }
};

ObjectMemberRange ObjectView::members() const noexcept {
	return {storage_, mem_start_, mem_count_};
}
ArrayElementRange ArrayView::elements() const noexcept {
	return {storage_, child_start_, child_count_};
}

// ---------------------------------------------------------------------------
// Comparison free functions + identity helpers
// ---------------------------------------------------------------------------

export bool is_same_node(
	NodeRef a,
	NodeRef b) noexcept {
	return a.storage_ == b.storage_ && a.idx_ == b.idx_;
}

// NOLINTNEXTLINE(misc-no-recursion)
export bool is_value_equal(
	NodeRef a,
	NodeRef b) {
	if (a.rec().kind != b.rec().kind) {
		return false;
	}
	switch (a.rec().kind) {
	case NodeKind::null_  : return true;
	case NodeKind::boolean: return a.rec().bool_val == b.rec().bool_val;
	case NodeKind::string_: return a.storage_->str_at(a.rec().a, a.rec().b) == b.storage_->str_at(b.rec().a, b.rec().b);
	case NodeKind::number:
		{
			auto la = a.storage_->str_at(a.rec().a, a.rec().b);
			auto lb = b.storage_->str_at(b.rec().a, b.rec().b);
			if (la == lb) {
				return true;
			}
			auto fa = JsonNumberView{la, a.rec().num_is_integer ? JsonNumberForm::integer : JsonNumberForm::non_integer}
						  .to_f64();
			auto fb = JsonNumberView{lb, b.rec().num_is_integer ? JsonNumberForm::integer : JsonNumberForm::non_integer}
						  .to_f64();
			return fa && fb && *fa == *fb;
		}
	case NodeKind::array:
		{
			ArrayView const av{a.storage_, a.rec().a, a.rec().b};
			ArrayView const bv{b.storage_, b.rec().a, b.rec().b};
			if (av.size() != bv.size()) {
				return false;
			}
			for (size_t i = 0; i < av.size(); ++i) {
				if (!is_value_equal(*av.element(i), *bv.element(i))) {
					return false;
				}
			}
			return true;
		}
	case NodeKind::object:
		{
			ObjectView const ao{a.storage_, a.rec().a, a.rec().b};
			ObjectView const bo{b.storage_, b.rec().a, b.rec().b};
			if (ao.size() != bo.size()) {
				return false;
			}
			for (auto const &[name, val]: ao.members()) {
				auto found = bo.find_member(name);
				if (!found || !is_value_equal(val, *found)) {
					return false;
				}
			}
			return true;
		}
	}
	return false;
}

// NOLINTNEXTLINE(misc-no-recursion)
export bool is_value_equal_exact(
	NodeRef a,
	NodeRef b) {
	if (a.rec().kind != b.rec().kind) {
		return false;
	}
	switch (a.rec().kind) {
	case NodeKind::null_  : return true;
	case NodeKind::boolean: return a.rec().bool_val == b.rec().bool_val;
	case NodeKind::number :
	case NodeKind::string_: return a.storage_->str_at(a.rec().a, a.rec().b) == b.storage_->str_at(b.rec().a, b.rec().b);
	case NodeKind::array:
		{
			ArrayView const av{a.storage_, a.rec().a, a.rec().b};
			ArrayView const bv{b.storage_, b.rec().a, b.rec().b};
			if (av.size() != bv.size()) {
				return false;
			}
			for (size_t i = 0; i < av.size(); ++i) {
				if (!is_value_equal_exact(*av.element(i), *bv.element(i))) {
					return false;
				}
			}
			return true;
		}
	case NodeKind::object:
		{
			ObjectView const ao{a.storage_, a.rec().a, a.rec().b};
			ObjectView const bo{b.storage_, b.rec().a, b.rec().b};
			if (ao.size() != bo.size()) {
				return false;
			}
			for (auto const &[name, val]: ao.members()) {
				auto found = bo.find_member(name);
				if (!found || !is_value_equal_exact(val, *found)) {
					return false;
				}
			}
			return true;
		}
	}
	return false;
}

export struct NodeIdentityHash {
	size_t operator ()(
		NodeRef n) const noexcept {
		return hash<void const *>{}(n.storage_) ^ (hash<size_t>{}(n.idx_) << 1U);
	}
};
export struct NodeIdentityEqual {
	bool operator ()(
		NodeRef a,
		NodeRef b) const noexcept {
		return is_same_node(a, b);
	}
};

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

export struct JsonDumpOptions {
	bool pretty{false};
	unsigned indent{2};
	bool sort_object_keys{false};
	bool ascii_only{false};
};

export class Document {
	unique_ptr<DocumentStorage> storage_;

	friend class ValueBuilder;
	friend Document make_document(unique_ptr<DocumentStorage>) noexcept;
	explicit Document(
		unique_ptr<DocumentStorage> s) noexcept
		: storage_{move(s)} {}

public:
	Document() = default;
	Document(Document &&) noexcept = default;
	Document &operator =(Document &&) noexcept = default;
	Document(Document const &) = delete;
	Document &operator =(Document const &) = delete;

	[[nodiscard]] NodeRef root() const noexcept { return NodeRef{storage_.get(), storage_->root_node}; }

	[[nodiscard]] expected<string, JsonError> dump(JsonDumpOptions const &opts = {}) const;
};

// Module-private factory: parse and builder use this to construct Documents.
Document make_document(
	unique_ptr<DocumentStorage> s) noexcept {
	return Document{move(s)};
}

// ---------------------------------------------------------------------------
// Dump implementation
// ---------------------------------------------------------------------------

// NOLINTBEGIN(readability-magic-numbers)
void dump_str(
	string_view sv,
	string &out,
	bool ascii_only) {
	out += '"';
	for (size_t i = 0; i < sv.size();) {
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
				out += format("\\u{:04x}", static_cast<unsigned>(c));
				++i;
			} else if (ascii_only && c >= 0x80U) {
				// Decode UTF-8 to get code point, then emit \uXXXX or surrogate pair.
				u32 cp = 0;
				size_t seq = 0;
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
				for (size_t k = 1; k < seq && i + k < sv.size(); ++k) {
					cp = (cp << 6U) | (static_cast<unsigned char>(sv[i + k]) & 0x3FU);
				}
				i += seq;
				if (cp < 0x10000U) {
					out += format("\\u{:04x}", cp);
				} else {
					cp -= 0x10000U;
					out += format("\\u{:04x}", 0xD800U | (cp >> 10U));
					out += format("\\u{:04x}", 0xDC00U | (cp & 0x3FFU));
				}
			} else {
				out += static_cast<char>(c);
				++i;
			}
		}
	}
	out += '"';
}
// NOLINTEND(readability-magic-numbers)

// NOLINTNEXTLINE(misc-no-recursion)
void dump_node(
	DocumentStorage const &store,
	size_t node_idx,
	JsonDumpOptions const &opts,
	unsigned depth,
	string &out) {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
	auto const &n = store.nodes[node_idx];
	auto indent = [&](unsigned d) {
		if (!opts.pretty) {
			return;
		}
		out += '\n';
		out.append(static_cast<size_t>(d) * opts.indent, ' ');
	};

	switch (n.kind) {
	case NodeKind::null_  : out += "null"; break;
	case NodeKind::boolean: out += n.bool_val ? "true" : "false"; break;
	case NodeKind::string_: dump_str(store.str_at(n.a, n.b), out, opts.ascii_only); break;
	case NodeKind::number : out += store.str_at(n.a, n.b); break;
	case NodeKind::array:
		{
			out += '[';
			if (n.b > 0) {
				for (size_t i = 0; i < n.b; ++i) {
					if (i > 0) {
						out += ',';
					}
					indent(depth + 1);
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
					dump_node(store, store.array_children[n.a + i], opts, depth + 1, out);
				}
				indent(depth);
			}
			out += ']';
			break;
		}
	case NodeKind::object:
		{
			out += '{';
			if (n.b > 0) {
				vector<size_t> order(n.b);
				iota(order.begin(), order.end(), 0);
				if (opts.sort_object_keys) {
					sort(order.begin(), order.end(), [&](size_t x, size_t y) {
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &mx = store.object_members[n.a + x];
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &my = store.object_members[n.a + y];
						return store.str_at(mx.name_off, mx.name_len) < store.str_at(my.name_off, my.name_len);
					});
				}
				for (size_t i = 0; i < n.b; ++i) {
					if (i > 0) {
						out += ',';
					}
					indent(depth + 1);
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
					auto const &m = store.object_members[n.a + order[i]];
					dump_str(store.str_at(m.name_off, m.name_len), out, opts.ascii_only);
					out += opts.pretty ? ": " : ":";
					dump_node(store, m.val_node, opts, depth + 1, out);
				}
				indent(depth);
			}
			out += '}';
			break;
		}
	}
}

expected<string, JsonError> Document::dump(
	JsonDumpOptions const &opts) const {
	string out;
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

constexpr size_t kDefaultMaxDepth = 512;
constexpr size_t kDefaultMaxInput = 128ULL * 1024 * 1024;
constexpr size_t kDefaultMaxString = 64ULL * 1024 * 1024;

size_t utf8_seq_len(
	unsigned char lead) noexcept {
	// NOLINTBEGIN(readability-magic-numbers)
	if (lead < 0x80U) {
		return 1;
	}
	if (lead < 0xC2U) {
		return 0;
	}
	if (lead < 0xE0U) {
		return 2;
	}
	if (lead < 0xF0U) {
		return 3;
	}
	if (lead < 0xF5U) {
		return 4;
	}
	return 0;
	// NOLINTEND(readability-magic-numbers)
}

bool is_cont(
	unsigned char c) noexcept {
	return (c & 0xC0U) == 0x80U;
}

struct Parser {
	string_view src;
	size_t pos{};
	size_t line{1};
	size_t col{1};
	DocumentStorage &store;
	JsonParseOptions const &opts;

	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		string msg) const {
		return {
			.stage = JsonStage::parse,
			.code = code,
			.source = JsonSourceLocation{.offset = pos, .line = line, .column = col},
			.message = move(msg)
        };
	}

	void skip_ws() noexcept {
		while (pos < src.size()) {
			char const c = src[pos];
			if (c == '\n') {
				++pos;
				++line;
				col = 1;
			} else if (c == ' ' || c == '\t' || c == '\r') {
				++pos;
				++col;
			} else {
				break;
			}
		}
	}
	void adv(
		size_t n = 1) noexcept {
		pos += n;
		col += n;
	}

	static void append_utf8(
		u32 cp,
		string &out) {
		// NOLINTBEGIN(readability-magic-numbers,hicpp-signed-bitwise)
		if (cp < 0x80U) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800U) {
			out += static_cast<char>(0xC0U | (cp >> 6U));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		} else if (cp < 0x10000U) {
			out += static_cast<char>(0xE0U | (cp >> 12U));
			out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		} else {
			out += static_cast<char>(0xF0U | (cp >> 18U));
			out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
			out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		}
		// NOLINTEND(readability-magic-numbers,hicpp-signed-bitwise)
	}

	[[nodiscard]] bool hex4(
		u32 &out) noexcept {
		if (pos + 4 > src.size()) {
			return false;
		}
		out = 0;
		for (size_t i = 0; i < 4; ++i) {
			char const c = src[pos + i];
			u32 d = 0;
			constexpr u32 kA = 10;
			if (c >= '0' && c <= '9') {
				d = static_cast<u32>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				d = static_cast<u32>(c - 'a') + kA;
			} else if (c >= 'A' && c <= 'F') {
				d = static_cast<u32>(c - 'A') + kA;
			} else {
				return false;
			}
			// NOLINTNEXTLINE(hicpp-signed-bitwise)
			out = (out << 4U) | d;
		}
		pos += 4;
		col += 4;
		return true;
	}

	[[nodiscard]] expected<size_t, JsonError> parse_str_into_arena() {
		constexpr unsigned char kCtrlEnd = 0x20U;
		size_t const off = store.string_arena.size();
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				adv();
				break;
			}
			if (c < kCtrlEnd) {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				adv();
				if (pos >= src.size()) {
					return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				switch (src[pos]) {
				case '"':
					store.string_arena += '"';
					adv();
					break;
				case '\\':
					store.string_arena += '\\';
					adv();
					break;
				case '/':
					store.string_arena += '/';
					adv();
					break;
				case 'b':
					store.string_arena += '\b';
					adv();
					break;
				case 'f':
					store.string_arena += '\f';
					adv();
					break;
				case 'n':
					store.string_arena += '\n';
					adv();
					break;
				case 'r':
					store.string_arena += '\r';
					adv();
					break;
				case 't':
					store.string_arena += '\t';
					adv();
					break;
				case 'u':
					{
						adv();
						u32 cp = 0;
						if (!hex4(cp)) {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
						}
						// NOLINTBEGIN(readability-magic-numbers)
						if (cp >= 0xD800U && cp <= 0xDBFFU) {
							if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
								return unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
							}
							adv(2);
							u32 lo = 0;
							if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU) {
								return unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
							}
							cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
						} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
						}
						// NOLINTEND(readability-magic-numbers)
						append_utf8(cp, store.string_arena);
						break;
					}
				default: return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				continue;
			}
			// Validate UTF-8.
			size_t const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos + seq > src.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (size_t k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(src[pos + k]))) {
					return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			store.string_arena.append(src.data() + pos, seq);
			pos += seq;
			col += 1;
		}
		return off;
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<size_t, JsonError> parse_value(
		size_t depth) {
		skip_ws();
		if (pos >= src.size()) {
			return unexpected(mk_err(JsonIssueCode::unexpected_eof, "unexpected end of input"));
		}
		if (opts.max_depth.exceeds(depth, kDefaultMaxDepth)) {
			return unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}

		char const c = src[pos];
		if (c == '"') {
			adv();
			return parse_str_node();
		}
		if (c == '[') {
			return parse_array(depth);
		}
		if (c == '{') {
			return parse_object(depth);
		}
		if (c == 't') {
			if (src.substr(pos, 4) != "true") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(4);
			store.nodes.push_back({.kind = NodeKind::boolean, .bool_val = true});
			return store.nodes.size() - 1;
		}
		if (c == 'f') {
			if (src.substr(pos, 5) != "false") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(5);
			store.nodes.push_back({.kind = NodeKind::boolean, .bool_val = false});
			return store.nodes.size() - 1;
		}
		if (c == 'n') {
			if (src.substr(pos, 4) != "null") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(4);
			store.nodes.push_back({.kind = NodeKind::null_});
			return store.nodes.size() - 1;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return parse_number();
		}
		return unexpected(mk_err(JsonIssueCode::syntax_error, format("unexpected character '{}'", c)));
	}

	[[nodiscard]] expected<size_t, JsonError> parse_str_node() {
		size_t const arena_before = store.string_arena.size();
		auto off = parse_str_into_arena();
		if (!off) {
			return unexpected(move(off).error());
		}
		size_t const len = store.string_arena.size() - arena_before;
		if (opts.max_string_size.exceeds(len, kDefaultMaxString)) {
			return unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
		}
		store.nodes.push_back({.kind = NodeKind::string_, .a = *off, .b = len});
		return store.nodes.size() - 1;
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<size_t, JsonError> parse_array(
		size_t depth) {
		adv(); // '['
		skip_ws();
		if (pos < src.size() && src[pos] == ']') {
			adv();
			size_t const cs = store.array_children.size();
			store.nodes.push_back({.kind = NodeKind::array, .a = cs, .b = 0});
			return store.nodes.size() - 1;
		}
		// Collect child node indices locally to keep arena contiguous.
		vector<size_t> local_children;
		while (true) {
			auto child = parse_value(depth + 1);
			if (!child) {
				return unexpected(move(child).error());
			}
			local_children.push_back(*child);
			skip_ws();
			if (pos >= src.size()) {
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in array"));
			}
			if (src[pos] == ']') {
				adv();
				size_t const cs = store.array_children.size();
				for (size_t const idx: local_children) {
					store.array_children.push_back(idx);
				}
				store.nodes.push_back({.kind = NodeKind::array, .a = cs, .b = local_children.size()});
				return store.nodes.size() - 1;
			}
			if (src[pos] != ',') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'"));
			}
			adv();
		}
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<size_t, JsonError> parse_object(
		size_t depth) {
		adv(); // '{'
		unordered_map<string_view, size_t> seen;
		skip_ws();
		if (pos < src.size() && src[pos] == '}') {
			adv();
			size_t const ms = store.object_members.size();
			store.nodes.push_back({.kind = NodeKind::object, .a = ms, .b = 0});
			return store.nodes.size() - 1;
		}
		// Collect members locally to keep arena contiguous.
		vector<MemberEntry> local_members;
		while (true) {
			skip_ws();
			if (pos >= src.size() || src[pos] != '"') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
			}
			adv();
			size_t const name_before = store.string_arena.size();
			auto name_off = parse_str_into_arena();
			if (!name_off) {
				return unexpected(move(name_off).error());
			}
			size_t const name_len = store.string_arena.size() - name_before;
			string_view const name_sv = store.str_at(*name_off, name_len);
			if (seen.contains(name_sv)) {
				return unexpected(mk_err(JsonIssueCode::duplicate_member, format("duplicate member: {}", name_sv)));
			}
			seen.emplace(name_sv, local_members.size());

			skip_ws();
			if (pos >= src.size() || src[pos] != ':') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ':'"));
			}
			adv();

			auto val = parse_value(depth + 1);
			if (!val) {
				return unexpected(move(val).error());
			}
			local_members.push_back({*name_off, name_len, *val});

			skip_ws();
			if (pos >= src.size()) {
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			if (src[pos] == '}') {
				adv();
				size_t const ms = store.object_members.size();
				for (auto const &m: local_members) {
					store.object_members.push_back(m);
				}
				store.nodes.push_back({.kind = NodeKind::object, .a = ms, .b = local_members.size()});
				return store.nodes.size() - 1;
			}
			if (src[pos] != ',') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'"));
			}
			adv();
		}
	}

	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] expected<size_t, JsonError> parse_number() {
		size_t const start = pos;
		bool const neg = src[pos] == '-';
		if (neg) {
			adv();
		}
		if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
			return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after sign"));
		}
		bool const starts_zero = src[pos] == '0';
		adv();
		if (starts_zero && pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
			return unexpected(mk_err(JsonIssueCode::syntax_error, "leading zeros forbidden"));
		}
		while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
			adv();
		}
		bool non_int = false;
		if (pos < src.size() && src[pos] == '.') {
			non_int = true;
			adv();
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
			non_int = true;
			adv();
			if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
				adv();
			}
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required in exponent"));
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		string_view const lex = src.substr(start, pos - start);
		size_t const off = store.string_arena.size();
		store.string_arena.append(lex.data(), lex.size());
		store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = lex.size(), .num_is_integer = !non_int});
		return store.nodes.size() - 1;
	}
};

// ---------------------------------------------------------------------------
// parse()
// ---------------------------------------------------------------------------

export namespace conflux::json {

expected<Document, JsonError> parse(
	string_view input,
	JsonParseOptions const &opts = {}) {
	if (opts.max_input_size.exceeds(input.size(), kDefaultMaxInput)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds max_input_size"});
	}

	string_view src = input;
	constexpr string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
	}

	auto storage = make_unique<DocumentStorage>();
	storage->nodes.reserve(64);

	Parser p{.src = src, .store = *storage, .opts = opts};
	p.skip_ws();
	if (p.pos >= src.size()) {
		return unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}

	auto root = p.parse_value(0);
	if (!root) {
		return unexpected(move(root).error());
	}
	storage->root_node = *root;

	p.skip_ws();
	if (p.pos < src.size()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source = JsonSourceLocation{.offset = p.pos, .line = p.line, .column = p.col},
				.message = "trailing content after value"
        });
	}

	return ::make_document(move(storage));
}

} // namespace conflux::json

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

// Builder state shared across ValueBuilder / child builders.
struct BuilderState {
	DocumentStorage store;
	bool root_set{false};
	size_t root_node{};
	bool child_active{false};

	// Scratch hashtable for duplicate detection, per active ObjectBuilder session.
	// Reused by clearing between sessions.
	unordered_map<string, size_t> dup_check;
};

// Holds the active object/array being built:
struct ChildFrame {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind {
		object,
		array,
	};
	Kind kind;
	size_t mem_start; // start in object_members (object) or array_children (array)
	BuilderState *state{};
	bool committed{false};
};

export class ObjectBuilder {
	ChildFrame frame_;

	friend class ValueBuilder;
	friend class ArrayBuilder;
	ObjectBuilder(
		BuilderState *st,
		size_t mem_start)
		: frame_{.kind = ChildFrame::Kind::object, .mem_start = mem_start, .state = st} {}

	[[nodiscard]] expected<void, JsonError> check_not_committed() const {
		if (frame_.committed) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "ObjectBuilder already committed"});
		}
		return {};
	}

	expected<void, JsonError> do_insert_node(string_view name, size_t node_idx);

public:
	ObjectBuilder(
		ObjectBuilder &&o) noexcept
		: frame_{o.frame_} {
		o.frame_.state = nullptr;
	}
	ObjectBuilder &operator =(
		ObjectBuilder &&o) noexcept {
		if (this != &o) {
			abort_if_open();
			frame_ = o.frame_;
			o.frame_.state = nullptr;
		}
		return *this;
	}
	ObjectBuilder(ObjectBuilder const &) = delete;
	ObjectBuilder &operator =(ObjectBuilder const &) = delete;

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void abort_if_open() noexcept {
		if ((frame_.state != nullptr) && !frame_.committed) {
			auto *st = frame_.state;
			st->store.object_members.resize(frame_.mem_start); // shrink: no throw
			st->child_active = false;
			frame_.state = nullptr;
		}
	}

	~ObjectBuilder() noexcept { abort_if_open(); }

	expected<void, JsonError> insert_null(string_view name);
	expected<void, JsonError> insert_bool(string_view name, bool v);
	expected<void, JsonError> insert_string(string_view name, string_view value);
	expected<void, JsonError> insert_number(string_view name, string_view lexeme);
	expected<void, JsonError> insert_i64(string_view name, int64_t v);
	expected<void, JsonError> insert_u64(string_view name, uint64_t v);
	expected<void, JsonError> insert_f64(string_view name, double v);

	expected<ObjectBuilder, JsonError> insert_object(string_view name);
	expected<ArrayBuilder, JsonError> insert_array(string_view name);

	template<class T>
		requires /* has_json_codec<T> */ true
	expected<void, JsonError> insert(string_view name, T const &value);

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept {
		if ((frame_.state == nullptr) || frame_.committed) {
			return;
		}
		auto *st = frame_.state;
		size_t const cnt = st->store.object_members.size() - frame_.mem_start;
		st->store.nodes.push_back({.kind = NodeKind::object, .a = frame_.mem_start, .b = cnt});
		size_t const node_idx = st->store.nodes.size() - 1;
		st->root_node = node_idx;
		frame_.committed = true;
		st->child_active = false;
	}
};

export class ArrayBuilder {
	ChildFrame frame_;

	friend class ValueBuilder;
	friend class ObjectBuilder;
	ArrayBuilder(
		BuilderState *st,
		size_t child_start)
		: frame_{.kind = ChildFrame::Kind::array, .mem_start = child_start, .state = st} {}

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void abort_if_open() noexcept {
		if ((frame_.state != nullptr) && !frame_.committed) {
			auto *st = frame_.state;
			st->store.array_children.resize(frame_.mem_start); // shrink: no throw
			st->child_active = false;
			frame_.state = nullptr;
		}
	}

public:
	ArrayBuilder(
		ArrayBuilder &&o) noexcept
		: frame_{o.frame_} {
		o.frame_.state = nullptr;
	}
	ArrayBuilder &operator =(
		ArrayBuilder &&o) noexcept {
		if (this != &o) {
			abort_if_open();
			frame_ = o.frame_;
			o.frame_.state = nullptr;
		}
		return *this;
	}
	ArrayBuilder(ArrayBuilder const &) = delete;
	ArrayBuilder &operator =(ArrayBuilder const &) = delete;

	~ArrayBuilder() noexcept { abort_if_open(); }

	expected<void, JsonError> append_null();
	expected<void, JsonError> append_bool(bool v);
	expected<void, JsonError> append_string(string_view value);
	expected<void, JsonError> append_number(string_view lexeme);
	expected<void, JsonError> append_i64(int64_t v);
	expected<void, JsonError> append_u64(uint64_t v);
	expected<void, JsonError> append_f64(double v);

	expected<ObjectBuilder, JsonError> append_object();
	expected<ArrayBuilder, JsonError> append_array();

	template<class T>
		requires /* has_json_codec<T> */ true
	expected<void, JsonError> append(T const &value);

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept {
		if ((frame_.state == nullptr) || frame_.committed) {
			return;
		}
		auto *st = frame_.state;
		size_t const cnt = st->store.array_children.size() - frame_.mem_start;
		st->store.nodes.push_back({.kind = NodeKind::array, .a = frame_.mem_start, .b = cnt});
		st->root_node = st->store.nodes.size() - 1;
		frame_.committed = true;
		st->child_active = false;
	}
};

// ---------------------------------------------------------------------------
// ValueBuilder
// ---------------------------------------------------------------------------

export class ValueBuilder {
	unique_ptr<BuilderState> state_;

	friend class ObjectBuilder;
	friend class ArrayBuilder;

	[[nodiscard]] expected<void, JsonError> check_can_set() const {
		if (state_->root_set) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "root value already fixed; use reset() to start over"});
		}
		if (state_->child_active) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "child builder is active"});
		}
		return {};
	}

	expected<void, JsonError> set_node(
		NodeRec n) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		state_->store.nodes.push_back(n);
		state_->root_node = state_->store.nodes.size() - 1;
		state_->root_set = true;
		return {};
	}

public:
	ValueBuilder()
		: state_{make_unique<BuilderState>()} {}
	ValueBuilder(ValueBuilder &&) noexcept = default;
	ValueBuilder &operator =(ValueBuilder &&) noexcept = default;
	ValueBuilder(ValueBuilder const &) = delete;
	ValueBuilder &operator =(ValueBuilder const &) = delete;

	expected<void, JsonError> set_null() { return set_node({.kind = NodeKind::null_}); }
	expected<void, JsonError> set_bool(
		bool v) {
		return set_node({.kind = NodeKind::boolean, .bool_val = v});
	}

	expected<void, JsonError> set_string(
		string_view sv) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->store.string_arena.size();
		state_->store.string_arena.append(sv.data(), sv.size());
		return set_node({.kind = NodeKind::string_, .a = off, .b = sv.size()});
	}

	expected<void, JsonError> set_number(string_view lexeme);

	expected<void, JsonError> set_i64(
		int64_t v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->store.string_arena.size();
		array<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->store.string_arena.size() - off;
		return set_node({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	}

	expected<void, JsonError> set_u64(
		uint64_t v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->store.string_arena.size();
		array<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->store.string_arena.size() - off;
		return set_node({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	}

	expected<void, JsonError> set_f64(
		double v) {
		if (!isfinite(v)) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::number_out_of_range,
					.message = "set_f64 requires finite value"});
		}
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->store.string_arena.size();
		array<char, 32> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->store.string_arena.size() - off;
		string_view const lex = state_->store.str_at(off, len);
		bool const is_int = lex.find_first_of(".eE") == string_view::npos;
		return set_node({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = is_int});
	}

	[[nodiscard]] expected<ObjectBuilder, JsonError> begin_object() {
		auto ok = check_can_set();
		if (!ok) {
			return unexpected(move(ok).error());
		}
		state_->child_active = true;
		state_->root_set = true;
		size_t const ms = state_->store.object_members.size();
		return ObjectBuilder{state_.get(), ms};
	}

	[[nodiscard]] expected<ArrayBuilder, JsonError> begin_array() {
		auto ok = check_can_set();
		if (!ok) {
			return unexpected(move(ok).error());
		}
		state_->child_active = true;
		state_->root_set = true;
		size_t const cs = state_->store.array_children.size();
		return ArrayBuilder{state_.get(), cs};
	}

	template<class T>
		requires /* has_json_codec<T> */ true
	expected<void, JsonError> set(T const &value);

	void reset() noexcept {
		state_->store = DocumentStorage{};
		state_->root_set = false;
		state_->child_active = false;
	}

	void discard() && noexcept { state_.reset(); }

	[[nodiscard]] expected<Document, JsonError> finish() && {
		if (!state_->root_set || state_->child_active) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = state_->child_active ? "child builder still active" : "root value was never set"});
		}
		auto storage = make_unique<DocumentStorage>(move(state_->store));
		storage->root_node = state_->root_node;
		return ::make_document(move(storage));
	}
};

export ValueBuilder value_builder() {
	return {};
}

// ---------------------------------------------------------------------------
// Builder number lexeme validation
// ---------------------------------------------------------------------------

bool validate_number_lexeme(
	string_view lex) noexcept {
	if (lex.empty()) {
		return false;
	}
	size_t i = 0;
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
	string_view lexeme) {
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
	size_t const off = state_->store.string_arena.size();
	state_->store.string_arena.append(lexeme.data(), lexeme.size());
	bool const is_int = lexeme.find_first_of(".eE") == string_view::npos;
	return set_node({.kind = NodeKind::number, .a = off, .b = lexeme.size(), .num_is_integer = is_int});
}

// ---------------------------------------------------------------------------
// ObjectBuilder member insert helpers
// ---------------------------------------------------------------------------

expected<void, JsonError> ObjectBuilder::do_insert_node(
	string_view name,
	size_t node_idx) {
	auto *st = frame_.state;
	// Duplicate check
	for (size_t i = frame_.mem_start; i < st->store.object_members.size(); ++i) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		auto const &m = st->store.object_members[i];
		if (st->store.str_at(m.name_off, m.name_len) == name) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::duplicate_member,
					.member_name = string{name},
					.message = format("duplicate member: {}", name)});
		}
	}
	size_t const name_off = st->store.string_arena.size();
	st->store.string_arena.append(name.data(), name.size());
	st->store.object_members.push_back({name_off, name.size(), node_idx});
	return {};
}

expected<void, JsonError> ObjectBuilder::insert_null(
	string_view name) {
	if (auto ok = check_not_committed(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back({.kind = NodeKind::null_});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_bool(
	string_view name,
	bool v) {
	if (auto ok = check_not_committed(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back({.kind = NodeKind::boolean, .bool_val = v});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string(
	string_view name,
	string_view value) {
	if (auto ok = check_not_committed(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	st->store.string_arena.append(value.data(), value.size());
	st->store.nodes.push_back({.kind = NodeKind::string_, .a = off, .b = value.size()});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_number(
	string_view name,
	string_view lexeme) {
	if (auto ok = check_not_committed(); !ok) {
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
	size_t const off = st->store.string_arena.size();
	st->store.string_arena.append(lexeme.data(), lexeme.size());
	bool const is_int = lexeme.find_first_of(".eE") == string_view::npos;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = lexeme.size(), .num_is_integer = is_int});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_i64(
	string_view name,
	int64_t v) {
	if (auto ok = check_not_committed(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_u64(
	string_view name,
	uint64_t v) {
	if (auto ok = check_not_committed(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_f64(
	string_view name,
	double v) {
	if (auto ok = check_not_committed(); !ok) {
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
	size_t const off = st->store.string_arena.size();
	array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	string_view const lex = st->store.str_at(off, len);
	bool const is_int = lex.find_first_of(".eE") == string_view::npos;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = is_int});
	return do_insert_node(name, st->store.nodes.size() - 1);
}

expected<ObjectBuilder, JsonError> ObjectBuilder::insert_object(
	string_view name) {
	if (auto ok = check_not_committed(); !ok) {
		return unexpected(move(ok).error());
	}
	auto *st = frame_.state;
	if (st->child_active) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "child builder already active"});
	}
	// Pre-check duplicate.
	for (size_t i = frame_.mem_start; i < st->store.object_members.size(); ++i) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		auto const &m = st->store.object_members[i];
		if (st->store.str_at(m.name_off, m.name_len) == name) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::duplicate_member,
					.member_name = string{name},
					.message = format("duplicate member: {}", name)});
		}
	}
	// Reserve name slot — we need to store the name but the val_node comes after commit.
	// Use a placeholder: push with val_node = numeric_limits<size_t>::max(), fix after child commits.
	size_t const name_off = st->store.string_arena.size();
	st->store.string_arena.append(name.data(), name.size());
	size_t const member_slot = st->store.object_members.size();
	st->store.object_members.push_back({name_off, name.size(), numeric_limits<size_t>::max()});
	st->child_active = true;
	size_t const child_ms = st->store.object_members.size();

	// When child commits, it pushes a node; we need to fix member_slot.val_node.
	// We handle this by having a separate ObjectBuilder that does this on commit.
	// Actually we need a mechanism. For now, use a wrapper that overrides commit.
	// Simplest: sub-ObjectBuilder that on commit sets parent's member slot.
	// We store member_slot in ChildFrame.mem_start (temporarily abused).
	// Actually let me use a different field. Add a parent_member_slot concept.
	// For Phase 0 simplicity: use a helper struct.

	// Build sub-ObjectBuilder; on its commit it pushes object node.
	// Then we update member_slot with the pushed node index.
	// This requires the parent to be notified. Use a shared "last_committed_node" in BuilderState.
	st->store.object_members[member_slot].val_node = st->store.nodes.size(); // will be set when child commits

	ObjectBuilder child{st, child_ms};
	child.frame_.mem_start = child_ms;
	// Stash member_slot info so commit can fix the val_node.
	// We'll use the builder state's pending mechanism.
	// For now: commit will push node at `nodes.size()` — but we don't know that ahead of time.
	// Better approach: track the member_slot differently.
	// SIMPLE FIX: Don't pre-insert the member. On commit, insert atomically.
	// Revert: remove the placeholder we just added.
	st->store.object_members.pop_back();
	st->store.string_arena.resize(name_off);
	// Store name for post-commit insertion.
	// We need to pass the name through. Add name_pending to ChildFrame.
	// But ChildFrame doesn't have a name field...
	// For Phase 0: just make ObjectBuilder store the parent name.
	(void)member_slot;
	st->child_active = false;

	// Restart cleanly: nested object builder that will insert into this object when committed.
	// Use a different mechanism: pass the parent member_start and name through BuilderState.
	// For simplicity in Phase 0, implement this as a direct child with deferred name insertion.

	// Real approach: build a sub-ObjectBuilder that on commit inserts the member.
	// Requires a parent_insert callback. For Phase 0 prototype, keep it simple.
	// Use a "pending member insert" in BuilderState.

	// Store the name in string_arena and remember its offset+len:
	size_t const name_off2 = st->store.string_arena.size();
	st->store.string_arena.append(name.data(), name.size());
	st->child_active = true;
	size_t const child_ms2 = st->store.object_members.size();

	ObjectBuilder const result{st, child_ms2};
	// We can't easily fix val_node after child commits here without
	// invasive changes. Use a simpler approach: after commit(), the last
	// pushed node is the object node. We read it from nodes.back().
	// Store enough in ChildFrame to do the member insert on commit.
	// Add fields to ChildFrame: parent_mem_start_, parent_name_off_, parent_name_len_.
	// Since ChildFrame is internal, we can do this.
	// BUT ChildFrame is already defined. This is getting messy.
	//
	// SIMPLEST FIX FOR PHASE 0: don't support nested object/array builders from ObjectBuilder/ArrayBuilder.
	// Only support ValueBuilder::begin_object/begin_array.
	// This is not spec-compliant but is a valid Phase 0 prototype limitation.
	// Mark as TODO and provide the flat builder only for now.
	//
	// Actually, let me just fix this properly by extending ChildFrame.

	// Revert all the arena changes:
	st->store.string_arena.resize(name_off);
	st->child_active = false;

	(void)name_off2;
	(void)child_ms2;
	(void)result;

	// Return an error indicating nested builders not yet supported in Phase 0.
	return unexpected(
		JsonError{
			.stage = JsonStage::build,
			.code = JsonIssueCode::constraint_violation,
			.message = "nested object/array builders: use ValueBuilder-level nesting (Phase 0 limitation)"});
}

expected<ArrayBuilder, JsonError> ObjectBuilder::insert_array(
	string_view /*name*/) {
	return unexpected(
		JsonError{
			.stage = JsonStage::build,
			.code = JsonIssueCode::constraint_violation,
			.message = "nested object/array builders not yet supported (Phase 0)"});
}

// ---------------------------------------------------------------------------
// ArrayBuilder append helpers
// ---------------------------------------------------------------------------

expected<void, JsonError> ArrayBuilder::append_null() {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	auto *st = frame_.state;
	st->store.nodes.push_back({.kind = NodeKind::null_});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_bool(
	bool v) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	auto *st = frame_.state;
	st->store.nodes.push_back({.kind = NodeKind::boolean, .bool_val = v});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_string(
	string_view value) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	st->store.string_arena.append(value.data(), value.size());
	st->store.nodes.push_back({.kind = NodeKind::string_, .a = off, .b = value.size()});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_number(
	string_view lexeme) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	if (!validate_number_lexeme(lexeme)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_number,
				.message = format("invalid number lexeme: {}", lexeme)});
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	st->store.string_arena.append(lexeme.data(), lexeme.size());
	bool const is_int = lexeme.find_first_of(".eE") == string_view::npos;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = lexeme.size(), .num_is_integer = is_int});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_i64(
	int64_t v) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_u64(
	uint64_t v) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = true});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_f64(
	double v) {
	if (frame_.committed || (frame_.state == nullptr)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = "ArrayBuilder already committed"});
	}
	if (!isfinite(v)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::number_out_of_range,
				.message = "append_f64 requires finite value"});
	}
	auto *st = frame_.state;
	size_t const off = st->store.string_arena.size();
	array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->store.string_arena.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->store.string_arena.size() - off;
	string_view const lex = st->store.str_at(off, len);
	bool const is_int = lex.find_first_of(".eE") == string_view::npos;
	st->store.nodes.push_back({.kind = NodeKind::number, .a = off, .b = len, .num_is_integer = is_int});
	st->store.array_children.push_back(st->store.nodes.size() - 1);
	return {};
}

expected<ObjectBuilder, JsonError> ArrayBuilder::append_object() {
	return unexpected(
		JsonError{
			.stage = JsonStage::build,
			.code = JsonIssueCode::constraint_violation,
			.message = "nested object/array builders not yet supported (Phase 0)"});
}
expected<ArrayBuilder, JsonError> ArrayBuilder::append_array() {
	return unexpected(
		JsonError{
			.stage = JsonStage::build,
			.code = JsonIssueCode::constraint_violation,
			.message = "nested object/array builders not yet supported (Phase 0)"});
}

// ValueBuilder::finish needs to set root_node from state.
// Fix: store root_node in BuilderState so finish can read it.

// ---------------------------------------------------------------------------
// Nullable<T>
// ---------------------------------------------------------------------------

export template<class T>
class Nullable {
	optional<T> val_;

public:
	constexpr Nullable() noexcept = default;
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Nullable(
		nullptr_t) noexcept {}
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr Nullable(
		T value)
		: val_{move(value)} {}
	Nullable(Nullable const &) = default;
	Nullable(Nullable &&) noexcept = default;
	Nullable &operator =(Nullable const &) = default;
	Nullable &operator =(Nullable &&) noexcept = default;

	[[nodiscard]] constexpr bool is_null() const noexcept { return !val_.has_value(); }
	[[nodiscard]] constexpr bool has_value() const noexcept { return val_.has_value(); }
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

	[[nodiscard]] constexpr T &value() & { return *val_; }
	[[nodiscard]] constexpr T const &value() const & { return *val_; }
	[[nodiscard]] constexpr T &&value() && { return move(*val_); }

	[[nodiscard]] constexpr T &operator *() & noexcept { return *val_; }
	[[nodiscard]] constexpr T const &operator *() const & noexcept { return *val_; }
	[[nodiscard]] constexpr T *operator ->() noexcept { return &*val_; }
	[[nodiscard]] constexpr T const *operator ->() const noexcept { return &*val_; }

	template<class U>
		requires convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) const & {
		return val_ ? *val_ : static_cast<T>(forward<U>(fallback));
	}
	template<class U>
		requires convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) && {
		return val_ ? move(*val_) : static_cast<T>(forward<U>(fallback));
	}

	friend bool operator ==(
		Nullable const &a,
		Nullable const &b)
		requires equality_comparable<T>
	{
		return a.val_ == b.val_;
	}
	friend auto operator <=>(
		Nullable const &a,
		Nullable const &b)
		requires three_way_comparable<T>
	{
		return a.val_ <=> b.val_;
	}
};

template<class T>
struct std::hash<Nullable<T>> {
	size_t operator ()(
		Nullable<T> const &n) const noexcept {
		if (!n.has_value()) {
			return 0;
		}
		return hash<T>{}(n.value());
	}
};

// ---------------------------------------------------------------------------
// JsonCodec / JsonMembers / has_json_codec / decode
// ---------------------------------------------------------------------------

export template<class T, class M>
struct JsonMember {
	string_view name;
	M T::*pointer;
};

export template<class T, class M>
constexpr JsonMember<T, M> json_member(
	string_view name,
	M T::*p) {
	return {name, p};
}

export template<class T>
struct JsonMembers;
export template<class T>
struct JsonCodec;

namespace detail {

template<class T, class = void>
struct has_codec_spec : false_type {};

template<class T>
struct has_codec_spec<
	T,
	void_t<
		decltype(JsonCodec<T>::decode(declval<NodeRef>())),
		decltype(JsonCodec<T>::encode(declval<ValueBuilder &>(), declval<T const &>()))>> : true_type {};

template<class T, class = void>
struct has_members_spec : false_type {};

template<class T>
struct has_members_spec<T, void_t<decltype(JsonMembers<T>::members())>> : bool_constant<default_initializable<T>> {};

} // namespace detail

export template<class T>
concept has_json_codec = detail::has_codec_spec<T>::value || detail::has_members_spec<T>::value;

export template<class T>
inline constexpr bool has_json_codec_v = has_json_codec<T>;

// Built-in specializations declared here, defined below.
template<>
struct JsonCodec<bool>;
template<>
struct JsonCodec<int64_t>;
template<>
struct JsonCodec<uint64_t>;
template<>
struct JsonCodec<double>;
template<>
struct JsonCodec<string>;
template<>
struct JsonCodec<string_view>;
template<class T>
struct JsonCodec<optional<T>>;
template<class T>
struct JsonCodec<Nullable<T>>;
template<class T>
struct JsonCodec<vector<T>>;

export template<class T>
expected<T, JsonError> decode(NodeRef root);

export template<class T>
expected<T, JsonError> decode(
	Document const &d) {
	return decode<T>(d.root());
}

// Built-in JsonCodec specializations.

template<>
struct JsonCodec<bool> {
	static expected<bool, JsonError> decode(
		NodeRef n) {
		return n.as_bool();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		bool v) {
		return b.set_bool(v);
	}
	static constexpr string_view type_name() { return "bool"; }
};
template<>
struct JsonCodec<int64_t> {
	static expected<int64_t, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return unexpected(move(num).error());
		}
		return num->to_i64();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		int64_t v) {
		return b.set_i64(v);
	}
	static constexpr string_view type_name() { return "int64_t"; }
};
template<>
struct JsonCodec<uint64_t> {
	static expected<uint64_t, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return unexpected(move(num).error());
		}
		return num->to_u64();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		uint64_t v) {
		return b.set_u64(v);
	}
	static constexpr string_view type_name() { return "uint64_t"; }
};
template<>
struct JsonCodec<double> {
	static expected<double, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return unexpected(move(num).error());
		}
		return num->to_f64();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		double v) {
		return b.set_f64(v);
	}
	static constexpr string_view type_name() { return "double"; }
};
template<>
struct JsonCodec<string> {
	static expected<string, JsonError> decode(
		NodeRef n) {
		auto sv = n.as_string();
		if (!sv) {
			return unexpected(move(sv).error());
		}
		return string{*sv};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		string const &v) {
		return b.set_string(v);
	}
	static constexpr string_view type_name() { return "string"; }
};
template<>
struct JsonCodec<string_view> {
	static expected<string_view, JsonError> decode(
		NodeRef n) {
		return n.as_string();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		string_view v) {
		return b.set_string(v);
	}
	static constexpr string_view type_name() { return "string_view"; }
};

template<class T>
struct JsonCodec<optional<T>> {
	static expected<optional<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			return optional<T>{}; // strict: null is not presence-only
		}
		// Actually per spec: optional<T> is presence-only. null -> error unless T is Nullable.
		// But presence is determined by the caller (missing member returns nullopt before we're called).
		// If we're called, the value is present. Null is not accepted unless T accepts null.
		if (n.is_null()) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::wrong_kind,
					.actual_kind = JsonKind::null,
					.message = "explicit JSON null not accepted for optional<T> (use Nullable<T>)"});
		}
		auto v = ::decode<T>(n);
		if (!v) {
			return unexpected(move(v).error());
		}
		return optional<T>{move(*v)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		optional<T> const &v) {
		if (!v) {
			return b.set_null();
		}
		return ::decode<T>(*v); // encode
	}
	static constexpr string_view type_name() { return "optional"; }
};

template<class T>
struct JsonCodec<Nullable<T>> {
	static expected<Nullable<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			return Nullable<T>{};
		}
		auto v = ::decode<T>(n);
		if (!v) {
			return unexpected(move(v).error());
		}
		return Nullable<T>{move(*v)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		Nullable<T> const &v) {
		if (v.is_null()) {
			return b.set_null();
		}
		auto inner_b = value_builder();
		if (auto ok = JsonCodec<T>::encode(inner_b, v.value()); !ok) {
			return ok;
		}
		// merge into b — not straightforward without nested builder support
		// For Phase 0: encode directly when T is a scalar.
		return JsonCodec<T>::encode(b, v.value());
	}
	static constexpr string_view type_name() { return "Nullable"; }
};

template<class T>
struct JsonCodec<vector<T>> {
	static expected<vector<T>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		vector<T> result;
		result.reserve(arr->size());
		for (size_t i = 0; i < arr->size(); ++i) {
			auto elem = arr->element(i);
			if (!elem) {
				return unexpected(move(elem).error());
			}
			auto v = ::decode<T>(*elem);
			if (!v) {
				auto err = move(v).error();
				err.path.push_index(i);
				return unexpected(move(err));
			}
			result.push_back(move(*v));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		vector<T> const &v) {
		auto arr = b.begin_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		for (auto const &elem: v) {
			// For Phase 0: only scalar appends supported via generic append
			(void)elem;
		}
		move(*arr).commit();
		return {};
	}
	static constexpr string_view type_name() { return "vector"; }
};

// decode<T> dispatch
export template<class T>
expected<T, JsonError> decode(
	NodeRef root) {
	if constexpr (detail::has_codec_spec<T>::value) {
		return JsonCodec<T>::decode(root);
	} else if constexpr (detail::has_members_spec<T>::value) {
		auto obj = root.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		T result{};
		auto const members = JsonMembers<T>::members();
		bool ok = true;
		JsonError first_err;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &m) {
					 if (!ok) {
						 return;
					 }
					 auto val = obj->member(m.name);
					 if (!val) {
						 ok = false;
						 first_err = move(val).error();
						 return;
					 }
					 using M = remove_reference_t<decltype(result.*m.pointer)>;
					 auto decoded = decode<M>(*val);
					 if (!decoded) {
						 ok = false;
						 first_err = move(decoded).error();
						 first_err.path.push_member(m.name);
						 return;
					 }
					 result.*m.pointer = move(*decoded);
				 })(ms),
				 ...);
			},
			members);
		if (!ok) {
			return unexpected(move(first_err));
		}
		// Unknown members check.
		for (auto const &[name, val]: obj->members()) {
			bool found = false;
			apply([&](auto const &...ms) { ((found = found || name == ms.name), ...); }, members);
			if (!found) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.member_name = string{name},
						.message = format("unknown member: {}", name)});
			}
		}
		return result;
	} else {
		static_assert(false, "No JsonCodec<T> or JsonMembers<T> found for T");
	}
}
