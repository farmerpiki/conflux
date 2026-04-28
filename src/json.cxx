module;
#include <cassert>
#include <locale.h>
#include <stdlib.h>
#include <sys/random.h>
#include <xxhash.h>
#if defined(__x86_64__) || defined(_M_X64)
	#include <immintrin.h>
	#ifndef CONFLUX_JSON_DISABLE_SIMD
		#define CONFLUX_JSON_HAS_SSE2 1
	#endif
#endif

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
export struct WarmIndexOptions;
export template<class T>
class Nullable;

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

// ---------------------------------------------------------------------------
// Phase 0 (v11) — Node flag bits.
//
// Storage flags (where the bytes live; populated in Phase 1+ — for Phase 0,
// numbers/strings still live in string_arena and these flags are clear).
// ---------------------------------------------------------------------------

constexpr u8 kStorageInputView = 0x01; // off,len index into input_view
constexpr u8 kRawJsonSlice = 0x02; // bytes are raw JSON content (dump-safe memcpy)

// Number value-kind flags (at most one of kValKind* set on a number node).
constexpr u8 kLexIntForm = 0x08; // lexeme matches -?(0|[1-9][0-9]*)
constexpr u8 kValKindInt = 0x10; // ival valid
constexpr u8 kValKindUint = 0x20; // uval valid
constexpr u8 kValKindF64 = 0x40; // dval valid
constexpr u8 kValKindDeferred = 0x04; // range-error f64 ≤ 4 KiB; strtod_l deferred to to_f64()

// All three kValKind* clear on a number node = f64-overflow (lexeme preserved).

constexpr u32 kMemberExternalView = 0x04u; // insert_member_view: caller-owned pointer in name_ptr

struct MemberEntry {
	u32 name_off;
	u32 name_len;
	u32 val_node;
	u32 name_flags; // 0=arena; kStorageInputView=0x01; kMemberExternalView=0x02
	char const *name_ptr{nullptr}; // C: filled by build_table; E: caller-owned pointer
};
static_assert(sizeof(MemberEntry) == 24);
static_assert(std::is_trivially_copyable_v<MemberEntry>);

// ---------------------------------------------------------------------------
// Phase 6 — ObjHashTable (v14 HHH–JJJ, v15 RRR–SSS)
// ---------------------------------------------------------------------------

constexpr u32 kEmptySlot = ~u32{};

struct ObjHashSlot {
	u32 member_index{kEmptySlot};
	u32 name_hash{0};
};
static_assert(sizeof(ObjHashSlot) == 8);

struct ObjHashTable {
	u32 capacity;
	u32 member_count;

	[[nodiscard]] ObjHashSlot *slots_data() noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<ObjHashSlot *>(this + 1);
	}
	[[nodiscard]] ObjHashSlot const *slots_data() const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<ObjHashSlot const *>(this + 1);
	}

	static ObjHashTable *create(
		u32 capacity,
		u32 member_count) noexcept {
		size_t const bytes = sizeof(ObjHashTable) + sizeof(ObjHashSlot) * capacity;
		void *mem = ::operator new(bytes, std::nothrow); // NOLINT(misc-const-correctness)
		if (mem == nullptr) {
			return nullptr;
		}
		auto *t = ::new (mem) ObjHashTable{capacity, member_count};
		std::fill_n(t->slots_data(), capacity, ObjHashSlot{});
		return t;
	}
	static void destroy(
		ObjHashTable *t) noexcept {
		if (t == nullptr) {
			return;
		}
		t->~ObjHashTable();
		::operator delete(t);
	}
};

constexpr u32 kHashThreshold = 32;
constexpr u32 kProbeChainMax = 64;
constexpr u32 kMaxHashTableCapacity = 1u << 30;
// FI-7 — practical byte budget on the per-object hash index to bound
// DoS payloads. 256 MiB / 8 B per slot = 32 Mi slots; well above any
// realistic object size.
constexpr size_t kMaxHashIndexBytes = 256ULL * 1024 * 1024;
// FI-1 — sentinel value stashed into hash_idx_raw when a previous build
// attempt failed (probe-cap reached or budget exceeded). Subsequent
// find_member calls observe the sentinel and short-circuit straight to
// the linear scan, avoiding repeated build attempts. The Document
// destructor and published-table reads must treat this sentinel as
// "no table" (i.e. neither dereference nor delete it).
inline ObjHashTable *const kHashBuildFailedSentinel = reinterpret_cast<ObjHashTable *>(static_cast<uintptr_t>(1));

// ---------------------------------------------------------------------------
// Phase 0 (v11) — Node, 24 B, u32 offsets, union payload.
//
// The 8-byte union's active member is determined by (kind, flags):
//   kind == null_                          → none (zero-init via _raw)
//   kind == boolean                        → bool_val
//   kind == string_                        → none (bytes via off/len)
//   kind == number, flags & kValKindInt    → ival
//   kind == number, flags & kValKindUint   → uval
//   kind == number, flags & kValKindF64    → dval
//   kind == number, flags & kValKindDeferred → range-error f64 (≤4 KiB); strtod_l at to_f64()
//   kind == number, no kValKind* flag      → f64-overflow; lexeme only
//   kind == array                          → none (children via off/len)
//   kind == object                         → hash_idx_raw (lazy; nullable)
// Construction goes exclusively through factory helpers below.
// ---------------------------------------------------------------------------

struct Node {
	NodeKind kind;
	u8 flags;
	u16 _pad0;
	u32 off;
	u32 len;
	u32 _pad1;
	union {
		bool bool_val;
		i64 ival;
		u64 uval;
		double dval;
		ObjHashTable *hash_idx_raw;
		u64 _raw;
	};
};
static_assert(sizeof(Node) == 24);
static_assert(alignof(Node) >= 8);
static_assert(std::is_trivially_copyable_v<Node>);
static_assert(std::is_trivially_destructible_v<Node>);
static_assert(std::atomic_ref<ObjHashTable *>::is_always_lock_free);
static_assert(alignof(Node) >= std::atomic_ref<ObjHashTable *>::required_alignment);

// Factory helpers (Correction NN/MMM): every kind transition routes through here.
namespace detail {

[[nodiscard]] inline Node make_null() noexcept {
	return Node{.kind = NodeKind::null_, .flags = 0, ._pad0 = 0, .off = 0, .len = 0, ._pad1 = 0, ._raw = 0};
}

[[nodiscard]] inline Node make_bool(
	bool v) noexcept {
	return Node{.kind = NodeKind::boolean, .flags = 0, ._pad0 = 0, .off = 0, .len = 0, ._pad1 = 0, .bool_val = v};
}

[[nodiscard]] inline Node make_string(
	u32 off,
	u32 len,
	u8 flags) noexcept {
	return Node{.kind = NodeKind::string_, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}

[[nodiscard]] inline Node make_array(
	u32 off,
	u32 len) noexcept {
	return Node{.kind = NodeKind::array, .flags = 0, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}

[[nodiscard]] inline Node make_object(
	u32 off,
	u32 len) noexcept {
	return Node{
		.kind = NodeKind::object,
		.flags = 0,
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		.hash_idx_raw = nullptr};
}

[[nodiscard]] inline Node make_number_int(
	u32 off,
	u32 len,
	u8 storage_flags,
	i64 v) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<u8>(storage_flags | kLexIntForm | kValKindInt),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		.ival = v};
}

[[nodiscard]] inline Node make_number_uint(
	u32 off,
	u32 len,
	u8 storage_flags,
	u64 v) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<u8>(storage_flags | kLexIntForm | kValKindUint),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		.uval = v};
}

[[nodiscard]] inline Node make_number_f64(
	u32 off,
	u32 len,
	u8 storage_flags,
	double v,
	bool int_form) noexcept {
	u8 const flags = static_cast<u8>(storage_flags | (int_form ? kLexIntForm : 0) | kValKindF64);
	return Node{.kind = NodeKind::number, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, .dval = v};
}

[[nodiscard]] inline Node make_number_overflow(
	u32 off,
	u32 len,
	u8 storage_flags,
	bool int_form) noexcept {
	u8 const flags = static_cast<u8>(storage_flags | (int_form ? kLexIntForm : 0));
	return Node{.kind = NodeKind::number, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}

[[nodiscard]] inline Node make_number_deferred(
	u32 off,
	u32 len,
	u8 storage_flags) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<u8>(storage_flags | kValKindDeferred),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		._raw = 0};
}

[[nodiscard]] inline u64 make_hash_seed() noexcept {
	u64 seed{};
	if (::getrandom(&seed, sizeof(seed), 0) != static_cast<ssize_t>(sizeof(seed))) {
		seed = static_cast<u64>(reinterpret_cast<uintptr_t>(&seed)) ^ UINT64_C(0x517cc1b727220a95);
	}
	return seed;
}

} // namespace detail

struct DocumentStorage {
	vector<Node> nodes;
	string string_arena;
	vector<u32> array_children;
	vector<MemberEntry> object_members;
	// Phase 1 (v11): owned/borrowed input buffer. Numbers index into input_view
	// when their flags include kStorageInputView; strings still live in
	// string_arena until Phase 2 lands.
	unique_ptr<string> owned_input; // non-null for copy/move modes; null in parse_borrowed
	string_view input_view; // post-BOM bytes; pointer-stable across Document moves
	u32 root_node{0}; // index of the root Node
	u32 bom_prefix_bytes{0}; // 0 or 3 — added to source-offset reports (Correction Q)
	u64 hash_seed_{detail::make_hash_seed()};

	DocumentStorage() = default;
	DocumentStorage(DocumentStorage const &) = delete;
	DocumentStorage &operator =(DocumentStorage const &) = delete;
	// Explicit move ops re-instate what the user-declared destructor suppresses.
	// owned_input transfers via unique_ptr (heap allocation stays put);
	// input_view's data pointer remains valid because it points into the heap
	// string body, not the moved-from string object.
	DocumentStorage(DocumentStorage &&) noexcept = default;
	DocumentStorage &operator =(DocumentStorage &&) noexcept = default;
	~DocumentStorage() noexcept {
		for (auto &n: nodes) {
			if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
				ObjHashTable::destroy(n.hash_idx_raw);
			}
		}
	}

	[[nodiscard]] string_view str_at(
		u32 off,
		u32 len) const noexcept {
		return {string_arena.data() + off, len};
	}

	// Resolve a Node's lexeme bytes by storage-flag dispatch.
	[[nodiscard]] string_view bytes_at(
		u32 off,
		u32 len,
		u8 flags) const noexcept {
		if ((flags & kStorageInputView) != 0) {
			return input_view.substr(off, len);
		}
		return str_at(off, len);
	}

	[[nodiscard]] string_view member_name(
		MemberEntry const &m) const noexcept {
		if ((m.name_flags & kMemberExternalView) != 0) {
			return {m.name_ptr, m.name_len}; // Item E: caller-owned pointer
		}
		return bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags));
	}
};

