// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
export module conflux.json;
import std;
import conflux.types;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
using namespace std; // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

export class Value;
export class Document;

// ---------------------------------------------------------------------------
// JsonType
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size)
export enum class JsonType : u8 {
	Null,
	Bool,
	Int,
	Uint,
	Float,
	String,
	Array,
	Object,
};

// ---------------------------------------------------------------------------
// ParseError
// ---------------------------------------------------------------------------

export struct ParseError {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Code : u8 {
		UnexpectedChar,
		UnexpectedEof,
		InvalidEscape,
		InvalidUnicode,
		NumberOverflow,
		NestingTooDeep,
		TrailingContent,
	} code{};
	size_t offset{};
	u32 line{1};
	u32 column{1};

	[[nodiscard]] string message() const {
		static constexpr array<string_view, 7> kNames{
			"unexpected character",
			"unexpected end of input",
			"invalid escape sequence",
			"invalid unicode escape",
			"number overflow",
			"nesting too deep",
			"trailing content after value",
		};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		return format("{}:{}: {}", line, column, kNames[static_cast<u8>(code)]);
	}
};

// ---------------------------------------------------------------------------
// Value
//
// Variant alternatives (index → JsonType):
//   0: monostate   → Null
//   1: bool        → Bool
//   2: i64     → Int
//   3: u64    → Uint
//   4: double      → Float
//   5: string_view → String  (zero-copy: points into caller's buffer or Document source)
//   6: string      → String  (owned copy: builder mode)
//   7: JArray      → Array   (shared_ptr)
//   8: JObject     → Object  (shared_ptr)
//
// Object keys are always std::string (unconditional lifetime).
// String values: string_view in borrow-mode parse; string in own-mode or builder.
//
// Borrow-mode escape: a Value returned by parse_borrowed() (or any sub-Value
// reached from it) is only valid while the parent Document is alive.  If a
// caller needs the Value to outlive the Document, call Value::promote_to_owned()
// (in-place) or Document::clone_owned() (returns a detached copy).
// ---------------------------------------------------------------------------

using JArray = shared_ptr<vector<Value>>;
using JObject = shared_ptr<vector<pair<string, Value>>>;

export class Value {
	using Var = variant<monostate, bool, i64, u64, double, string_view, string, JArray, JObject>;

	static constexpr array<JsonType, 9> kTypeMap{
		JsonType::Null,
		JsonType::Bool,
		JsonType::Int,
		JsonType::Uint,
		JsonType::Float,
		JsonType::String,
		JsonType::String,
		JsonType::Array,
		JsonType::Object,
	};

	Var v_{};

	// Private helpers to reduce get<>() cognitive complexity.
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] optional<i64> get_i64() const noexcept {
		if (auto const *p = get_if<i64>(&v_)) {
			return *p;
		}
		if (auto const *p = get_if<u64>(&v_); p != nullptr && *p <= static_cast<u64>(numeric_limits<i64>::max())) {
			return static_cast<i64>(*p);
		}
		return nullopt;
	}
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] optional<u64> get_u64() const noexcept {
		if (auto const *p = get_if<u64>(&v_)) {
			return *p;
		}
		if (auto const *p = get_if<i64>(&v_); p != nullptr && *p >= 0) {
			return static_cast<u64>(*p);
		}
		return nullopt;
	}
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] optional<double> get_f64() const noexcept {
		if (auto const *p = get_if<double>(&v_)) {
			return *p;
		}
		if (auto const *p = get_if<i64>(&v_)) {
			return static_cast<double>(*p);
		}
		if (auto const *p = get_if<u64>(&v_)) {
			return static_cast<double>(*p);
		}
		return nullopt;
	}