// ---------------------------------------------------------------------------
// Phase 0 — slow-path f64 classifier (v14 AAA–EEE, v15 QQQ)
// ---------------------------------------------------------------------------

// v15 TTT: lexemes longer than this are conservatively classified as
// overflow_infinite → number_out_of_range. v7 would have returned 0.0 for
// pathological underflow tokens like "0." + 4000+ zeros + "1". DoS hardening
// against megabyte-long number tokens.
constexpr size_t kSlowFloatLexemeCopyLimit = 4096;
constexpr size_t kMaxNumberLexemeLen = 1024;

namespace detail {

struct CLocaleHolder {
	::locale_t loc;
	bool ok;
};

[[nodiscard]] inline CLocaleHolder const &c_locale_holder() noexcept {
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	static CLocaleHolder const h = [] {
		::locale_t l = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0)); // NOLINT(modernize-use-nullptr)
		return CLocaleHolder{l, l != static_cast<::locale_t>(0)}; // NOLINT(modernize-use-nullptr)
	}();
	// Intentional: process-lifetime singleton, never freelocale'd.
	// Static-destruction order would create use-after-free for any caller
	// that touches the slow path during teardown.
	return h;
}

struct ClassifiedDouble {
	enum class Kind : u8 {
		underflow_finite,
		overflow_infinite,
	} kind;
	double value;
};

[[nodiscard]] inline expected<ClassifiedDouble, JsonError> classify_range_error_slow(
	char const *first,
	char const *last) noexcept {
	auto const &lh = c_locale_holder();
	if (!lh.ok) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::resource_exhausted,
				.message = "newlocale(C) failed at startup; strtod_l unavailable"});
	}

	auto const n = static_cast<size_t>(last - first);
	if (n > kSlowFloatLexemeCopyLimit) {
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}

	// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
	char stack_buf[128];
	// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
	unique_ptr<char[]> heap_buf;
	char *p = nullptr;

	if (n + 1 <= sizeof(stack_buf)) {
		p = stack_buf;
	} else {
		heap_buf = unique_ptr<char[]>{new (nothrow) char[n + 1]};
		if (!heap_buf) {
			return unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::resource_exhausted,
					.message = "OOM in classify_range_error_slow"});
		}
		p = heap_buf.get();
	}

	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	memcpy(p, first, n);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	p[n] = '\0';

	char *end = nullptr; // NOLINT(misc-const-correctness) — strtod_l takes char**
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	double const v = ::strtod_l(p, &end, lh.loc);

	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	if (end != p + n) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::invalid_number,
				.message = "strtod_l rejected lexeme (stage-2 check)"});
	}

	if (isinf(v)) {
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}
	return ClassifiedDouble{ClassifiedDouble::Kind::underflow_finite, v};
}

// Pre-parsed number factory: takes a syntactically valid JSON number lexeme
// (caller validates) plus its arena offset/length, runs the two-stage parse
// (i64 → u64 → f64 with strtod_l fallback for range-error f64), and returns
// the appropriate Node. Locale-init / OOM in the slow path surface as
// resource_exhausted via expected<>.
[[nodiscard]] inline expected<Node, JsonError> build_number_node_from_lexeme(
	u32 off,
	u32 len,
	u8 storage_flags,
	string_view lex) noexcept {
	bool const int_form = lex.find_first_of(".eE") == string_view::npos;
	bool const neg = !lex.empty() && lex.front() == '-';
	auto const *b = lex.data();
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto const *e = lex.data() + lex.size();

	if (int_form) {
		int64_t iv{};
		if (auto [p, ec] = from_chars(b, e, iv); ec == errc{} && p == e) {
			return make_number_int(off, len, storage_flags, iv);
		}
		if (!neg) {
			uint64_t uv{};
			if (auto [p2, ec2] = from_chars(b, e, uv); ec2 == errc{} && p2 == e) {
				return make_number_uint(off, len, storage_flags, uv);
			}
		}
	}

	double dv{};
	auto const [p, ec] = from_chars(b, e, dv, chars_format::general);
	if (ec == errc{} && p == e) {
		if (isfinite(dv)) {
			return make_number_f64(off, len, storage_flags, dv, int_form);
		}
		return make_number_overflow(off, len, storage_flags, int_form);
	}
	if (ec == errc::result_out_of_range) {
		if (static_cast<size_t>(e - b) > kSlowFloatLexemeCopyLimit) {
			return make_number_overflow(off, len, storage_flags, int_form);
		}
		return make_number_deferred(off, len, storage_flags);
	}
	return unexpected(
		JsonError{
			.stage = JsonStage::parse,
			.code = JsonIssueCode::invalid_number,
			.message = format("number rejected by from_chars: {}", lex)});
}

} // namespace detail

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
	u64 raw_payload_; // bit-cast<i64/u64/double> selected by flags_
	u8 flags_; // kLexIntForm | kValKindInt|Uint|F64

	friend class NodeRef;
	friend bool is_value_equal(NodeRef, NodeRef);
	JsonNumberView(
		string_view lex,
		u8 flags,
		u64 raw) noexcept
		: lexeme_{lex}
		, raw_payload_{raw}
		, flags_{flags} {}

public:
	[[nodiscard]] string_view lexeme() const noexcept { return lexeme_; }
	[[nodiscard]] JsonNumberForm form() const noexcept {
		return (flags_ & kLexIntForm) != 0 ? JsonNumberForm::integer : JsonNumberForm::non_integer;
	}

	[[nodiscard]] expected<int64_t, JsonError> to_i64() const {
		if ((flags_ & kLexIntForm) == 0) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::invalid_number,
					.message = "to_i64 requires integer-form number"});
		}
		if ((flags_ & kValKindInt) != 0) {
			return std::bit_cast<int64_t>(raw_payload_);
		}
		// kValKindUint, kValKindF64, or no kValKind* (overflow): integer-form
		// lexeme outside i64 range → number_out_of_range (Correction K).
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("value out of i64 range: {}", lexeme_)});
	}

	[[nodiscard]] expected<uint64_t, JsonError> to_u64() const {
		if ((flags_ & kLexIntForm) == 0) {
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
		if ((flags_ & kValKindUint) != 0) {
			return raw_payload_;
		}
		if ((flags_ & kValKindInt) != 0) {
			auto const v = std::bit_cast<int64_t>(raw_payload_);
			if (v >= 0) {
				return static_cast<uint64_t>(v);
			}
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("value out of u64 range: {}", lexeme_)});
	}

	[[nodiscard]] expected<double, JsonError> to_f64() const {
		if ((flags_ & kValKindF64) != 0) {
			return std::bit_cast<double>(raw_payload_);
		}
		if ((flags_ & kValKindInt) != 0) {
			return static_cast<double>(std::bit_cast<int64_t>(raw_payload_));
		}
		if ((flags_ & kValKindUint) != 0) {
			return static_cast<double>(raw_payload_);
		}
		if ((flags_ & kValKindDeferred) != 0) {
			auto res = detail::classify_range_error_slow(lexeme_.data(), lexeme_.data() + lexeme_.size());
			if (!res) {
				auto err = move(res).error();
				err.stage = JsonStage::lookup;
				return unexpected(move(err));
			}
			if (res->kind == detail::ClassifiedDouble::Kind::underflow_finite) {
				return res->value;
			}
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::number_out_of_range,
					.message = format("f64 conversion overflows: {}", lexeme_)});
		}
		// No kValKind* set → f64-overflow (Correction K).
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("f64 conversion overflows: {}", lexeme_)});
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

	[[nodiscard]] Node const &rec() const noexcept {
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
		return storage_->bytes_at(rec().off, rec().len, rec().flags);
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
		return JsonNumberView{storage_->bytes_at(rec().off, rec().len, rec().flags), rec().flags, rec()._raw};
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
// Phase 6 helpers — hash table build + lookup
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline u32 hash_name(
	string_view name,
	u64 seed) noexcept {
	return static_cast<u32>(XXH3_64bits_withSeed(name.data(), name.size(), seed));
}

// Smallest power-of-two >= 2*count, capped at kMaxHashTableCapacity AND
// at kMaxHashIndexBytes / sizeof(ObjHashSlot) (FI-7 — byte-budget cap).
// Returns 0 on overflow so the caller can fall back to linear scan.
[[nodiscard]] inline u32 clamped_capacity(
	u32 count) noexcept {
	constexpr u32 kSlotMax = static_cast<u32>(kMaxHashIndexBytes / sizeof(ObjHashSlot));
	constexpr u32 kEffectiveMax = kMaxHashTableCapacity < kSlotMax ? kMaxHashTableCapacity : kSlotMax;
	u32 cap = 1;
	while (cap < 2 * count && cap < kEffectiveMax) {
		cap <<= 1;
	}
	if (cap < count) {
		return 0; // Object too large to index — fall back to linear scan.
	}
	return cap;
}

// Linear scan: returns val_node index or nullopt.
[[nodiscard]] inline optional<size_t> lookup_linear(
	DocumentStorage const *storage,
	size_t mem_start,
	size_t mem_count,
	string_view name) noexcept {
	for (size_t i = 0; i < mem_count; ++i) {
		auto const &m =
			storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
		if (storage->member_name(m) == name) {
			return m.val_node;
		}
	}
	return nullopt;
}

// Probe hash table; fall back to linear if probe chain exceeds kProbeChainMax.
[[nodiscard]] inline optional<size_t> lookup_in(
	ObjHashTable const &ht,
	DocumentStorage const *storage,
	size_t mem_start,
	size_t mem_count,
	string_view name) noexcept {
	auto const h = hash_name(name, storage->hash_seed_);
	u32 const mask = ht.capacity - 1;
	u32 slot = h & mask;
	auto const *slots = ht.slots_data();
	for (u32 probe = 0; probe < kProbeChainMax; ++probe) {
		auto const &s = slots[slot]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (s.member_index == kEmptySlot) {
			return nullopt;
		}
		if (s.name_hash == h) {
			auto const &m =
				storage->object_members
					[mem_start + s.member_index]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
			if (string_view{m.name_ptr, m.name_len} == name) { // Item C: direct pointer, no dispatch
				return m.val_node;
			}
		}
		slot = (slot + 1) & mask;
	}
	return lookup_linear(storage, mem_start, mem_count, name);
}

// Returns true on success, false on probe-chain overflow (RRR).
[[nodiscard]] inline bool build_table(
	ObjHashTable &ht,
	DocumentStorage const *storage,
	size_t mem_start,
	size_t mem_count) noexcept {
	u32 const mask = ht.capacity - 1;
	auto *slots = ht.slots_data();
	for (u32 i = 0; i < static_cast<u32>(mem_count); ++i) {
		auto const &m =
			storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
		auto const sv = storage->member_name(m);
		const_cast<MemberEntry &>(m).name_ptr = sv.data(); // Item C: cache pointer (arena stable post-parse)
		auto const h = hash_name(sv, storage->hash_seed_);
		u32 slot = h & mask;
		bool inserted = false;
		for (u32 probe = 0; probe < kProbeChainMax; ++probe) {
			auto &s = slots[slot]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			if (s.member_index == kEmptySlot) {
				s = ObjHashSlot{i, h};
				inserted = true;
				break;
			}
			slot = (slot + 1) & mask;
		}
		if (!inserted) {
			return false;
		}
	}
	return true;
}

} // namespace detail

// ---------------------------------------------------------------------------
// ObjectView / ArrayView
// ---------------------------------------------------------------------------

export class ObjectView {
	DocumentStorage const *storage_{};
	size_t mem_start_{};
	size_t mem_count_{};
	size_t node_idx_{};

	friend class NodeRef;
	friend class Document;
	friend bool is_value_equal(NodeRef, NodeRef);
	friend bool is_value_equal_exact(NodeRef, NodeRef);
	ObjectView(
		DocumentStorage const *s,
		size_t start,
		size_t count,
		size_t node_idx) noexcept
		: storage_{s}
		, mem_start_{start}
		, mem_count_{count}
		, node_idx_{node_idx} {}

public:
	[[nodiscard]] size_t size() const noexcept { return mem_count_; }

	[[nodiscard]] optional<NodeRef> find_member(
		string_view name) const noexcept {
		auto to_ref = [&](optional<size_t> idx) -> optional<NodeRef> {
			if (!idx) {
				return nullopt;
			}
			return NodeRef{storage_, *idx};
		};
		if (mem_count_ < kHashThreshold) {
			return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
		}
		// Lazy hash table build via atomic CAS. The hash slot is the only
		// mutable surface on a published Document — see post-publication
		// freeze contract; const_cast is justified by that invariant.
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		auto &raw = const_cast<ObjHashTable *&>(storage_->nodes[node_idx_].hash_idx_raw);
		auto ref = atomic_ref<ObjHashTable *>{raw};
		auto *ht = ref.load(memory_order_acquire);
		// FI-1: prior build failed and was cached. Skip the rebuild; go linear.
		if (ht == kHashBuildFailedSentinel) {
			return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
		}
		if (ht == nullptr) {
			u32 const cap = detail::clamped_capacity(static_cast<u32>(mem_count_));
			bool build_ok = false;
			ObjHashTable *owned = nullptr;
			if (cap > 0) {
				owned = ObjHashTable::create(cap, static_cast<u32>(mem_count_));
				if (owned != nullptr) {
					if (detail::build_table(*owned, storage_, mem_start_, mem_count_)) {
						ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
						if (ref.compare_exchange_strong(
								expected_null,
								owned,
								memory_order_release,
								memory_order_acquire)) {
							ht = owned;
							owned = nullptr; // CAS won — table published
							build_ok = true;
						} else {
							ht = (expected_null == kHashBuildFailedSentinel) ? nullptr : expected_null;
							build_ok = (ht != nullptr);
						}
					}
				}
			}
			if (!build_ok) {
				// FI-1: cache the failure so subsequent lookups don't retry.
				ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
				(void)ref.compare_exchange_strong(
					expected_null,
					kHashBuildFailedSentinel,
					memory_order_release,
					memory_order_acquire);
			}
			ObjHashTable::destroy(owned);
		}
		if (ht != nullptr) {
			return to_ref(detail::lookup_in(*ht, storage_, mem_start_, mem_count_, name));
		}
		return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
	}

	[[nodiscard]] expected<NodeRef, JsonError> member(
		string_view name) const {
		auto found = find_member(name);
		if (!found) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::missing_member,
					.member_name = string{name},
					.message = format("missing member: {}", name)});
		}
		return *found;
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
	return ObjectView{storage_, rec().off, rec().len, idx_};
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
	return ArrayView{storage_, rec().off, rec().len};
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
				storage_->member_name(m),
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
	case NodeKind::string_:
		return a.storage_->bytes_at(a.rec().off, a.rec().len, a.rec().flags)
			== b.storage_->bytes_at(b.rec().off, b.rec().len, b.rec().flags);
	case NodeKind::number:
		{
			auto la = a.storage_->bytes_at(a.rec().off, a.rec().len, a.rec().flags);
			auto lb = b.storage_->bytes_at(b.rec().off, b.rec().len, b.rec().flags);
			if (la == lb) {
				return true;
			}
			auto fa = JsonNumberView{la, a.rec().flags, a.rec()._raw}.to_f64();
			auto fb = JsonNumberView{lb, b.rec().flags, b.rec()._raw}.to_f64();
			return fa && fb && *fa == *fb;
		}
	case NodeKind::array:
		{
			ArrayView const av{a.storage_, a.rec().off, a.rec().len};
			ArrayView const bv{b.storage_, b.rec().off, b.rec().len};
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
			ObjectView const ao{a.storage_, a.rec().off, a.rec().len, a.idx_};
			ObjectView const bo{b.storage_, b.rec().off, b.rec().len, b.idx_};
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
	case NodeKind::number:
		return a.storage_->bytes_at(a.rec().off, a.rec().len, a.rec().flags)
			== b.storage_->bytes_at(b.rec().off, b.rec().len, b.rec().flags);
	case NodeKind::string_:
		return a.storage_->bytes_at(a.rec().off, a.rec().len, a.rec().flags)
			== b.storage_->bytes_at(b.rec().off, b.rec().len, b.rec().flags);
	case NodeKind::array:
		{
			ArrayView const av{a.storage_, a.rec().off, a.rec().len};
			ArrayView const bv{b.storage_, b.rec().off, b.rec().len};
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
			ObjectView const ao{a.storage_, a.rec().off, a.rec().len, a.idx_};
			ObjectView const bo{b.storage_, b.rec().off, b.rec().len, b.idx_};
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

export struct WarmIndexOptions {
	size_t max_objects{SIZE_MAX};
	size_t max_extra_bytes{SIZE_MAX};
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

	// Pre-build hash index for the given object node (idempotent, thread-safe).
	[[nodiscard]] expected<void, JsonError> warm_member_index(
		NodeRef node) const {
		auto obj_or = node.as_object();
		if (!obj_or) {
			return unexpected(move(obj_or).error());
		}
		auto const &ov = *obj_or;
		if (ov.mem_count_ < kHashThreshold) {
			return {};
		}
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
		auto &slot = storage_->nodes[ov.node_idx_].hash_idx_raw;
		auto ref = atomic_ref<ObjHashTable *>{slot};
		auto *prior = ref.load(memory_order_acquire);
		if (prior != nullptr && prior != kHashBuildFailedSentinel) {
			return {}; // already built
		}
		if (prior == kHashBuildFailedSentinel) {
			// Cached prior failure — surface the same error.
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "object hash index unavailable (cached failure)"});
		}
		ObjHashTable *owned = nullptr;
		auto stash_failure_sentinel = [&] {
			ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
			(void)ref.compare_exchange_strong(
				expected_null,
				kHashBuildFailedSentinel,
				memory_order_release,
				memory_order_acquire);
		};
		try {
			u32 const cap = detail::clamped_capacity(static_cast<u32>(ov.mem_count_));
			if (cap == 0) {
				stash_failure_sentinel();
				return unexpected(
					JsonError{
						.stage = JsonStage::lookup,
						.code = JsonIssueCode::resource_exhausted,
						.message = "object exceeds hash-index byte budget"});
			}
			owned = ObjHashTable::create(cap, static_cast<u32>(ov.mem_count_));
			if (owned == nullptr) {
				stash_failure_sentinel();
				return unexpected(
					JsonError{
						.stage = JsonStage::lookup,
						.code = JsonIssueCode::resource_exhausted,
						.message = "OOM building object hash index"});
			}
			if (!detail::build_table(*owned, storage_.get(), ov.mem_start_, ov.mem_count_)) {
				ObjHashTable::destroy(owned);
				stash_failure_sentinel();
				return unexpected(
					JsonError{
						.stage = JsonStage::lookup,
						.code = JsonIssueCode::resource_exhausted,
						.message = "object hash build exceeded probe-chain cap"});
			}
			ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
			if (!ref.compare_exchange_strong(expected_null, owned, memory_order_release, memory_order_acquire)) {
				ObjHashTable::destroy(owned); // lost race — other thread published first
			}
			return {};
		} catch (bad_alloc const &) {
			ObjHashTable::destroy(owned);
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "OOM building object hash index"});
		} catch (...) {
			ObjHashTable::destroy(owned);
			assert(false && "warm_member_index: unexpected exception from no-user-code build path");
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::constraint_violation,
					.message = "unexpected exception building object hash index"});
		}
	}

	// Pre-build hash indices for every object node in the document.
	[[nodiscard]] expected<void, JsonError> warm_member_indices(
		WarmIndexOptions const &opts = {}) const {
		size_t objects_warmed = 0;
		size_t bytes_allocated = 0;
		for (size_t i = 0; i < storage_->nodes.size(); ++i) {
			auto &n = storage_->nodes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
			if (n.kind != NodeKind::object) {
				continue;
			}
			auto const mem_count = n.len;
			if (mem_count < kHashThreshold) {
				continue;
			}
			if (atomic_ref<ObjHashTable *>{n.hash_idx_raw}.load(memory_order_acquire) != nullptr) {
				continue; // already indexed or failed
			}
			u32 const cap = detail::clamped_capacity(static_cast<u32>(mem_count));
			size_t const est_bytes =
				cap > 0 ? sizeof(ObjHashTable) + static_cast<size_t>(cap) * sizeof(ObjHashSlot) : 0;
			if (objects_warmed >= opts.max_objects) {
				break;
			}
			if (est_bytes > 0
				&& opts.max_extra_bytes != SIZE_MAX
				&& bytes_allocated + est_bytes > opts.max_extra_bytes) {
				break;
			}
			auto res = warm_member_index(NodeRef{storage_.get(), i});
			if (!res) {
				return res;
			}
			++objects_warmed;
			bytes_allocated += est_bytes;
		}
		return {};
	}
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
// Fast-path dump for bytes already known to be a raw JSON string body
// (kRawJsonSlice set on parse-side unescaped strings/numbers): no scan,
// just bracket the slice with quotes. Caller must guarantee `flags &
// kRawJsonSlice` and !ascii_only (the latter would still need a
// byte-by-byte non-ASCII rewrite).
inline void dump_str_raw(
	string_view sv,
	string &out) {
	out += '"';
	out.append(sv.data(), sv.size());
	out += '"';
}