public:
	Value() = default;

	// Implicit scalar/string conversions by design — callers write
	//   Value v = 42; Value v = "hi"; Value v = true;
	// Container alternatives (JArray/JObject) stay explicit; see below.
	// NOLINTBEGIN(google-explicit-constructor,hicpp-explicit-conversions)
	Value(
		monostate /*tag*/)
		: v_{monostate{}} {}
	Value(
		nullptr_t /*tag*/)
		: v_{monostate{}} {}
	Value(
		bool b)
		: v_{b} {}

	// Any non-bool integer: signed → i64, unsigned → u64.
	template<typename I>
		requires integral<I> && (!same_as<remove_cv_t<I>, bool>)
	Value(
		I i) {
		if constexpr (signed_integral<I>) {
			v_ = static_cast<i64>(i);
		} else {
			v_ = static_cast<u64>(i);
		}
	}

	template<typename F>
		requires floating_point<F>
	Value(
		F f)
		: v_{static_cast<double>(f)} {}

	Value(
		char const *s)
		: v_{string_view{s}} {}
	Value(
		string_view sv)
		: v_{sv} {}
	Value(
		string s)
		: v_{std::move(s)} {}
	// NOLINTEND(google-explicit-constructor,hicpp-explicit-conversions)
	explicit Value(
		JArray a)
		: v_{std::move(a)} {}
	explicit Value(
		JObject o)
		: v_{std::move(o)} {}

	// Type queries
	[[nodiscard]] JsonType type() const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		return kTypeMap[v_.index()];
	}
	[[nodiscard]] bool is_null() const noexcept { return v_.index() == 0; }
	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	[[nodiscard]] bool is_bool() const noexcept { return holds_alternative<bool>(v_); }
	[[nodiscard]] bool is_int() const noexcept { return holds_alternative<i64>(v_); }
	[[nodiscard]] bool is_uint() const noexcept { return holds_alternative<u64>(v_); }
	[[nodiscard]] bool is_float() const noexcept { return holds_alternative<double>(v_); }
	// NOLINTEND(readability-convert-member-functions-to-static)
	[[nodiscard]] bool is_string() const noexcept {
		return holds_alternative<string_view>(v_) || holds_alternative<string>(v_);
	}
	[[nodiscard]] bool is_array() const noexcept { return holds_alternative<JArray>(v_); }
	[[nodiscard]] bool is_object() const noexcept { return holds_alternative<JObject>(v_); }
	[[nodiscard]] bool is_number() const noexcept { return is_int() || is_uint() || is_float(); }

	// Typed access — optional.  String variants can return nullopt on alloc failure
	// (get<string> copies); all integer/bool/float variants are truly noexcept.
	// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-branch-clone)
	template<typename T>
	[[nodiscard]] optional<T> get() const noexcept(
		noexcept(optional<T>{})) {
		if constexpr (same_as<T, bool>) {
			if (auto const *p = get_if<bool>(&v_)) {
				return *p;
			}
		} else if constexpr (same_as<T, i64>) {
			return get_i64();
		} else if constexpr (same_as<T, u64>) {
			return get_u64();
		} else if constexpr (same_as<T, double>) {
			return get_f64();
		} else if constexpr (same_as<T, string_view>) {
			if (auto const *p = get_if<string_view>(&v_)) {
				return *p;
			}
			if (auto const *p = get_if<string>(&v_)) {
				return string_view{*p};
			}
		} else if constexpr (same_as<T, string>) {
			if (auto const *p = get_if<string_view>(&v_)) {
				return string{*p};
			}
			if (auto const *p = get_if<string>(&v_)) {
				return *p;
			}
		}
		return nullopt;
	}
	// NOLINTEND(readability-function-cognitive-complexity,bugprone-branch-clone)

	// Typed access — throws invalid_argument on type mismatch.
	template<typename T>
	[[nodiscard]] T as() const {
		auto r = get<T>();
		if (!r) {
			throw invalid_argument{format("json::Value::as: type mismatch (have {})", static_cast<int>(type()))};
		}
		return *r;
	}

	// get_or: value of key in object (child), or default.
	template<typename T>
	[[nodiscard]] T get_or(
		string_view key,
		T def) const
		noexcept(
			noexcept(optional<T>{})) {
		auto r = (*this)[key].get<T>();
		return r ? *r : def;
	}

	// Object key lookup — null Value if key absent or not an object.
	// Returns by value: cheap (variant + refcount bump for containers).
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	[[nodiscard]] Value operator [](
		string_view key) const {
		if (auto const *p = get_if<JObject>(&v_); p != nullptr && static_cast<bool>(*p)) {
			for (auto const &[k, v]: **p) {
				if (k == key) {
					return v;
				}
			}
		}
		return {};
	}

	// Array index — null Value if out of bounds or not an array.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	[[nodiscard]] Value operator [](
		size_t idx) const {
		if (auto const *p = get_if<JArray>(&v_); p != nullptr && static_cast<bool>(*p) && idx < (*p)->size()) {
			return (**p)[idx];
		}
		return {};
	}

	// Mutating object access — null upgrades to object; inserts null on miss.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	Value &operator [](
		string_view key) {
		if (is_null()) {
			v_ = make_shared<vector<pair<string, Value>>>();
		}
		auto *p = get_if<JObject>(&v_);
		if (p == nullptr || !static_cast<bool>(*p)) {
			throw invalid_argument{"json::Value::operator[]: not an object"};
		}
		for (auto &[k, v]: **p) {
			if (k == key) {
				return v;
			}
		}
		(*p)->emplace_back(string{key}, Value{});
		return (*p)->back().second;
	}

	// Mutating array access — index must be in bounds.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	Value &operator [](
		size_t idx) {
		auto *p = get_if<JArray>(&v_);
		if (p == nullptr || !static_cast<bool>(*p)) {
			throw invalid_argument{"json::Value::operator[]: not an array"};
		}
		if (idx >= (*p)->size()) {
			throw out_of_range{"json::Value::operator[]: index out of bounds"};
		}
		return (**p)[idx];
	}

	// Bulk read views.
	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	[[nodiscard]] span<Value const> as_array() const noexcept {
		if (auto const *p = get_if<JArray>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return span<Value const>((*p)->data(), (*p)->size());
		}
		return {};
	}
	[[nodiscard]] span<pair<string, Value> const> as_object() const noexcept {
		if (auto const *p = get_if<JObject>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return span<pair<string, Value> const>((*p)->data(), (*p)->size());
		}
		return {};
	}

	// Mutating views — empty span if type mismatches.
	[[nodiscard]] span<Value> as_array_mut() noexcept {
		if (auto const *p = get_if<JArray>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return span<Value>((*p)->data(), (*p)->size());
		}
		return {};
	}
	[[nodiscard]] span<pair<string, Value>> as_object_mut() noexcept {
		if (auto const *p = get_if<JObject>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return span<pair<string, Value>>((*p)->data(), (*p)->size());
		}
		return {};
	}

	[[nodiscard]] size_t size() const noexcept {
		if (auto const *p = get_if<JArray>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return (*p)->size();
		}
		if (auto const *p = get_if<JObject>(&v_); p != nullptr && static_cast<bool>(*p)) {
			return (*p)->size();
		}
		return 0;
	}
	[[nodiscard]] bool empty() const noexcept { return size() == 0; }
	// NOLINTEND(readability-convert-member-functions-to-static)

	// Mutable container access for json::object_set / json::array_push.
	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	[[nodiscard]] JArray shared_array() const noexcept {
		if (auto const *p = get_if<JArray>(&v_)) {
			return *p;
		}
		return {};
	}
	[[nodiscard]] JObject shared_object() const noexcept {
		if (auto const *p = get_if<JObject>(&v_)) {
			return *p;
		}
		return {};
	}
	// NOLINTEND(readability-convert-member-functions-to-static)

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	void dump(string &out) const;
	[[nodiscard]] string dump() const {
		string s;
		dump(s);
		return s;
	}
	// NOLINTEND(readability-convert-member-functions-to-static)

	// Append to array — null upgrades to array.
	Value &push_back(
		Value v) {
		if (is_null()) {
			v_ = make_shared<vector<Value>>();
		}
		auto *p = get_if<JArray>(&v_);
		if (p == nullptr || !static_cast<bool>(*p)) {
			throw invalid_argument{"json::Value::push_back: not an array"};
		}
		(*p)->push_back(std::move(v));
		return (*p)->back();
	}

	// Set or replace object member — null upgrades to object.
	Value &set(
		string key,
		Value v) {
		if (is_null()) {
			v_ = make_shared<vector<pair<string, Value>>>();
		}
		auto *p = get_if<JObject>(&v_);
		if (p == nullptr || !static_cast<bool>(*p)) {
			throw invalid_argument{"json::Value::set: not an object"};
		}
		for (auto &[k, val]: **p) {
			if (k == key) {
				val = std::move(v);
				return val;
			}
		}
		(*p)->emplace_back(std::move(key), std::move(v));
		return (*p)->back().second;
	}

	void erase(
		string_view key) {
		auto *p = get_if<JObject>(&v_);
		if (p == nullptr || !static_cast<bool>(*p)) {
			return;
		}
		auto &vec = **p;
		vec.erase(ranges::remove_if(vec, [&](auto const &kv) { return kv.first == key; }).begin(), vec.end());
	}

	[[nodiscard]] bool contains(
		string_view key) const noexcept {
		if (auto const *p = get_if<JObject>(&v_); p != nullptr && static_cast<bool>(*p)) {
			for (auto const &[k, _]: **p) {
				if (k == key) {
					return true;
				}
			}
		}
		return false;
	}

	// Deep copy — recursive.  Arrays/objects get fresh shared_ptrs; string_view
	// values promote to owned string so the clone is self-contained.
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] Value clone() const {
		return visit(
			// NOLINTNEXTLINE(misc-no-recursion)
			[]<typename T>(T const &v) -> Value {
				if constexpr (same_as<T, JArray>) {
					if (!v) {
						return Value{JArray{}};
					}
					auto copy = make_shared<vector<Value>>();
					copy->reserve(v->size());
					for (auto const &elem: *v) {
						copy->push_back(elem.clone());
					}
					return Value{std::move(copy)};
				} else if constexpr (same_as<T, JObject>) {
					if (!v) {
						return Value{JObject{}};
					}
					auto copy = make_shared<vector<pair<string, Value>>>();
					copy->reserve(v->size());
					for (auto const &[k, val]: *v) {
						copy->emplace_back(k, val.clone());
					}
					return Value{std::move(copy)};
				} else if constexpr (same_as<T, string_view>) {
					return Value{string{v}};
				} else {
					return Value{v};
				}
			},
			v_);
	}

	// In-place promotion for borrow-mode escape: rewrites every string_view
	// leaf to an owned string and detaches shared containers, so *this can
	// outlive the Document that originally backed it.
	void promote_to_owned() { *this = clone(); }

	// Content-aware equality: containers compared recursively (not by
	// shared_ptr identity); string_view and string compare by text so values
	// from borrow-mode and own-mode parses can be compared.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator,misc-no-recursion,readability-function-cognitive-complexity,bugprone-exception-escape)
	bool operator ==(
		Value const &o) const noexcept {
		return visit(
			// NOLINTNEXTLINE(misc-no-recursion)
			[]<typename A, typename B>(A const &a, B const &b) -> bool {
				if constexpr (same_as<A, B>) {
					if constexpr (same_as<A, JArray>) {
						if (!a || !b) {
							return !a && !b;
						}
						if (a->size() != b->size()) {
							return false;
						}
						for (size_t i = 0; i < a->size(); ++i) {
							if (!((*a)[i] == (*b)[i])) {
								return false;
							}
						}
						return true;
					} else if constexpr (same_as<A, JObject>) {
						if (!a || !b) {
							return !a && !b;
						}
						if (a->size() != b->size()) {
							return false;
						}
						for (size_t i = 0; i < a->size(); ++i) {
							if ((*a)[i].first != (*b)[i].first) {
								return false;
							}
							if (!((*a)[i].second == (*b)[i].second)) {
								return false;
							}
						}
						return true;
					} else {
						return a == b;
					}
				} else if constexpr (
					(same_as<A, string> || same_as<A, string_view>)
					&& (same_as<B, string> || same_as<B, string_view>)) {
					return string_view{a} == string_view{b};
				} else {
					return false;
				}
			},
			v_,
			o.v_);
	}
};

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