// R3 — find the next byte in [p, n) that needs escaping in a JSON string
// body. With ascii_only=false: '"', '\\', or any byte < 0x20.
// With ascii_only=true: also any byte >= 0x80 (UTF-8 lead/continuation —
// caller decodes the code point and emits \uXXXX surrogate pairs).
//
// SSE2 chunked scan; scalar tail. Symmetric to detail::simd::scan_str_until_special
// on the parse side, modulo the conditional high-bit threshold.
[[nodiscard]] inline size_t scan_dump_safe_run(
	char const *p,
	size_t n,
	bool ascii_only) noexcept {
	size_t i = 0;
#if defined(CONFLUX_JSON_HAS_SSE2)
	__m128i const v_quote = _mm_set1_epi8('"');
	__m128i const v_back = _mm_set1_epi8('\\');
	// cmplt_epi8(byte, 0x20) is true for ctrl bytes (<0x20) AND signed-negative
	// bytes (>=0x80). When ascii_only is true we want the high-bit catch; when
	// false we still want only ctrl bytes, so we additionally require the
	// signed comparison against 0 (high-bit clear).
	__m128i const v_lim = _mm_set1_epi8(0x20);
	__m128i const v_zero = _mm_setzero_si128();
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back);
		__m128i const lt_lim = _mm_cmplt_epi8(v, v_lim); // ctrl OR high-bit
		__m128i mix = _mm_or_si128(eq_q, eq_b);
		if (ascii_only) {
			mix = _mm_or_si128(mix, lt_lim);
		} else {
			// Restrict lt_lim to non-high-bit (ctrl only).
			__m128i const ctrl_only = _mm_and_si128(lt_lim, _mm_cmpgt_epi8(v, _mm_set1_epi8(-1)));
			(void)v_zero;
			mix = _mm_or_si128(mix, ctrl_only);
		}
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<size_t>(__builtin_ctz(mask));
		}
		i += 16;
	}
#endif
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U) {
			return i;
		}
		if (ascii_only && c >= 0x80U) {
			return i;
		}
	}
	return n;
}

void dump_str(
	string_view sv,
	string &out,
	bool ascii_only) {
	out += '"';
	size_t i = 0;
	while (i < sv.size()) {
		auto const c = static_cast<unsigned char>(sv[i]);
		// Scalar pre-check: when the very next byte already needs escaping,
		// skip the SIMD chunk setup entirely. Avoids paying SIMD cost on
		// escape-dense payloads where every other byte is an escape.
		bool const needs_escape = (c == '"' || c == '\\' || c < 0x20U || (ascii_only && c >= 0x80U));
		if (!needs_escape) {
			// R3 — fast-forward over the safe-ASCII run.
			size_t const run = scan_dump_safe_run(sv.data() + i, sv.size() - i, ascii_only);
			out.append(sv.data() + i, run);
			i += run;
			if (i >= sv.size()) {
				break;
			}
		}
		auto const cc = static_cast<unsigned char>(sv[i]);
		switch (cc) {
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
			if (cc < 0x20U) {
				out += format("\\u{:04x}", static_cast<unsigned>(cc));
				++i;
			} else if (ascii_only && cc >= 0x80U) {
				// Decode UTF-8 to get code point, then emit \uXXXX or surrogate pair.
				u32 cp = 0;
				size_t seq = 0;
				if (cc < 0xE0U) {
					cp = cc & 0x1FU;
					seq = 2;
				} else if (cc < 0xF0U) {
					cp = cc & 0x0FU;
					seq = 3;
				} else {
					cp = cc & 0x07U;
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
				out += static_cast<char>(cc);
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
	case NodeKind::string_:
		{
			auto const bytes = store.bytes_at(n.off, n.len, n.flags);
			if ((n.flags & kRawJsonSlice) != 0 && !opts.ascii_only) {
				dump_str_raw(bytes, out);
			} else {
				dump_str(bytes, out, opts.ascii_only);
			}
			break;
		}
	case NodeKind::number: out += store.bytes_at(n.off, n.len, n.flags); break;
	case NodeKind::array:
		{
			out += '[';
			if (n.len > 0) {
				for (size_t i = 0; i < n.len; ++i) {
					if (i > 0) {
						out += ',';
					}
					indent(depth + 1);
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
					dump_node(store, store.array_children[n.off + i], opts, depth + 1, out);
				}
				indent(depth);
			}
			out += ']';
			break;
		}
	case NodeKind::object:
		{
			out += '{';
			if (n.len > 0) {
				// R3 — only allocate the order vector when sorting; the
				// unsorted path iterates members in source order directly.
				if (opts.sort_object_keys) {
					vector<size_t> order(n.len);
					iota(order.begin(), order.end(), 0);
					sort(order.begin(), order.end(), [&](size_t x, size_t y) {
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &mx = store.object_members[n.off + x];
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &my = store.object_members[n.off + y];
						return store.member_name(mx) < store.member_name(my);
					});
					for (size_t i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &m = store.object_members[n.off + order[i]];
						if ((m.name_flags & kRawJsonSlice) != 0 && !opts.ascii_only) {
							dump_str_raw(store.member_name(m), out);
						} else {
							dump_str(store.member_name(m), out, opts.ascii_only);
						}
						out += opts.pretty ? ": " : ":";
						dump_node(store, m.val_node, opts, depth + 1, out);
					}
				} else {
					for (size_t i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
						auto const &m = store.object_members[n.off + i];
						if ((m.name_flags & kRawJsonSlice) != 0 && !opts.ascii_only) {
							dump_str_raw(store.member_name(m), out);
						} else {
							dump_str(store.member_name(m), out, opts.ascii_only);
						}
						out += opts.pretty ? ": " : ":";
						dump_node(store, m.val_node, opts, depth + 1, out);
					}
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
	// R3 — skip the small-buffer doubling cycle. Empirically dump output
	// is roughly 1.05–1.2x the input size for compact corpora and within
	// 3x for pretty-printed; reserve from string_arena + nodes count.
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

constexpr size_t kDefaultMaxDepth = 128;
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

// ---------------------------------------------------------------------------
// Phase 8 — SIMD scans for the Tokenizer hot path (SSE2 baseline on x86-64).
// ---------------------------------------------------------------------------
namespace detail::simd {

// Scan [p, end) for the first byte that is '"', '\\', or has high bit set
// (>=0x80) or is a control byte (<0x20). Returns the offset to that byte
// from p, or (end - p) if none found. The scalar caller is responsible for
// classifying the byte at the returned offset (terminator / escape / error /
// UTF-8 lead) — this routine only fast-forwards over bulk ASCII content.
[[nodiscard]] inline size_t scan_str_until_special(
	char const *p,
	size_t n) noexcept {
	size_t i = 0;
#if defined(CONFLUX_JSON_HAS_SSE2)
	__m128i const v_quote = _mm_set1_epi8('"');
	__m128i const v_back = _mm_set1_epi8('\\');
	// Signed cmplt against 0x20 simultaneously catches bytes <0x20 (positive
	// small) and bytes >=0x80 (negative under signed interpretation).
	__m128i const v_lim = _mm_set1_epi8(0x20);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back);
		__m128i const lt_lim = _mm_cmplt_epi8(v, v_lim);
		__m128i const mix = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_lim);
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<size_t>(__builtin_ctz(mask));
		}
		i += 16;
	}
#endif
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U || c >= 0x80U) {
			return i;
		}
	}
	return n;
}

} // namespace detail::simd

// Phase 3 — Tokenizer owns input bytes / source coordinates and emits string
// + number lexemes; TreeBuilder consumes those + structural punctuation and
// builds Nodes. Splitting them keeps the byte-level scan layer reusable
// (SIMD prerequisite) without changing semantics.
struct Tokenizer {
	string_view src;
	size_t pos{};
	size_t line{1};
	size_t col{1};
	DocumentStorage &store;
	u32 bom_prefix_bytes;

	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		string msg) const {
		// Source offsets are reported in raw input bytes including any
		// stripped BOM (Correction Q): bom_prefix_bytes is added here so
		// every error site sees v7-compatible coordinates.
		return {
			.stage = JsonStage::parse,
			.code = code,
			.source = JsonSourceLocation{.offset = pos + bom_prefix_bytes, .line = line, .column = col},
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

	struct ParsedStr {
		u32 off;
		u32 len;
		u8 flags; // kStorageInputView | kRawJsonSlice for zero-copy, 0 for escaped
	};

	// Phase 2: fast path scans for `"` or `\` without copying. On `\`, copies
	// the prefix to escape_arena and continues with the escape-decoding loop;
	// the result then lives in escape_arena with flags = 0.
	// Phase 8: SIMD-accelerated bulk-ASCII fast-forward via
	// detail::simd::scan_str_until_special; scalar fallback handles the
	// boundary byte (terminator / escape / control / UTF-8 lead).
	[[nodiscard]] expected<ParsedStr, JsonError> parse_str_body() {
		constexpr unsigned char kCtrlEnd = 0x20U;
		auto const start_pos = static_cast<u32>(pos);
		while (pos < src.size()) {
			size_t const remaining = src.size() - pos;
			size_t const skip = detail::simd::scan_str_until_special(src.data() + pos, remaining);
			pos += skip;
			col += skip;
			if (pos >= src.size()) {
				break;
			}
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				auto const len = static_cast<u32>(pos) - start_pos;
				adv();
				return ParsedStr{start_pos, len, static_cast<u8>(kStorageInputView | kRawJsonSlice)};
			}
			if (c < kCtrlEnd) {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				// Slow path: copy bytes seen so far to escape_arena, then keep decoding.
				size_t const arena_off = store.string_arena.size();
				store.string_arena.append(src.data() + start_pos, pos - start_pos);
				return parse_str_decode_tail(arena_off);
			}
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
			pos += seq;
			col += 1;
		}
		return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}

	[[nodiscard]] expected<ParsedStr, JsonError> parse_str_decode_tail(
		size_t arena_off) {
		constexpr unsigned char kCtrlEnd = 0x20U;
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				adv();
				size_t const len = store.string_arena.size() - arena_off;
				return ParsedStr{static_cast<u32>(arena_off), static_cast<u32>(len), 0};
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
		return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}

	// Scans a number lexeme per RFC 8259 grammar and returns the slice of `src`
	// covering it. Caller (TreeBuilder) classifies the value and stores the
	// node; the lexeme references input_view directly (Phase 1: zero-copy).
	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] expected<string_view, JsonError> parse_number_lexeme() {
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
		if (pos < src.size() && src[pos] == '.') {
			adv();
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
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
		if (pos - start > kMaxNumberLexemeLen) {
			return unexpected(mk_err(JsonIssueCode::invalid_number, "number lexeme exceeds maximum length"));
		}
		return src.substr(start, pos - start);
	}
};

struct TreeBuilder {
	Tokenizer tok;
	DocumentStorage &store;
	JsonParseOptions const &opts;

	// Phase 4: shared staging buffers across nested array/object frames. Each
	// frame's slice is [frame.children_start .. staging.size()) for arrays and
	// [frame.members_start .. staging_members.size()) for objects; on close
	// the slice is moved to store.array_children / store.object_members and
	// the staging buffer truncated back. This eliminates per-frame heap
	// allocation that the v7-style local vectors paid for each container.
	vector<u32> staging;
	vector<MemberEntry> staging_members;

	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		string msg) const {
		return tok.mk_err(code, move(msg));
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<size_t, JsonError> parse_value(
		size_t depth) {
		tok.skip_ws();
		if (tok.pos >= tok.src.size()) {
			return unexpected(mk_err(JsonIssueCode::unexpected_eof, "unexpected end of input"));
		}
		if (opts.max_depth.exceeds(depth, kDefaultMaxDepth)) {
			return unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}

		char const c = tok.src[tok.pos];
		if (c == '"') {
			tok.adv();
			return parse_str_node();
		}
		if (c == '[') {
			return parse_array(depth);
		}
		if (c == '{') {
			return parse_object(depth);
		}
		if (c == 't') {
			if (tok.src.substr(tok.pos, 4) != "true") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			store.nodes.push_back(detail::make_bool(true));
			return store.nodes.size() - 1;
		}
		if (c == 'f') {
			if (tok.src.substr(tok.pos, 5) != "false") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(5);
			store.nodes.push_back(detail::make_bool(false));
			return store.nodes.size() - 1;
		}
		if (c == 'n') {
			if (tok.src.substr(tok.pos, 4) != "null") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			store.nodes.push_back(detail::make_null());
			return store.nodes.size() - 1;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return parse_number();
		}
		return unexpected(mk_err(JsonIssueCode::syntax_error, format("unexpected character '{}'", c)));
	}

	[[nodiscard]] expected<size_t, JsonError> parse_str_node() {
		auto parsed = tok.parse_str_body();
		if (!parsed) {
			return unexpected(move(parsed).error());
		}
		if (opts.max_string_size.exceeds(parsed->len, kDefaultMaxString)) {
			return unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
		}
		store.nodes.push_back(detail::make_string(parsed->off, parsed->len, parsed->flags));
		return store.nodes.size() - 1;
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<size_t, JsonError> parse_array(
		size_t depth) {
		tok.adv(); // '['
		tok.skip_ws();
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
			tok.adv();
			size_t const cs = store.array_children.size();
			store.nodes.push_back(detail::make_array(static_cast<u32>(cs), static_cast<u32>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: append child indices to shared staging[children_start..],
		// flush to array_children at close, then truncate staging.
		size_t const children_start = staging.size();
		while (true) {
			auto child = parse_value(depth + 1);
			if (!child) {
				return unexpected(move(child).error());
			}
			staging.push_back(static_cast<u32>(*child));
			tok.skip_ws();
			if (tok.pos >= tok.src.size()) {
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in array"));
			}
			if (tok.src[tok.pos] == ']') {
				tok.adv();
				size_t const len = staging.size() - children_start;
				size_t const cs = store.array_children.size();
				store.array_children.insert(
					store.array_children.end(),
					staging.begin() + static_cast<ptrdiff_t>(children_start),
					staging.end());
				staging.resize(children_start);
				store.nodes.push_back(detail::make_array(static_cast<u32>(cs), static_cast<u32>(len)));
				return store.nodes.size() - 1;
			}
			if (tok.src[tok.pos] != ',') {
				staging.resize(children_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'"));
			}
			tok.adv();
		}
	}

	// Phase 5: linear dedup for n <= 8 (no allocation), lazy unordered_set
	// promotion above the threshold. The set is constructed only when the
	// object actually exceeds the linear-scan window — typical configs
	// (small flat objects) pay zero hash-table cost.
	static constexpr size_t kDedupLinearMax = 8;

	[[nodiscard]] bool dedup_member_present(
		size_t members_start,
		string_view name,
		optional<unordered_set<string_view>> const &seen_hash) const {
		if (seen_hash.has_value()) {
			return seen_hash->contains(name);
		}
		for (size_t i = members_start; i < staging_members.size(); ++i) {
			auto const &m = staging_members[i];
			if (store.bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags)) == name) {
				return true;
			}
		}
		return false;
	}

	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] expected<size_t, JsonError> parse_object(
		size_t depth) {
		tok.adv(); // '{'
		tok.skip_ws();
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
			tok.adv();
			size_t const ms = store.object_members.size();
			store.nodes.push_back(detail::make_object(static_cast<u32>(ms), static_cast<u32>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: members go to shared staging_members[members_start..],
		// flushed to object_members at close.
		size_t const members_start = staging_members.size();
		// Phase 5: dedup is linear until size > kDedupLinearMax, then a
		// hash set is built once and reused for the remainder of this object.
		optional<unordered_set<string_view>> seen_hash;
		while (true) {
			tok.skip_ws();
			if (tok.pos >= tok.src.size() || tok.src[tok.pos] != '"') {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
			}
			tok.adv();
			auto parsed_name = tok.parse_str_body();
			if (!parsed_name) {
				staging_members.resize(members_start);
				return unexpected(move(parsed_name).error());
			}
			string_view const name_sv = store.bytes_at(parsed_name->off, parsed_name->len, parsed_name->flags);
			if (dedup_member_present(members_start, name_sv, seen_hash)) {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::duplicate_member, format("duplicate member: {}", name_sv)));
			}
			if (seen_hash.has_value()) {
				seen_hash->insert(name_sv);
			}

			tok.skip_ws();
			if (tok.pos >= tok.src.size() || tok.src[tok.pos] != ':') {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ':'"));
			}
			tok.adv();

			auto val = parse_value(depth + 1);
			if (!val) {
				staging_members.resize(members_start);
				return unexpected(move(val).error());
			}
			staging_members.push_back({parsed_name->off, parsed_name->len, static_cast<u32>(*val), parsed_name->flags});

			// Promote linear → hash once we cross the threshold.
			size_t const cur_count = staging_members.size() - members_start;
			if (!seen_hash.has_value() && cur_count > kDedupLinearMax) {
				seen_hash.emplace();
				seen_hash->reserve(cur_count * 2);
				for (size_t i = members_start; i < staging_members.size(); ++i) {
					auto const &m = staging_members[i];
					seen_hash->insert(store.bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags)));
				}
			}

			tok.skip_ws();
			if (tok.pos >= tok.src.size()) {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			if (tok.src[tok.pos] == '}') {
				tok.adv();
				size_t const len = staging_members.size() - members_start;
				size_t const ms = store.object_members.size();
				store.object_members.insert(
					store.object_members.end(),
					staging_members.begin() + static_cast<ptrdiff_t>(members_start),
					staging_members.end());
				staging_members.resize(members_start);
				store.nodes.push_back(detail::make_object(static_cast<u32>(ms), static_cast<u32>(len)));
				return store.nodes.size() - 1;
			}
			if (tok.src[tok.pos] != ',') {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'"));
			}
			tok.adv();
		}
	}

	[[nodiscard]] expected<size_t, JsonError> parse_number() {
		size_t const start = tok.pos;
		auto lex_result = tok.parse_number_lexeme();
		if (!lex_result) {
			return unexpected(move(lex_result).error());
		}
		string_view const lex = *lex_result;
		// Phase 1: number lexemes reference input_view directly — zero-copy.
		auto node = detail::build_number_node_from_lexeme(
			static_cast<u32>(start),
			static_cast<u32>(lex.size()),
			static_cast<u8>(kStorageInputView | kRawJsonSlice),
			lex);
		if (!node) {
			return unexpected(move(node).error());
		}
		store.nodes.push_back(*node);
		return store.nodes.size() - 1;
	}
};

// ---------------------------------------------------------------------------
// parse()
// ---------------------------------------------------------------------------

[[nodiscard]] inline expected<void, JsonError> check_input_limits(
	size_t input_size,
	JsonParseOptions const &opts) noexcept {
	// 4 GiB hard ceiling — Fix F / Correction P. Unbypassable by
	// max_input_size = no_limit because Node::off / Node::len /
	// array_children entries are all u32.
	constexpr size_t kU32Ceiling = (size_t{1} << 32) - 1;
	if (input_size >= kU32Ceiling) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds 4 GiB hard ceiling"});
	}
	if (opts.max_input_size.exceeds(input_size, kDefaultMaxInput)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds max_input_size"});
	}
	return {};
}

[[nodiscard]] inline expected<Document, JsonError> parse_with_storage(
	DocumentStorage &storage_ref,
	unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts) {
	// R1 / Polish AA — pre-size the three growth vectors. JSON has roughly
	// one node per 8–16 bytes of input on typical payloads; reserving ahead
	// of the parse skips the geometric realloc cycle on >100 KB inputs.
	// Floor at 64 preserves the tiny-input baseline. A precise structural
	// prescan was tried and rejected — the branchful in-string scan
	// (~1 GB/s) cost more than the realloc copies it saved on the
	// 4 KB / 200 KB corpora in this bench.
	size_t const reserve_n = max<size_t>(64, storage_ref.input_view.size() / 16 + 16);
	storage->nodes.reserve(reserve_n);
	storage->array_children.reserve(reserve_n);
	storage->object_members.reserve(reserve_n);

	TreeBuilder tb{
		.tok =
			Tokenizer{
					  .src = storage_ref.input_view,
					  .store = storage_ref,
					  .bom_prefix_bytes = storage_ref.bom_prefix_bytes},
		.store = storage_ref,
		.opts = opts,
		.staging = {},
		.staging_members = {}
    };
	tb.tok.skip_ws();
	if (tb.tok.pos >= storage_ref.input_view.size()) {
		return unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}

	auto root = tb.parse_value(0);
	if (!root) {
		return unexpected(move(root).error());
	}
	storage->root_node = static_cast<u32>(*root);

	tb.tok.skip_ws();
	if (tb.tok.pos < storage_ref.input_view.size()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source =
					JsonSourceLocation{
									   .offset = tb.tok.pos + storage_ref.bom_prefix_bytes,
									   .line = tb.tok.line,
									   .column = tb.tok.col},
				.message = "trailing content after value"
        });
	}

	return make_document(move(storage));
}