export class Document {
	Value root_{};
	shared_ptr<string const> backing_{};

public:
	Document() = default;
	~Document() = default;
	Document(Document &&) = default;
	Document &operator =(Document &&) = default;
	Document(Document const &) = delete;
	Document &operator =(Document const &) = delete;

	// Forwarding accessors to root_.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	[[nodiscard]] Value operator [](
		string_view key) const {
		return root_[key];
	}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	[[nodiscard]] Value operator [](
		size_t idx) const {
		return root_[idx];
	}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	Value &operator [](
		string_view key) {
		return root_[key];
	}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	Value &operator [](
		size_t idx) {
		return root_[idx];
	}

	[[nodiscard]] bool is_null() const noexcept { return root_.is_null(); }
	[[nodiscard]] bool is_bool() const noexcept { return root_.is_bool(); }
	[[nodiscard]] bool is_int() const noexcept { return root_.is_int(); }
	[[nodiscard]] bool is_uint() const noexcept { return root_.is_uint(); }
	[[nodiscard]] bool is_float() const noexcept { return root_.is_float(); }
	[[nodiscard]] bool is_number() const noexcept { return root_.is_number(); }
	[[nodiscard]] bool is_string() const noexcept { return root_.is_string(); }
	[[nodiscard]] bool is_array() const noexcept { return root_.is_array(); }
	[[nodiscard]] bool is_object() const noexcept { return root_.is_object(); }
	[[nodiscard]] JsonType type() const noexcept { return root_.type(); }

	template<typename T>
	[[nodiscard]] optional<T> get() const noexcept(
		noexcept(optional<T>{})) {
		return root_.get<T>();
	}
	template<typename T>
	[[nodiscard]] T as() const {
		return root_.as<T>();
	}
	template<typename T>
	[[nodiscard]] T get_or(
		string_view key,
		T def) const
		noexcept(
			noexcept(optional<T>{})) {
		return root_.get_or(key, def);
	}

	[[nodiscard]] span<Value const> as_array() const noexcept { return root_.as_array(); }
	[[nodiscard]] span<pair<string, Value> const> as_object() const noexcept { return root_.as_object(); }
	[[nodiscard]] size_t size() const noexcept { return root_.size(); }
	[[nodiscard]] bool empty() const noexcept { return root_.empty(); }

	void dump(
		string &out) const {
		root_.dump(out);
	}
	[[nodiscard]] string dump() const { return root_.dump(); }

	[[nodiscard]] Value const &root() const noexcept { return root_; }

	// Self-contained deep copy of the root.  Returned Value has no dependency
	// on this Document's backing storage and is safe to outlive it.
	[[nodiscard]] Value clone_owned() const { return root_.clone(); }
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	bool operator ==(
		Document const &o) const noexcept {
		return root_ == o.root_;
	}

	[[nodiscard]] bool has_backing_storage() const noexcept { return static_cast<bool>(backing_); }

	void set_root(
		Value v) {
		root_ = std::move(v);
	}
	void set_backing(
		shared_ptr<string const> backing) {
		backing_ = std::move(backing);
	}
};