export namespace conflux::json {

// Copies the input into the Document's owned buffer. Number lexemes index
// directly into that buffer (zero-copy on read paths).
expected<Document, JsonError> parse(
	string_view input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	storage->owned_input = make_unique<string>(input);
	string_view src = *storage->owned_input;
	constexpr string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}

// Move-in overload: avoids the input copy. Constrained to actual S
// rvalues so that const char[N] literals select the (string_view) overload
// without ambiguity.
template<class S>
	requires same_as<remove_cvref_t<S>, string> && (!is_lvalue_reference_v<S>)
expected<Document, JsonError> parse(
	S &&input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	storage->owned_input = make_unique<string>(std::forward<S>(input));
	string_view src = *storage->owned_input;
	constexpr string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}

// Borrow-only overload: caller guarantees the bytes outlive the Document.
// Rvalue overload is deleted to prevent obvious lifetime mistakes.
expected<Document, JsonError> parse_borrowed(
	string_view input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	string_view src = input;
	constexpr string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}

// Deleted rvalue overload (Correction T) — borrowing requires the caller to
// own the bytes. Constrained the same way as the parse(string&&) overload
// so const char[N] still selects parse_borrowed(string_view).
template<class S>
	requires same_as<remove_cvref_t<S>, string> && (!is_lvalue_reference_v<S>)
expected<Document, JsonError> parse_borrowed(S &&, JsonParseOptions const & = {}) = delete;

} // namespace conflux::json

// ---------------------------------------------------------------------------
// has_json_codec — forward-declared here so builders can use it in requires
// ---------------------------------------------------------------------------

export template<class T>
struct JsonMembers;
export template<class T>
struct JsonCodec;

namespace detail {

template<class T, class = void>
struct has_codec_spec : false_type {};

template<class T, class = void>
struct has_members_spec : false_type {};

template<class T>
struct is_optional : false_type {};

template<class T>
struct is_optional<optional<T>> : true_type {};

template<class T>
struct is_nullable_type : false_type {};

template<class T>
struct is_nullable_type<Nullable<T>> : true_type {};

} // namespace detail

export template<class T>
concept has_json_codec = detail::has_codec_spec<T>::value || detail::has_members_spec<T>::value;

export template<class T>
inline constexpr bool has_json_codec_v = has_json_codec<T>;

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
	string built_input;
	bool root_set{false};
	size_t root_node{};
	bool child_active{false}; // true when any descendant of ValueBuilder is open
	size_t active_depth{}; // depth of the innermost currently-active builder
						   // 1 = direct child of ValueBuilder, 2 = grandchild, etc.
};

// Describes where a committed child node gets placed in the parent.
struct ParentSlot {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : u8 {
		set_root, // top-level: store in BuilderState::root_node
		insert_member, // nested in ObjectBuilder: push to object_members
		append_child, // nested in ArrayBuilder: push parent's local_children
	};
	Kind kind{Kind::set_root};
	size_t name_off{}; // insert_member: name offset in string_arena
	size_t name_len{}; // insert_member: name length
	size_t arena_start{}; // rollback point for string_arena
	bool saved_root_set{}; // set_root only: root_set value before child was opened
	vector<size_t> *parent_local_children{}; // append_child only: parent's staging vector
	vector<MemberEntry> *parent_local_members{}; // insert_member only: parent's staging vector
};

// Holds the active object/array being built:
struct ChildFrame {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : u8 {
		object,
		array,
	};
	Kind kind;
	size_t depth{}; // this builder's own depth level (1 = direct child of ValueBuilder)
	BuilderState *state{};
	bool committed{};
	ParentSlot parent; // parent.arena_start is the rollback point for string_arena
	vector<size_t> local_children; // staged array child node indices (array builders only)
	vector<MemberEntry> local_members; // staged object members (object builders only)
	// Per-session duplicate detection for ObjectBuilder (kind==object only).
	unordered_map<string, size_t> dup_check;
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
			  .dup_check = {}} {}

	[[nodiscard]] expected<void, JsonError> check_can_insert() const {
		if (frame_.committed) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "ObjectBuilder already committed"});
		}
		if (frame_.state != nullptr && frame_.state->active_depth != frame_.depth) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "child builder already active"});
		}
		return {};
	}

	expected<void, JsonError> do_insert_node(string_view name, size_t node_idx);
	expected<void, JsonError> do_insert_node_view(string_view name, size_t node_idx);

public:
	ObjectBuilder(
		ObjectBuilder &&o) noexcept
		: frame_{move(o.frame_)} {
		o.frame_.state = nullptr;
	}
	ObjectBuilder &operator =(
		ObjectBuilder &&o) noexcept {
		if (this != &o) {
			abort_if_open();
			frame_ = move(o.frame_);
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
			st->built_input.resize(frame_.parent.arena_start);
			frame_.local_members.clear();
			frame_.dup_check.clear();
			st->active_depth = frame_.depth - 1;
			if (frame_.parent.kind == ParentSlot::Kind::set_root) {
				st->root_set = frame_.parent.saved_root_set;
				st->child_active = false;
			}
			frame_.state = nullptr;
		}
	}

	~ObjectBuilder() noexcept { abort_if_open(); }

	expected<void, JsonError> insert_null(string_view name);
	expected<void, JsonError> insert_bool(string_view name, bool v);
	expected<void, JsonError> insert_string(string_view name, string_view value);
	expected<void, JsonError> insert_string_view(string_view name, string_view value); // Item E: name not copied
	expected<void, JsonError> insert_number(string_view name, string_view lexeme);
	expected<void, JsonError> insert_i64(string_view name, int64_t v);
	expected<void, JsonError> insert_u64(string_view name, uint64_t v);
	expected<void, JsonError> insert_f64(string_view name, double v);

	expected<ObjectBuilder, JsonError> insert_object(string_view name);
	expected<ArrayBuilder, JsonError> insert_array(string_view name);

	template<class T>
		requires has_json_codec<T>
	expected<void, JsonError> insert(string_view name, T const &value);

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept {
		if ((frame_.state == nullptr) || frame_.committed) {
			return;
		}
		auto *st = frame_.state;
		size_t const mem_start = st->store.object_members.size();
		for (auto const &m: frame_.local_members) {
			st->store.object_members.push_back(m);
		}
		size_t const cnt = frame_.local_members.size();
		st->store.nodes.push_back(detail::make_object(static_cast<u32>(mem_start), static_cast<u32>(cnt)));
		size_t const node_idx = st->store.nodes.size() - 1;
		switch (frame_.parent.kind) {
		case ParentSlot::Kind::set_root:
			st->root_node = node_idx;
			st->child_active = false;
			break;
		case ParentSlot::Kind::insert_member:
			frame_.parent.parent_local_members->push_back(
				{static_cast<u32>(frame_.parent.name_off),
				 static_cast<u32>(frame_.parent.name_len),
				 static_cast<u32>(node_idx),
				 kStorageInputView});
			break;
		case ParentSlot::Kind::append_child: frame_.parent.parent_local_children->push_back(node_idx); break;
		}
		st->active_depth = frame_.depth - 1;
		frame_.local_members.clear();
		frame_.dup_check.clear();
		frame_.committed = true;
	}
};

export class ArrayBuilder {
	ChildFrame frame_;

	[[nodiscard]] static bool arr_check_active(
		ChildFrame const &f) noexcept {
		return !f.committed && (f.state != nullptr) && (f.state->active_depth == f.depth);
	}

	friend class ValueBuilder;
	friend class ObjectBuilder;
	ArrayBuilder(
		BuilderState *st,
		ParentSlot parent = {})
		: frame_{
			  .kind = ChildFrame::Kind::array,
			  .state = st,
			  .parent = parent,
			  .local_children = {},
			  .local_members = {},
			  .dup_check = {}} {}

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void abort_if_open() noexcept {
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

public:
	ArrayBuilder(
		ArrayBuilder &&o) noexcept
		: frame_{move(o.frame_)} {
		o.frame_.state = nullptr;
	}
	ArrayBuilder &operator =(
		ArrayBuilder &&o) noexcept {
		if (this != &o) {
			abort_if_open();
			frame_ = move(o.frame_);
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
		requires has_json_codec<T>
	expected<void, JsonError> append(T const &value);

	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept {
		if ((frame_.state == nullptr) || frame_.committed) {
			return;
		}
		auto *st = frame_.state;
		size_t const child_start = st->store.array_children.size();
		for (size_t const idx: frame_.local_children) {
			st->store.array_children.push_back(static_cast<u32>(idx));
		}
		size_t const cnt = frame_.local_children.size();
		st->store.nodes.push_back(detail::make_array(static_cast<u32>(child_start), static_cast<u32>(cnt)));
		size_t const node_idx = st->store.nodes.size() - 1;
		switch (frame_.parent.kind) {
		case ParentSlot::Kind::set_root:
			st->root_node = static_cast<u32>(node_idx);
			st->child_active = false;
			break;
		case ParentSlot::Kind::insert_member:
			frame_.parent.parent_local_members->push_back(
				{static_cast<u32>(frame_.parent.name_off),
				 static_cast<u32>(frame_.parent.name_len),
				 static_cast<u32>(node_idx),
				 kStorageInputView});
			break;
		case ParentSlot::Kind::append_child: frame_.parent.parent_local_children->push_back(node_idx); break;
		}
		st->active_depth = frame_.depth - 1;
		frame_.local_children.clear();
		frame_.committed = true;
	}
};

// ---------------------------------------------------------------------------
// ValueBuilder
// ---------------------------------------------------------------------------

namespace detail {

template<class T>
expected<size_t, JsonError> encode_into(BuilderState *st, T const &value);

template<class T>
expected<void, JsonError> encode_dispatch(ValueBuilder &b, T const &value);

} // namespace detail

export class ValueBuilder {
	unique_ptr<BuilderState> owned_;
	BuilderState *state_{};

	friend class ObjectBuilder;
	friend class ArrayBuilder;
	template<class T>
	friend expected<size_t, JsonError> detail::encode_into(BuilderState *, T const &);

	explicit ValueBuilder(
		BuilderState *borrowed) noexcept
		: state_{borrowed} {}

	[[nodiscard]] expected<void, JsonError> check_can_set() const {
		if (state_ == nullptr) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::constraint_violation,
					.message = "ValueBuilder has been discarded"});
		}
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

public:
	ValueBuilder()
		: owned_{make_unique<BuilderState>()}
		, state_{owned_.get()} {}
	ValueBuilder(
		ValueBuilder &&o) noexcept
		: owned_{move(o.owned_)}
		, state_{owned_ ? owned_.get() : o.state_} {
		o.state_ = nullptr;
	}
	ValueBuilder &operator =(
		ValueBuilder &&o) noexcept {
		if (this != &o) {
			owned_ = move(o.owned_);
			state_ = owned_ ? owned_.get() : o.state_;
			o.state_ = nullptr;
		}
		return *this;
	}
	ValueBuilder(ValueBuilder const &) = delete;
	ValueBuilder &operator =(ValueBuilder const &) = delete;

	expected<void, JsonError> set_null() { return set_node(detail::make_null()); }
	expected<void, JsonError> set_bool(
		bool v) {
		return set_node(detail::make_bool(v));
	}

	expected<void, JsonError> set_string(
		string_view sv) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->built_input.size();
		state_->built_input.append(sv.data(), sv.size());
		return set_node(detail::make_string(static_cast<u32>(off), static_cast<u32>(sv.size()), kStorageInputView));
	}

	expected<void, JsonError> set_number(string_view lexeme);

	expected<void, JsonError> set_i64(
		int64_t v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->built_input.size();
		array<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->built_input.size() - off;
		return set_node(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v));
	}

	expected<void, JsonError> set_u64(
		uint64_t v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		size_t const off = state_->built_input.size();
		array<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->built_input.size() - off;
		if (v <= static_cast<uint64_t>(numeric_limits<int64_t>::max())) {
			return set_node(
				detail::make_number_int(
					static_cast<u32>(off),
					static_cast<u32>(len),
					kStorageInputView | kRawJsonSlice,
					static_cast<int64_t>(v)));
		}
		return set_node(
			detail::make_number_uint(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v));
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
		size_t const off = state_->built_input.size();
		array<char, 32> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
		size_t const len = state_->built_input.size() - off;
		string_view const lex = string_view{state_->built_input.data() + off, len};
		bool const is_int = lex.find_first_of(".eE") == string_view::npos;
		return set_node(
			detail::make_number_f64(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v,
				is_int));
	}

	[[nodiscard]] expected<ObjectBuilder, JsonError> begin_object() {
		auto ok = check_can_set();
		if (!ok) {
			return unexpected(move(ok).error());
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

	[[nodiscard]] expected<ArrayBuilder, JsonError> begin_array() {
		auto ok = check_can_set();
		if (!ok) {
			return unexpected(move(ok).error());
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

	template<class T>
		requires has_json_codec<T>
	expected<void, JsonError> set(T const &value);

	void reset() noexcept {
		state_->store = DocumentStorage{};
		state_->built_input.clear();
		state_->root_set = false;
		state_->root_node = 0;
		state_->child_active = false;
		state_->active_depth = 0;
	}

	void discard() && noexcept {
		owned_.reset();
		state_ = nullptr;
	}

	[[nodiscard]] expected<Document, JsonError> finish() && {
		if ((state_ == nullptr) || !state_->root_set || state_->child_active) {
			return unexpected(
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
		constexpr size_t kU32Ceiling = (size_t{1} << 32) - 1;
		if (state_->built_input.size() >= kU32Ceiling) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::input_too_large,
					.message = "Builder input buffer exceeds 4 GiB hard ceiling"});
		}
		auto storage = make_unique<DocumentStorage>(move(state_->store));
		storage->root_node = static_cast<u32>(state_->root_node);
		storage->owned_input = make_unique<string>(move(state_->built_input));
		storage->input_view = *storage->owned_input;
		owned_.reset();
		state_ = nullptr;
		return ::make_document(move(storage));
	}
};

export ValueBuilder value_builder() {
	return {};
}

// Internal helpers: encode a value of type T into a shared BuilderState,
// returning the resulting node index. Rolls back on failure.
// Used by ArrayBuilder::append<T> and ObjectBuilder::insert<T>.
namespace detail {

template<class T>
expected<size_t, JsonError> encode_into(
	BuilderState *st,
	T const &value) {
	size_t const nodes_saved = st->store.nodes.size();
	size_t const arena_saved = st->built_input.size();
	size_t const arr_saved = st->store.array_children.size();
	size_t const obj_saved = st->store.object_members.size();
	bool const root_set_saved = st->root_set;
	bool const child_active_saved = st->child_active;
	size_t const active_depth_saved = st->active_depth;

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
		return unexpected(move(ok).error());
	}
	size_t const node_idx = st->root_node;
	st->root_set = root_set_saved;
	st->child_active = child_active_saved;
	st->active_depth = active_depth_saved;
	return node_idx;
}

} // namespace detail

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
	size_t const off = state_->built_input.size();
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
	string_view name,
	size_t node_idx) {
	auto *st = frame_.state;
	auto [it, inserted] = frame_.dup_check.try_emplace(string{name}, node_idx);
	if (!inserted) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = string{name},
				.message = format("duplicate member: {}", name)});
	}
	size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.local_members.push_back(
		{static_cast<u32>(name_off), static_cast<u32>(name.size()), static_cast<u32>(node_idx), kStorageInputView});
	return {};
}

expected<void, JsonError> ObjectBuilder::do_insert_node_view(
	string_view name,
	size_t node_idx) {
	auto [it, inserted] = frame_.dup_check.try_emplace(string{name}, node_idx);
	if (!inserted) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = string{name},
				.message = format("duplicate member: {}", name)});
	}
	MemberEntry m{};
	m.name_len = static_cast<u32>(name.size());
	m.val_node = static_cast<u32>(node_idx);
	m.name_flags = kMemberExternalView;
	m.name_ptr = name.data();
	frame_.local_members.push_back(m);
	return {};
}

expected<void, JsonError> ObjectBuilder::insert_null(
	string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_null());
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_bool(
	string_view name,
	bool v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	st->store.nodes.push_back(detail::make_bool(v));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string(
	string_view name,
	string_view value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_string_view(
	string_view name,
	string_view value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	return do_insert_node_view(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_number(
	string_view name,
	string_view lexeme) {
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
	size_t const off = st->built_input.size();
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
	string_view name,
	int64_t v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	st->store.nodes.push_back(
		detail::make_number_int(static_cast<u32>(off), static_cast<u32>(len), kStorageInputView | kRawJsonSlice, v));
	return do_insert_node(name, st->store.nodes.size() - 1);
}
expected<void, JsonError> ObjectBuilder::insert_u64(
	string_view name,
	uint64_t v) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	if (v <= static_cast<uint64_t>(numeric_limits<int64_t>::max())) {
		st->store.nodes.push_back(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<int64_t>(v)));
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
	string_view name,
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
	size_t const off = st->built_input.size();
	array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	string_view const lex = string_view{st->built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == string_view::npos;
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
	string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return unexpected(move(ok).error());
	}
	// Duplicate check before any work (O(1) amortized via hash).
	if (frame_.dup_check.contains(string{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = string{name},
				.message = format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.dup_check.emplace(string{name}, 0);
	size_t const child_depth = frame_.depth + 1;
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
	string_view name) {
	if (auto ok = check_can_insert(); !ok) {
		return unexpected(move(ok).error());
	}
	// Duplicate check before any work (O(1) amortized via hash).
	if (frame_.dup_check.contains(string{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = string{name},
				.message = format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	// Store name in arena; the member entry will be pushed when child commits.
	size_t const name_off = st->built_input.size();
	st->built_input.append(name.data(), name.size());
	frame_.dup_check.emplace(string{name}, 0);
	size_t const child_depth = frame_.depth + 1;
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
	string_view value) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	st->built_input.append(value.data(), value.size());
	st->store.nodes.push_back(
		detail::make_string(static_cast<u32>(off), static_cast<u32>(value.size()), kStorageInputView));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_number(
	string_view lexeme) {
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
	size_t const off = st->built_input.size();
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
	int64_t v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	st->store.nodes.push_back(
		detail::make_number_int(static_cast<u32>(off), static_cast<u32>(len), kStorageInputView | kRawJsonSlice, v));
	frame_.local_children.push_back(st->store.nodes.size() - 1);
	return {};
}
expected<void, JsonError> ArrayBuilder::append_u64(
	uint64_t v) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	size_t const off = st->built_input.size();
	array<char, 22> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	if (v <= static_cast<uint64_t>(numeric_limits<int64_t>::max())) {
		st->store.nodes.push_back(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				static_cast<int64_t>(v)));
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
	size_t const off = st->built_input.size();
	array<char, 32> buf{};
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
	st->built_input.append(buf.data(), static_cast<size_t>(p - buf.data()));
	size_t const len = st->built_input.size() - off;
	string_view const lex = string_view{st->built_input.data() + off, len};
	bool const is_int = lex.find_first_of(".eE") == string_view::npos;
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
	size_t const child_depth = frame_.depth + 1;
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
	size_t const child_depth = frame_.depth + 1;
	st->active_depth = child_depth;
	ParentSlot const parent{
		.kind = ParentSlot::Kind::append_child,
		.arena_start = st->built_input.size(),
		.parent_local_children = &frame_.local_children};
	ArrayBuilder child{st, parent};
	child.frame_.depth = child_depth;
	return child;
}

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
// JsonCodec / JsonMembers / decode
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

namespace detail {

template<class T>
struct has_codec_spec<
	T,
	void_t<
		decltype(JsonCodec<T>::decode(declval<NodeRef>())),
		decltype(JsonCodec<T>::encode(declval<ValueBuilder &>(), declval<T const &>()))>> : true_type {};

template<class T>
struct has_members_spec<T, void_t<decltype(JsonMembers<T>::members())>> : bool_constant<default_initializable<T>> {};

} // namespace detail

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
template<class T, size_t N>
struct JsonCodec<array<T, N>>;
template<class A, class B>
struct JsonCodec<pair<A, B>>;
template<class... Ts>
struct JsonCodec<tuple<Ts...>>;
template<class T>
struct JsonCodec<map<string, T>>;
template<class T>
struct JsonCodec<unordered_map<string, T>>;

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
			if constexpr (detail::is_nullable_type<T>::value) {
				auto v = ::decode<T>(n);
				if (!v) {
					return unexpected(move(v).error());
				}
				return optional<T>{move(*v)};
			} else {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::wrong_kind,
						.expected_kind = JsonKind::null,
						.actual_kind = JsonKind::null,
						.message =
							"explicit JSON null is not accepted for optional<T>; use Nullable<T> for nullable fields"});
			}
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
		return JsonCodec<T>::encode(b, *v);
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
		return JsonCodec<T>::encode(b, v.value());
	}
	static constexpr string_view type_name() { return "Nullable"; }
};

namespace detail {

template<class T>
expected<vector<T>, JsonError> decode_array_elements(
	ArrayView const &arr) {
	vector<T> result;
	result.reserve(arr.size());
	for (size_t i = 0; i < arr.size(); ++i) {
		auto elem = arr.element(i);
		if (!elem) {
			return unexpected(move(elem).error());
		}
		auto v = ::decode<T>(*elem);
		if (!v) {
			JsonPath prefix;
			prefix.push_index(i);
			return unexpected(move(v).error().with_prefix(prefix));
		}
		result.push_back(move(*v));
	}
	return result;
}

} // namespace detail

template<class T>
struct JsonCodec<vector<T>> {
	static expected<vector<T>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		return detail::decode_array_elements<T>(*arr);
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		vector<T> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		for (auto const &elem: v) {
			if (auto ok = arr.template append<T>(elem); !ok) {
				return unexpected(move(ok).error());
			}
		}
		move(arr).commit();
		return {};
	}
	static constexpr string_view type_name() { return "vector"; }
};

template<class T, size_t N>
struct JsonCodec<array<T, N>> {
	static expected<array<T, N>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		if (arr->size() != N) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = string{type_name()},
					.container_size = N,
					.message = format("expected array of length {}, got {}", N, arr->size())});
		}
		array<T, N> result{};
		for (size_t i = 0; i < N; ++i) {
			auto elem = arr->element(i);
			if (!elem) {
				return unexpected(move(elem).error());
			}
			auto v = ::decode<T>(*elem);
			if (!v) {
				JsonPath prefix;
				prefix.push_index(i);
				return unexpected(move(v).error().with_prefix(prefix));
			}
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			result[i] = move(*v);
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		array<T, N> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		for (size_t i = 0; i < N; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
			if (auto ok = arr.template append<T>(v[i]); !ok) {
				return unexpected(move(ok).error());
			}
		}
		move(arr).commit();
		return {};
	}
	static constexpr string_view type_name() { return "array"; }
};