// ---------------------------------------------------------------------------
// dump() implementation
// ---------------------------------------------------------------------------

namespace {

void dump_str(
	string_view sv,
	string &out) {
	out.reserve(out.size() + sv.size() + 2);
	out += '"';
	for (char const ch_: sv) {
		auto const c = static_cast<unsigned char>(ch_);
		switch (c) {
		case '"' : out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += format("\\u{:04x}", static_cast<unsigned>(c));
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-convert-member-functions-to-static)
void Value::dump(
	string &out) const {
	visit(
		// NOLINTNEXTLINE(readability-function-cognitive-complexity)
		[&]<typename T>(T const &v) {
			if constexpr (same_as<T, monostate>) {
				out += "null";
			} else if constexpr (same_as<T, bool>) {
				out += v ? "true" : "false";
			} else if constexpr (same_as<T, i64> || same_as<T, u64>) {
				array<char, 22> buf{};
				// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
				auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
				out.append(buf.data(), p);
			} else if constexpr (same_as<T, double>) {
				if (!isfinite(v)) {
					out += "null"; // NaN/Infinity are not valid JSON; serialize as null
				} else {
					array<char, 32> buf{};
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
					auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
					out.append(buf.data(), p);
				}
			} else if constexpr (same_as<T, string_view> || same_as<T, string>) {
				dump_str(string_view{v}, out);
			} else if constexpr (same_as<T, JArray>) {
				out += '[';
				if (v) {
					bool first = true;
					for (auto const &elem: *v) {
						if (!first) {
							out += ',';
						}
						first = false;
						elem.dump(out);
					}
				}
				out += ']';
			} else if constexpr (same_as<T, JObject>) {
				out += '{';
				if (v) {
					bool first = true;
					for (auto const &[k, val]: *v) {
						if (!first) {
							out += ',';
						}
						first = false;
						dump_str(k, out);
						out += ':';
						val.dump(out);
					}
				}
				out += '}';
			}
		},
		v_);
}

// ---------------------------------------------------------------------------
// Parser — recursive descent, depth-limited (kMaxNesting).
// misc-no-recursion is suppressed: depth is bounded by kMaxNesting.
// ---------------------------------------------------------------------------

namespace {

constexpr int kMaxNesting = 512;

struct Parser {
	string_view src{};
	size_t pos{};
	u32 line{1};
	u32 col{1};
	bool own_strings{false};

	[[nodiscard]] ParseError mk_err(
		ParseError::Code code) const noexcept {
		return {.code = code, .offset = pos, .line = line, .column = col};
	}

	// NOLINTBEGIN(readability-convert-member-functions-to-static,bugprone-branch-clone)
	void skip_ws() noexcept {
		while (pos < src.size()) {
			// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
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
	// NOLINTEND(readability-convert-member-functions-to-static,bugprone-branch-clone)

	void adv(
		size_t n = 1) noexcept {
		pos += n;
		col += static_cast<u32>(n);
	}

	// Append a UTF-32 code point as UTF-8 to `out`.
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

	// Parse 4 hex digits starting at pos, advance, write to out.
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] bool hex4(
		u32 &out) noexcept {
		if (pos + 4 > src.size()) {
			return false;
		}
		out = 0;
		for (size_t i = 0; i < 4; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
			char const c = src[pos + i];
			u32 d = 0;
			constexpr u32 kHexA = 10; // NOLINT(readability-magic-numbers)
			if (c >= '0' && c <= '9') {
				d = static_cast<u32>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				d = static_cast<u32>(c - 'a') + kHexA;
			} else if (c >= 'A' && c <= 'F') {
				d = static_cast<u32>(c - 'A') + kHexA;
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

	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity,readability-make-member-function-const)
	[[nodiscard]] expected<Value, ParseError> parse_str() {
		constexpr unsigned char kControlEnd = 0x20U;
		size_t const start = pos;
		while (pos < src.size()) {
			// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
			auto c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				string_view const sv = src.substr(start, pos - start);
				adv();
				if (own_strings) {
					return Value{string{sv}};
				}
				return Value{sv};
			}
			if (c == '\\' || c < kControlEnd) {
				break;
			}
			++pos;
			++col;
		}
		if (pos >= src.size()) {
			return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
		}
		if (static_cast<unsigned char>(src[pos]) < kControlEnd) {
			return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
		}

		string decoded;
		decoded.reserve(pos - start + 16);
		decoded.append(src.substr(start, pos - start));

		while (pos < src.size()) {
			// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
			auto c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				adv();
				return Value{std::move(decoded)};
			}
			if (c < kControlEnd) {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			if (c != '\\') {
				decoded += static_cast<char>(c);
				adv();
				continue;
			}

			adv(); // skip '\'
			if (pos >= src.size()) {
				return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
			}
			switch (src[pos]) {
			case '"':
				decoded += '"';
				adv();
				break;
			case '\\':
				decoded += '\\';
				adv();
				break;
			case '/':
				decoded += '/';
				adv();
				break;
			case 'b':
				decoded += '\b';
				adv();
				break;
			case 'f':
				decoded += '\f';
				adv();
				break;
			case 'n':
				decoded += '\n';
				adv();
				break;
			case 'r':
				decoded += '\r';
				adv();
				break;
			case 't':
				decoded += '\t';
				adv();
				break;
			case 'u':
				{
					adv();
					u32 cp = 0;
					if (!hex4(cp)) {
						return unexpected{mk_err(ParseError::Code::InvalidUnicode)};
					}
					// NOLINTBEGIN(readability-magic-numbers)
					if (cp >= 0xD800U && cp <= 0xDBFFU) {
						if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
							return unexpected{mk_err(ParseError::Code::InvalidUnicode)};
						}
						adv(2);
						u32 low = 0;
						if (!hex4(low) || low < 0xDC00U || low > 0xDFFFU) {
							return unexpected{mk_err(ParseError::Code::InvalidUnicode)};
						}
						cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
					} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
						return unexpected{mk_err(ParseError::Code::InvalidUnicode)};
					}
					// NOLINTEND(readability-magic-numbers)
					append_utf8(cp, decoded);
					break;
				}
			default: return unexpected{mk_err(ParseError::Code::InvalidEscape)};
			}
		}
		return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<Value, ParseError> parse_value(
		int depth) {
		skip_ws();
		if (pos >= src.size()) {
			return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
		}
		if (depth > kMaxNesting) {
			return unexpected{mk_err(ParseError::Code::NestingTooDeep)};
		}

		// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
		char const c = src[pos];
		if (c == '"') {
			adv();
			return parse_str();
		}
		if (c == '[') {
			return parse_array(depth);
		}
		if (c == '{') {
			return parse_object(depth);
		}
		if (c == 't') {
			if (src.substr(pos, 4) != "true") {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv(4);
			return Value{true};
		}
		if (c == 'f') {
			if (src.substr(pos, 5) != "false") {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv(5);
			return Value{false};
		}
		if (c == 'n') {
			if (src.substr(pos, 4) != "null") {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv(4);
			return Value{monostate{}};
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return parse_num();
		}
		return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<Value, ParseError> parse_array(
		int depth) {
		adv(); // skip '['
		auto arr = make_shared<vector<Value>>();
		skip_ws();
		if (pos < src.size() && src[pos] == ']') {
			adv();
			return Value{arr};
		}
		while (true) {
			auto v = parse_value(depth + 1);
			if (!v) {
				return unexpected{v.error()};
			}
			arr->push_back(std::move(*v));
			skip_ws();
			if (pos >= src.size()) {
				return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
			}
			if (src[pos] == ']') {
				adv();
				return Value{arr};
			}
			if (src[pos] != ',') {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv();
		}
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<Value, ParseError> parse_object(
		int depth) {
		adv(); // skip '{'
		auto obj = make_shared<vector<pair<string, Value>>>();
		skip_ws();
		if (pos < src.size() && src[pos] == '}') {
			adv();
			return Value{obj};
		}
		while (true) {
			skip_ws();
			if (pos >= src.size() || src[pos] != '"') {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv();
			// Keys always copied into std::string.
			bool const save = own_strings;
			own_strings = true;
			auto key_v = parse_str();
			own_strings = save;
			if (!key_v) {
				return unexpected{key_v.error()};
			}
			auto key = key_v->as<string>();

			skip_ws();
			if (pos >= src.size() || src[pos] != ':') {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv();

			auto val = parse_value(depth + 1);
			if (!val) {
				return unexpected{val.error()};
			}
			obj->emplace_back(std::move(key), std::move(*val));

			skip_ws();
			if (pos >= src.size()) {
				return unexpected{mk_err(ParseError::Code::UnexpectedEof)};
			}
			if (src[pos] == '}') {
				adv();
				return Value{obj};
			}
			if (src[pos] != ',') {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			adv();
		}
	}

	// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-convert-member-functions-to-static)
	[[nodiscard]] expected<Value, ParseError> parse_num() {
		size_t const start = pos;
		// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
		bool const neg = src[pos] == '-';
		if (neg) {
			adv();
		}
		// RFC 8259 §6: integer part must be present (digit required after optional '-').
		if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
			return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
		}
		// RFC 8259 §6: leading zeros forbidden (only '0' alone, or '1'-'9' followed by digits).
		// NOLINTNEXTLINE(cppcoreguidelines-init-variables)
		bool const int_starts_zero = src[pos] == '0';
		adv();
		if (int_starts_zero && pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
			return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
		}
		while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
			adv();
		}
		bool frac = false;
		bool exp = false;
		if (pos < src.size() && src[pos] == '.') {
			adv();
			// RFC 8259 §6: at least one digit must follow the decimal point.
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
				return unexpected{mk_err(ParseError::Code::UnexpectedChar)};
			}
			frac = true;
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
			exp = true;
			adv();
			if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
				adv();
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		string_view const sv = src.substr(start, pos - start);
		auto const *b = sv.data();
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto const *e = b + sv.size();

		if (!frac && !exp && !neg) {
			u64 u{};
			if (auto [p, ec] = from_chars(b, e, u); ec == errc{} && p == e) {
				return (u <= static_cast<u64>(numeric_limits<i64>::max())) ? Value{static_cast<i64>(u)} : Value{u};
			}
		}
		if (!frac && !exp) {
			i64 i{};
			if (auto [p, ec] = from_chars(b, e, i); ec == errc{} && p == e) {
				return Value{i};
			}
		}
		double d{};
		if (auto [p, ec] = from_chars(b, e, d); ec == errc{} && p == e) {
			return Value{d};
		}
		return unexpected{mk_err(ParseError::Code::NumberOverflow)};
	}
};

} // namespace

// Module-internal (not exported, not anonymous-namespace) so exported templates can call it.
expected<Document, ParseError> do_parse(
	string_view src,
	bool own_strings,
	shared_ptr<string const> backing = {}) {
	Parser p;
	p.src = src;
	p.own_strings = own_strings;

	p.skip_ws();
	if (p.pos >= src.size()) {
		return unexpected{p.mk_err(ParseError::Code::UnexpectedEof)};
	}

	auto root = p.parse_value(0);
	if (!root) {
		return unexpected{root.error()};
	}

	p.skip_ws();
	if (p.pos < src.size()) {
		return unexpected{p.mk_err(ParseError::Code::TrailingContent)};
	}

	Document doc; // NOLINT(misc-const-correctness) — set_root is non-const
	doc.set_root(std::move(*root));
	doc.set_backing(std::move(backing));
	return doc;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

export namespace json {

// Safe default: parse into a self-contained document.
[[nodiscard]] expected<Document, ParseError> parse(
	string_view input) {
	return do_parse(input, true);
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// Own-mode concept: any type whose string_view constructor is valid.
template<typename T>
concept JsonSourceOwned = requires(T const &t) { string_view{t}; };

// Own mode: all strings are std::string; Document is self-contained.
template<JsonSourceOwned T>
[[nodiscard]] expected<Document, ParseError> parse(
	T const &src) {
	return do_parse(string_view{src}, true);
}

// Borrowed-mode API: parsed string values reference document-managed backing
// storage instead of individual owned string allocations.
[[nodiscard]] expected<Document, ParseError> parse_borrowed(
	string_view input) {
	auto backing = make_shared<string>(input);
	return do_parse(*backing, false, backing);
}

[[nodiscard]] expected<Document, ParseError> parse_borrowed(
	string input) {
	auto backing = make_shared<string>(std::move(input));
	return do_parse(*backing, false, backing);
}
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ---------------------------------------------------------------------------
// Value builders
// ---------------------------------------------------------------------------

[[nodiscard]] Value null_value() {
	return Value{monostate{}};
}
[[nodiscard]] Value bool_value(
	bool b) {
	return Value{b};
}
[[nodiscard]] Value int_value(
	i64 i) {
	return Value{i};
}
[[nodiscard]] Value uint_value(
	u64 u) {
	return Value{u};
}
[[nodiscard]] Value float_value(
	double d) {
	return Value{d};
}

// Builders produce self-contained values by default.
[[nodiscard]] Value string_value(
	string_view sv) {
	return Value{string{sv}};
}
[[nodiscard]] Value string_value(
	char const *s) {
	return Value{string{s}};
}
// Owned: Value stores the string by value.
[[nodiscard]] Value string_value(
	string s) {
	return Value{std::move(s)};
}

struct KeyVal {
	string key{};
	Value val;
};

[[nodiscard]] Value object(
	initializer_list<KeyVal> kvs = {}) {
	auto obj = make_shared<vector<pair<string, Value>>>();
	for (auto const &kv: kvs) {
		obj->emplace_back(kv.key, kv.val);
	}
	return Value{std::move(obj)};
}

[[nodiscard]] Value array(
	initializer_list<Value> elems = {}) {
	return Value{make_shared<vector<Value>>(elems)};
}

// Mutate an object Value in-place (no-op if not an object).
void object_set(
	Value &obj,
	string key,
	Value val) {
	auto sp = obj.shared_object();
	if (!sp) {
		return;
	}
	auto it = ranges::find_if(*sp, [&](auto const &kv) { return kv.first == key; });
	if (it != sp->end()) {
		it->second = std::move(val);
	} else {
		sp->emplace_back(std::move(key), std::move(val));
	}
}

// Append to an array Value in-place (no-op if not an array).
void array_push(
	Value &arr,
	Value val) {
	auto sp = arr.shared_array();
	if (sp) {
		sp->push_back(std::move(val));
	}
}

} // namespace json