template<class A, class B>
struct JsonCodec<pair<A, B>> {
	static expected<pair<A, B>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		if (arr->size() != 2) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = string{type_name()},
					.container_size = 2UZ,
					.message = format("expected array of length 2, got {}", arr->size())});
		}
		auto e0 = arr->element(0);
		if (!e0) {
			return unexpected(move(e0).error());
		}
		auto first = ::decode<A>(*e0);
		if (!first) {
			JsonPath prefix;
			prefix.push_index(0);
			return unexpected(move(first).error().with_prefix(prefix));
		}
		auto e1 = arr->element(1);
		if (!e1) {
			return unexpected(move(e1).error());
		}
		auto second = ::decode<B>(*e1);
		if (!second) {
			JsonPath prefix;
			prefix.push_index(1);
			return unexpected(move(second).error().with_prefix(prefix));
		}
		return pair<A, B>{move(*first), move(*second)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		pair<A, B> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		if (auto ok = arr.template append<A>(v.first); !ok) {
			return unexpected(move(ok).error());
		}
		if (auto ok = arr.template append<B>(v.second); !ok) {
			return unexpected(move(ok).error());
		}
		move(arr).commit();
		return {};
	}
	static constexpr string_view type_name() { return "pair"; }
};

template<class... Ts>
struct JsonCodec<tuple<Ts...>> {
	static expected<tuple<Ts...>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		constexpr size_t N = sizeof...(Ts);
		if (arr->size() != N) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = string{type_name()},
					.container_size = N,
					.message = format("expected array of length {}, got {}", N, arr->size())});
		}
		tuple<Ts...> result{};
		bool ok = true;
		JsonError first_err;
		[&]<size_t... Is>(index_sequence<Is...>) {
			(([&]<size_t I>() {
				 if (!ok) {
					 return;
				 }
				 auto elem = arr->element(I);
				 if (!elem) {
					 ok = false;
					 first_err = move(elem).error();
					 return;
				 }
				 auto v = ::decode<tuple_element_t<I, tuple<Ts...>>>(*elem);
				 if (!v) {
					 ok = false;
					 JsonPath prefix;
					 prefix.push_index(I);
					 first_err = move(v).error().with_prefix(prefix);
					 return;
				 }
				 get<I>(result) = move(*v);
			 }.template operator ()<Is>()),
			 ...);
		}(make_index_sequence<N>{});
		if (!ok) {
			return unexpected(move(first_err));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		tuple<Ts...> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		bool ok = true;
		JsonError first_err;
		[&]<size_t... Is>(index_sequence<Is...>) {
			(([&]<size_t I>() {
				 if (!ok) {
					 return;
				 }
				 auto res = arr.template append<tuple_element_t<I, tuple<Ts...>>>(get<I>(v));
				 if (!res) {
					 ok = false;
					 first_err = move(res).error();
				 }
			 }.template operator ()<Is>()),
			 ...);
		}(make_index_sequence<sizeof...(Ts)>{});
		if (!ok) {
			return unexpected(move(first_err));
		}
		move(arr).commit();
		return {};
	}
	static constexpr string_view type_name() { return "tuple"; }
};

template<class T>
struct JsonCodec<map<string, T>> {
	static expected<map<string, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		map<string, T> result;
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return unexpected(move(v).error().with_prefix(prefix));
			}
			result.emplace(string{name}, move(*v));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		map<string, T> const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return unexpected(move(obj_res).error());
		}
		auto &obj = *obj_res;
		for (auto const &[key, val]: v) {
			if (auto ok = obj.template insert<T>(key, val); !ok) {
				return unexpected(move(ok).error());
			}
		}
		move(obj).commit();
		return {};
	}
	static constexpr string_view type_name() { return "map"; }
};

template<class T>
struct JsonCodec<unordered_map<string, T>> {
	static expected<unordered_map<string, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		unordered_map<string, T> result;
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return unexpected(move(v).error().with_prefix(prefix));
			}
			result.emplace(string{name}, move(*v));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		unordered_map<string, T> const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return unexpected(move(obj_res).error());
		}
		auto &obj = *obj_res;
		for (auto const &[key, val]: v) {
			if (auto ok = obj.template insert<T>(key, val); !ok) {
				return unexpected(move(ok).error());
			}
		}
		move(obj).commit();
		return {};
	}
	static constexpr string_view type_name() { return "unordered_map"; }
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
					 using M = remove_reference_t<decltype(result.*m.pointer)>;
					 auto val = obj->find_member(m.name);
					 if (!val) {
						 if constexpr (detail::is_optional<M>::value) {
							 result.*m.pointer = M{};
						 } else {
							 ok = false;
							 first_err = JsonError{
								 .stage = JsonStage::decode,
								 .code = JsonIssueCode::missing_member,
								 .member_name = string{m.name},
								 .message = format("missing member: {}", m.name)};
						 }
						 return;
					 }
					 auto decoded = decode<M>(*val);
					 if (!decoded) {
						 ok = false;
						 JsonPath prefix;
						 prefix.push_member(m.name);
						 first_err = move(decoded).error().with_prefix(prefix);
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

// ---------------------------------------------------------------------------
// Generic builder methods — defined after codec specializations
// ---------------------------------------------------------------------------

template<class T>
	requires has_json_codec<T>
expected<void, JsonError> ArrayBuilder::append(
	T const &value) {
	if (!arr_check_active(frame_)) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::constraint_violation,
				.message = frame_.committed ? "ArrayBuilder already committed" : "child builder already active"});
	}
	auto *st = frame_.state;
	auto node_or = detail::encode_into<T>(st, value);
	if (!node_or) {
		return unexpected(move(node_or).error());
	}
	frame_.local_children.push_back(*node_or);
	return {};
}

template<class T>
	requires has_json_codec<T>
expected<void, JsonError> ObjectBuilder::insert(
	string_view name,
	T const &value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	// Spec: duplicate-name rejection happens before dispatching to JsonCodec<T>::encode.
	if (frame_.dup_check.contains(string{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = string{name},
				.message = format("duplicate member: {}", name)});
	}
	auto *st = frame_.state;
	auto node_or = detail::encode_into<T>(st, value);
	if (!node_or) {
		return unexpected(move(node_or).error());
	}
	return do_insert_node(name, *node_or);
}

namespace detail {

// Encode dispatch: mirrors the decode<T> dispatch logic.
template<class T>
expected<void, JsonError> encode_dispatch(
	ValueBuilder &b,
	T const &value) {
	if constexpr (has_codec_spec<T>::value) {
		return JsonCodec<T>::encode(b, value);
	} else if constexpr (has_members_spec<T>::value) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return unexpected(move(obj_res).error());
		}
		auto &obj = *obj_res;
		auto const members = JsonMembers<T>::members();
		bool ok = true;
		JsonError first_err;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &m) {
					 if (!ok) {
						 return;
					 }
					 using M = remove_cvref_t<decltype(value.*m.pointer)>;
					 auto res = obj.template insert<M>(m.name, value.*m.pointer);
					 if (!res) {
						 ok = false;
						 first_err = move(res).error();
					 }
				 })(ms),
				 ...);
			},
			members);
		if (!ok) {
			return unexpected(move(first_err));
		}
		move(obj).commit();
		return {};
	} else {
		static_assert(false, "No JsonCodec<T> or JsonMembers<T> found for T");
	}
}

} // namespace detail

template<class T>
	requires has_json_codec<T>
expected<void, JsonError> ValueBuilder::set(
	T const &value) {
	if (auto ok = check_can_set(); !ok) {
		return ok;
	}
	return detail::encode_dispatch<T>(*this, value);
}
