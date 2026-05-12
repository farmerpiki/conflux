module;
#include <cassert>
#include <locale.h>
// stdlib.h and sys/random.h pull in pthreadtypes.h which conflicts with the
// std module BMI under GCC -freflection; forward-declare what we need instead
extern "C" {
double strtod_l(char const *, char **, ::locale_t) noexcept;
long getrandom(void *, unsigned long, unsigned int);
}
#include <xxhash.h>
// <immintrin.h> → mm_malloc.h → stdlib.h → pthreadtypes.h conflicts with the
// std module BMI under GCC -freflection; scalar fallback used in that build.
#if defined(CONFLUX_JSON_USE_STDSIMD)
	#include <cstddef>
	#define CONFLUX_JSON_HAS_STDSIMD 1
extern "C" {
std::size_t conflux_json_scan_str_until_special_stdsimd(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd(char const *, std::size_t, int) noexcept;
}
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(__cpp_impl_reflection)
	#include <immintrin.h>
	#ifndef CONFLUX_JSON_DISABLE_SIMD
		#define CONFLUX_JSON_HAS_SSE2 1
		#if defined(__AVX2__)
			#define CONFLUX_JSON_HAS_AVX2 1
		#endif
	#endif
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
export module conflux.json;
import std;
import std.compat;
import conflux.types;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

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
export class JsonStringToken;
export class JsonReader;
export struct JsonArenaOptions;
export class ArenaDocument;
export class JsonArena;
export class NdjsonRange;
export class JsonAccumulator;

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
	SZ offset{};
	SZ line{1};
	SZ column{1};
};

// NOLINTNEXTLINE(performance-enum-size)
export enum class DuplicateKeyPolicy : u8 {
	reject, // RFC 8259 recommended; current default
	last_wins, // keep last value; first occurrence's name position preserved
	first_wins, // keep first value; duplicate parsed for syntax, then discarded
};
// ---------------------------------------------------------------------------
// JsonPath
// ---------------------------------------------------------------------------

export struct JsonPathMember {
	S name;
};
export struct JsonPathIndex {
	SZ index{};
};
export using JsonPathSegment = variant<JsonPathMember, JsonPathIndex>;
export class JsonPath {
	V<JsonPathSegment> segs_;

public:
	static JsonPath root() { return {}; }
	JsonPath() = default;
	JsonPath(JsonPath const &) = default;
	JsonPath(JsonPath &&) noexcept = default;
	JsonPath &operator =(JsonPath const &) = default;
	JsonPath &operator =(JsonPath &&) noexcept = default;
	[[nodiscard]] bool empty() const noexcept { return segs_.empty(); }
	[[nodiscard]] SZ size() const noexcept { return segs_.size(); }
	void reserve(
		SZ n) {
		segs_.reserve(n);
	}
	void push_member(
		SV name) {
		segs_.emplace_back(JsonPathMember{S{name}});
	}
	void push_index(
		SZ idx) {
		segs_.emplace_back(JsonPathIndex{idx});
	}
	void pop() noexcept {
		if (!segs_.empty()) {
			segs_.pop_back();
		}
	}
	[[nodiscard]] JsonPathSegment const &segment(
		SZ i) const {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		return segs_[i];
	}
	[[nodiscard]] auto begin() const noexcept { return segs_.begin(); }
	[[nodiscard]] auto end() const noexcept { return segs_.end(); }
	[[nodiscard]] S to_pointer() const {
		if (segs_.empty()) {
			return "";
		}
		S out;
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
	static expected<JsonPath, JsonError> from_pointer(SV sv);

	bool friend operator ==(JsonPath const &, JsonPath const &) = default;
};
template<>
struct std::hash<JsonPath> {
	SZ operator ()(
		JsonPath const &p) const noexcept {
		SZ h = 0;
		for (auto const &seg: p) {
			SZ const sh = holds_alternative<JsonPathMember>(seg) ? hash<S>{}(get<JsonPathMember>(seg).name) :
																   hash<SZ>{}(get<JsonPathIndex>(seg).index);
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
	Opt<JsonSourceLocation> source{};
	Opt<JsonKind> expected_kind{};
	Opt<JsonKind> actual_kind{};
	Opt<S> member_name{};
	Opt<S> target_type{};
	Opt<SZ> requested_index{};
	Opt<SZ> container_size{};
	S message{};
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
	SV sv) {
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
	SZ pos = 1;
	while (pos <= sv.size()) {
		SZ slash = sv.find('/', pos);
		if (slash == SV::npos) {
			slash = sv.size();
		}
		S name;
		name.reserve(slash - pos);
		for (SZ i = pos; i < slash; ++i) {
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
	SZ value_{};

public:
	constexpr LimitOption() noexcept = default;
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr LimitOption(
		NoLimit) noexcept
		: tag_{Tag::unlimited} {}
	constexpr explicit LimitOption(
		SZ v) noexcept
		: tag_{Tag::bound}
		, value_{v} {}
	[[nodiscard]] static constexpr LimitOption bound(
		SZ v) noexcept {
		return LimitOption{v};
	}
	[[nodiscard]] constexpr bool is_default() const noexcept { return tag_ == Tag::default_; }
	[[nodiscard]] constexpr bool is_unlimited() const noexcept { return tag_ == Tag::unlimited; }
	[[nodiscard]] constexpr Opt<SZ> explicit_value() const noexcept {
		if (tag_ == Tag::bound) {
			return value_;
		}
		return nullopt;
	}
	[[nodiscard]] constexpr bool exceeds(
		SZ n,
		SZ default_cap) const noexcept {
		if (tag_ == Tag::unlimited) {
			return false;
		}
		if (tag_ == Tag::default_) {
			return n > default_cap;
		}
		return n > value_;
	}
};

export enum class ParseMode : u8 {
	strict,
	json5,
};
export struct JsonParseOptions {
	LimitOption max_depth;
	LimitOption max_input_size;
	LimitOption max_string_size;
	DuplicateKeyPolicy duplicate_key{DuplicateKeyPolicy::reject};
	Opt<u32> warm_threshold{};
	ParseMode mode{ParseMode::strict};
};

// NOLINTNEXTLINE(performance-enum-size)
export enum class UnknownMemberPolicy : u8 {
	reject,
	ignore,
};
export struct JsonDecodeOptions {
	UnknownMemberPolicy unknown_members{UnknownMemberPolicy::reject};
};
export struct JsonByteRange {
	SZ start;
	SZ end;
};

// ---------------------------------------------------------------------------
// Internal storage
// ---------------------------------------------------------------------------

enum class NodeKind : u8 {
	null_,
	boolean,
	number,
	string_,
	array_,
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
constexpr u8 kValueExternalView = 0x80; // off indexes external_ptrs_, value is caller-owned (string nodes only)

// Number value-kind flags (at most one of kValKind* set on a number node).
constexpr u8 kLexIntForm = 0x08; // lexeme matches -?(0|[1-9][0-9]*)
constexpr u8 kValKindInt = 0x10; // ival valid
constexpr u8 kValKindUint = 0x20; // uval valid
constexpr u8 kValKindF64 = 0x40; // dval valid
constexpr u8 kValKindDeferred = 0x04; // range-error f64 ≤ 4 KiB; from_chars deferred to to_f64()

// All three kValKind* clear on a number node = f64-overflow (lexeme preserved).

// kMemberExternalView: name is caller-owned. name_off indexes DocumentStorage::external_ptrs_.
constexpr u32 kMemberExternalView = 0x04u;
struct MemberEntry {
	u32 name_off; // arena offset; or external_ptrs_ index when kMemberExternalView
	u32 name_len;
	u32 val_node;
	u32 name_flags; // 0=arena; kStorageInputView=0x01; kMemberExternalView=0x04
};
static_assert(sizeof(MemberEntry) == 16);
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
	std::pmr::memory_resource *mr;
	[[nodiscard]] ObjHashSlot *slots_data() noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<ObjHashSlot *>(this + 1);
	}
	[[nodiscard]] ObjHashSlot const *slots_data() const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<ObjHashSlot const *>(this + 1);
	}
	// ptr_cache follows slots: one char const* per member (build_table fills all member_count
	// entries before the table is published; lookup_in reads via s.member_index).
	[[nodiscard]] char const **ptr_cache_data() noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<char const **>(slots_data() + capacity);
	}
	[[nodiscard]] char const *const *ptr_cache_data() const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		return reinterpret_cast<char const *const *>(slots_data() + capacity);
	}
	static ObjHashTable *create(
		u32 capacity,
		u32 member_count,
		std::pmr::memory_resource *mr = std::pmr::new_delete_resource()) noexcept {
		SZ const bytes = sizeof(ObjHashTable) + sizeof(ObjHashSlot) * capacity + sizeof(char const *) * member_count;
		void *mem = nullptr;
		try {
			mem = mr->allocate(bytes, alignof(ObjHashTable));
		} catch (...) {}
		if (mem == nullptr) {
			return nullptr;
		}
		auto *t = ::new (mem) ObjHashTable{capacity, member_count, mr};
		std::fill_n(t->slots_data(), capacity, ObjHashSlot{});
		return t;
	}
	static void destroy(
		ObjHashTable *t) noexcept {
		if (t == nullptr) {
			return;
		}
		auto *mr = t->mr;
		SZ const bytes =
			sizeof(ObjHashTable) + sizeof(ObjHashSlot) * t->capacity + sizeof(char const *) * t->member_count;
		t->~ObjHashTable();
		mr->deallocate(t, bytes, alignof(ObjHashTable));
	}
};
constexpr u32 kHashThreshold = 32;
constexpr u32 kProbeChainMax = 64;
constexpr u32 kMaxHashTableCapacity = 1u << 30;
// FI-7 — practical byte budget on the per-object hash index to bound
// DoS payloads. 256 MiB / 8 B per slot = 32 Mi slots; well above any
// realistic object size.
constexpr SZ kMaxHashIndexBytes = 256ULL * 1024 * 1024;
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
//   kind == A                          → none (children via off/len)
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
[[nodiscard]] inline Node node_array(
	u32 off,
	u32 len) noexcept {
	return Node{.kind = NodeKind::array_, .flags = 0, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}
[[nodiscard]] inline Node node_object(
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
	if (::getrandom(&seed, sizeof(seed), 0) != static_cast<long>(sizeof(seed))) {
		seed = reinterpret_cast<uintptr_t>(&seed) ^ UINT64_C(0x517cc1b727220a95);
	}
	return seed;
}

} // namespace detail
struct DocumentStorage {
	std::pmr::vector<Node> nodes;
	std::pmr::string string_arena;
	std::pmr::vector<u32> array_children;
	std::pmr::vector<MemberEntry> object_members;
	V<char const *> external_ptrs_; // indexed by MemberEntry::name_off when kMemberExternalView
	UP<S> owned_input;
	SV input_view;
	u32 root_node{0};
	u32 bom_prefix_bytes{0};
	u64 hash_seed_{detail::make_hash_seed()};
	std::pmr::memory_resource *hash_mr_{std::pmr::new_delete_resource()};
	DocumentStorage()
		: nodes(std::pmr::new_delete_resource())
		, string_arena(std::pmr::new_delete_resource())
		, array_children(std::pmr::new_delete_resource())
		, object_members(std::pmr::new_delete_resource()) {}
	explicit DocumentStorage(
		std::pmr::memory_resource *r)
		: nodes(r)
		, string_arena(r)
		, array_children(r)
		, object_members(r)
		, hash_mr_(r) {}
	DocumentStorage(DocumentStorage const &) = delete;
	DocumentStorage &operator =(DocumentStorage const &) = delete;
	DocumentStorage(DocumentStorage &&) noexcept = default;
	DocumentStorage &operator =(DocumentStorage &&) noexcept = default;
	~DocumentStorage() noexcept {
		for (auto &n: nodes) {
			if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
				ObjHashTable::destroy(n.hash_idx_raw);
			}
		}
	}
	[[nodiscard]] SV str_at(
		u32 off,
		u32 len) const noexcept {
		return {string_arena.data() + off, len};
	}
	[[nodiscard]] SV bytes_at(
		u32 off,
		u32 len,
		u8 flags) const noexcept {
		if ((flags & kValueExternalView) != 0) {
			return {external_ptrs_[off], len};
		}
		if ((flags & kStorageInputView) != 0) {
			return input_view.substr(off, len);
		}
		return str_at(off, len);
	}
	[[nodiscard]] SV member_name(
		MemberEntry const &m) const noexcept {
		if ((m.name_flags & kMemberExternalView) != 0) {
			return {external_ptrs_[m.name_off], m.name_len};
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
constexpr SZ kSlowFloatLexemeCopyLimit = 4096;
constexpr SZ kMaxNumberLexemeLen = 1024;
constexpr SZ kDefaultMaxDepth = 128;
constexpr SZ kDefaultMaxInput = 128ULL * 1024 * 1024;
constexpr SZ kDefaultMaxString = 64ULL * 1024 * 1024;
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
	auto const n = static_cast<SZ>(last - first);
	if (n > kSlowFloatLexemeCopyLimit) {
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}
	double dv{};
	auto const [p, ec] = from_chars(first, last, dv, std::chars_format::general);
	if (ec == errc{} && p == last) {
		if (isfinite(dv)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::underflow_finite, dv};
		}
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}
	if (ec == errc::result_out_of_range) {
		// libc++ sets dv=inf for overflow; libstdc++ sets dv=0 for both cases.
		// When from_chars is informative (isinf), use it directly.
		if (isinf(dv)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
		}

		auto const &lh = c_locale_holder();
		if (!lh.ok) {
			return unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::resource_exhausted,
					.message = "newlocale(C) failed at startup; strtod_l unavailable"});
		}
		// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
		char stack_buf[128];
		// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
		UP<char[]> heap_buf;
		char *cp = nullptr;
		if (n + 1 <= sizeof(stack_buf)) {
			cp = stack_buf;
		} else {
			heap_buf = UP<char[]>{new (std::nothrow) char[n + 1]};
			if (!heap_buf) {
				return unexpected(
					JsonError{
						.stage = JsonStage::parse,
						.code = JsonIssueCode::resource_exhausted,
						.message = "OOM in classify_range_error_slow"});
			}
			cp = heap_buf.get();
		}
		std::copy_n(first, n, cp);
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		cp[n] = '\0';
		char *end = nullptr; // NOLINT(misc-const-correctness)
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		double const v = ::strtod_l(cp, &end, lh.loc);
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (end != cp + n) {
			return unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::invalid_number,
					.message = "strtod_l rejected deferred lexeme"});
		}
		if (isinf(v)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
		}
		return ClassifiedDouble{ClassifiedDouble::Kind::underflow_finite, v};
	}
	return unexpected(
		JsonError{
			.stage = JsonStage::parse,
			.code = JsonIssueCode::invalid_number,
			.message = format("from_chars rejected deferred lexeme: {}", SV{first, last})});
}
// Pre-parsed number factory: takes a syntactically valid JSON number lexeme
// (caller validates) plus its arena offset/length, runs the two-stage parse
// (i64 → u64 → f64 with from_chars fallback for range-error f64), and returns
// the appropriate Node.
[[nodiscard]] inline expected<Node, JsonError> build_number_node_from_lexeme(
	u32 off,
	u32 len,
	u8 storage_flags,
	SV lex) noexcept {
	bool const int_form = lex.find_first_of(".eE") == SV::npos;
	bool const neg = !lex.empty() && lex.front() == '-';
	auto const *b = lex.data();
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto const *e = lex.data() + lex.size();

	if (int_form) {
		i64 iv{};
		if (auto [p, ec] = from_chars(b, e, iv); ec == errc{} && p == e) {
			return make_number_int(off, len, storage_flags, iv);
		}
		if (!neg) {
			u64 uv{};
			if (auto [p2, ec2] = from_chars(b, e, uv); ec2 == errc{} && p2 == e) {
				return make_number_uint(off, len, storage_flags, uv);
			}
		}
	}

	double dv{};
	auto const [p, ec] = from_chars(b, e, dv, std::chars_format::general);
	if (ec == errc{} && p == e) {
		if (isfinite(dv)) {
			return make_number_f64(off, len, storage_flags, dv, int_form);
		}
		return make_number_overflow(off, len, storage_flags, int_form);
	}
	if (ec == errc::result_out_of_range) {
		if (static_cast<SZ>(e - b) > kSlowFloatLexemeCopyLimit) {
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
	SV lexeme_;
	u64 raw_payload_; // bit-cast<i64/u64/double> selected by flags_
	u8 flags_; // kLexIntForm | kValKindInt|Uint|F64

	friend class NodeRef;
	friend class JsonReader;
	bool friend is_value_equal(NodeRef, NodeRef);
	JsonNumberView(
		SV lex,
		u8 flags,
		u64 raw) noexcept
		: lexeme_{lex}
		, raw_payload_{raw}
		, flags_{flags} {}

public:
	[[nodiscard]] SV lexeme() const noexcept { return lexeme_; }
	[[nodiscard]] JsonNumberForm form() const noexcept {
		return (flags_ & kLexIntForm) != 0 ? JsonNumberForm::integer : JsonNumberForm::non_integer;
	}
	[[nodiscard]] expected<i64, JsonError> to_i64() const {
		if ((flags_ & kLexIntForm) == 0) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::invalid_number,
					.message = "to_i64 requires integer-form number"});
		}
		if ((flags_ & kValKindInt) != 0) {
			return std::bit_cast<i64>(raw_payload_);
		}
		// kValKindUint, kValKindF64, or no kValKind* (overflow): integer-form
		// lexeme outside i64 range → number_out_of_range (Correction K).
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = format("value out of i64 range: {}", lexeme_)});
	}
	[[nodiscard]] expected<u64, JsonError> to_u64() const {
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
			auto const v = std::bit_cast<i64>(raw_payload_);
			if (v >= 0) {
				return static_cast<u64>(v);
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
			return static_cast<double>(std::bit_cast<i64>(raw_payload_));
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
	template<class T>
		requires((std::integral<T> && !same_as<T, bool>) || std::floating_point<T>)
	[[nodiscard]] expected<T, JsonError> get_as() const {
		if constexpr (std::floating_point<T>) {
			return to_f64().transform([](double v) noexcept { return static_cast<T>(v); });
		} else if constexpr (std::is_signed_v<T>) {
			auto v = to_i64();
			if (!v) {
				return unexpected(move(v).error());
			}
			if constexpr (sizeof(T) < sizeof(i64)) {
				if (*v < static_cast<i64>(NL<T>::min()) || *v > static_cast<i64>(NL<T>::max())) {
					return unexpected(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::number_out_of_range,
							.message = format("value {} out of range for target type", lexeme_)});
				}
			}
			return static_cast<T>(*v);
		} else {
			auto v = to_u64();
			if (!v) {
				return unexpected(move(v).error());
			}
			if constexpr (sizeof(T) < sizeof(u64)) {
				if (*v > static_cast<u64>(NL<T>::max())) {
					return unexpected(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::number_out_of_range,
							.message = format("value {} out of range for target type", lexeme_)});
				}
			}
			return static_cast<T>(*v);
		}
	}
};
// Forward declarations for parser helpers used by JsonStringToken/JsonReader.
SZ utf8_seq_len(unsigned char lead) noexcept;
bool is_cont(unsigned char c) noexcept;
namespace detail::simd {

[[nodiscard]] SZ scan_str_until_special(char const *p, SZ n) noexcept;

} // namespace detail::simd
// ---------------------------------------------------------------------------
// decode_str_body helper (used by JsonStringToken)
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline Opt<u32> hex4_from_sv(
	SV body,
	SZ pos) noexcept {
	u32 out = 0;
	for (SZ i = 0; i < 4; ++i) {
		char const c = body[pos + i];
		u32 d;
		constexpr u32 kA = 10;
		if (c >= '0' && c <= '9') {
			d = static_cast<u32>(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			d = static_cast<u32>(c - 'a') + kA;
		} else if (c >= 'A' && c <= 'F') {
			d = static_cast<u32>(c - 'A') + kA;
		} else {
			return nullopt;
		}
		// NOLINTNEXTLINE(hicpp-signed-bitwise)
		out = (out << 4U) | d;
	}
	return out;
}
inline void append_utf8_to_sv(
	u32 cp,
	auto &&writer) {
	// NOLINTBEGIN(readability-magic-numbers,hicpp-signed-bitwise)
	char buf[4];
	SZ len = 0;
	if (cp < 0x80U) {
		buf[0] = static_cast<char>(cp);
		len = 1;
	} else if (cp < 0x800U) {
		buf[0] = static_cast<char>(0xC0U | (cp >> 6U));
		buf[1] = static_cast<char>(0x80U | (cp & 0x3FU));
		len = 2;
	} else if (cp < 0x10000U) {
		buf[0] = static_cast<char>(0xE0U | (cp >> 12U));
		buf[1] = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
		buf[2] = static_cast<char>(0x80U | (cp & 0x3FU));
		len = 3;
	} else {
		buf[0] = static_cast<char>(0xF0U | (cp >> 18U));
		buf[1] = static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
		buf[2] = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
		buf[3] = static_cast<char>(0x80U | (cp & 0x3FU));
		len = 4;
	}
	// NOLINTEND(readability-magic-numbers,hicpp-signed-bitwise)
	writer(SV{buf, len});
}
template<class Writer>
[[nodiscard]] expected<SZ, JsonError> decode_str_body(
	SV body,
	Writer &&writer,
	LimitOption max_sz)
	noexcept(
		false) {
	SZ total = 0;
	SZ i = 0;
	while (i < body.size()) {
		auto const c = static_cast<unsigned char>(body[i]);
		if (c != '\\') {
			SZ run_start = i;
			while (i < body.size() && static_cast<unsigned char>(body[i]) != '\\') {
				++i;
			}
			SV chunk = body.substr(run_start, i - run_start);
			writer(chunk);
			total += chunk.size();
			if (max_sz.exceeds(total, kDefaultMaxString)) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::string_too_large,
						.message = "decoded string exceeds max_string_size"});
			}
			continue;
		}
		++i; // skip '\\'
		if (i >= body.size()) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::syntax_error, .message = "EOF in escape"});
		}
		char esc_char[1]{};
		bool simple = true;
		switch (body[i]) {
		case '"' : esc_char[0] = '"'; break;
		case '\'': esc_char[0] = '\''; break;
		case '\\': esc_char[0] = '\\'; break;
		case '/' : esc_char[0] = '/'; break;
		case 'b' : esc_char[0] = '\b'; break;
		case 'f' : esc_char[0] = '\f'; break;
		case 'n' : esc_char[0] = '\n'; break;
		case 'r' : esc_char[0] = '\r'; break;
		case 't' : esc_char[0] = '\t'; break;
		case 'u':
			{
				simple = false;
				++i;
				if (i + 4 > body.size()) {
					return unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_unicode_escape,
							.message = "invalid \\uXXXX"});
				}
				auto cp_opt = hex4_from_sv(body, i);
				if (!cp_opt) {
					return unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_unicode_escape,
							.message = "invalid hex digit in \\uXXXX"});
				}
				u32 cp = *cp_opt;
				i += 4;
				// NOLINTBEGIN(readability-magic-numbers)
				if (cp >= 0xD800U && cp <= 0xDBFFU) {
					if (i + 6 > body.size() || body[i] != '\\' || body[i + 1] != 'u') {
						return unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "unpaired high surrogate"});
					}
					i += 2;
					auto lo_opt = hex4_from_sv(body, i);
					if (!lo_opt) {
						return unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "invalid hex digit in \\uXXXX"});
					}
					u32 const lo = *lo_opt;
					i += 4;
					if (lo < 0xDC00U || lo > 0xDFFFU) {
						return unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "invalid low surrogate"});
					}
					cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
				}
				// NOLINTEND(readability-magic-numbers)
				SZ before = total;
				append_utf8_to_sv(cp, [&](SV chunk) {
					writer(chunk);
					total += chunk.size();
				});
				auto _ = before;
				if (max_sz.exceeds(total, kDefaultMaxString)) {
					return unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::string_too_large,
							.message = "decoded string exceeds max_string_size"});
				}
				break;
			}
		default:
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::syntax_error,
					.message = "invalid escape"});
		}
		if (simple) {
			++i;
			writer(SV{esc_char, 1});
			total += 1;
			if (max_sz.exceeds(total, kDefaultMaxString)) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::string_too_large,
						.message = "decoded string exceeds max_string_size"});
			}
		}
	}
	return total;
}

} // namespace detail
// ---------------------------------------------------------------------------
// JsonStringToken
// ---------------------------------------------------------------------------

export class JsonStringToken {
	SV raw_lexeme_{};
	bool has_escapes_{false};
	bool unquoted_{false};
	LimitOption max_string_size_{};

	friend class JsonReader;
	JsonStringToken(
		SV raw_lex,
		bool has_esc,
		LimitOption max_sz) noexcept
		: raw_lexeme_{raw_lex}
		, has_escapes_{has_esc}
		, max_string_size_{max_sz} {}

public:
	JsonStringToken() = default;
	[[nodiscard]] SV raw_lexeme() const noexcept { return raw_lexeme_; }
	[[nodiscard]] bool has_escapes() const noexcept { return has_escapes_; }
	[[nodiscard]] Opt<SV> unescaped_borrow() const noexcept {
		if (has_escapes_) {
			return nullopt;
		}
		if (unquoted_) {
			return raw_lexeme_;
		}
		return raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
	}
	[[nodiscard]] SZ max_decoded_size() const noexcept {
		if (unquoted_) {
			return raw_lexeme_.size();
		}
		return raw_lexeme_.size() >= 2 ? raw_lexeme_.size() - 2 : 0;
	}
	[[nodiscard]] expected<void, JsonError> append_decoded_to(
		S &out) const {
		if (unquoted_) {
			out.append(raw_lexeme_.data(), raw_lexeme_.size());
			return {};
		}
		if (raw_lexeme_.size() < 2) {
			return {};
		}
		SV body = raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
		if (!has_escapes_) {
			out.append(body.data(), body.size());
			return {};
		}
		auto res =
			detail::decode_str_body(body, [&](SV chunk) { out.append(chunk.data(), chunk.size()); }, max_string_size_);
		if (!res) {
			return unexpected(move(res).error());
		}
		return {};
	}
	[[nodiscard]] expected<SV, JsonError> decode_into(
		span<char> buf) const {
		if (unquoted_) {
			ranges::copy(raw_lexeme_, buf.data());
			return SV{buf.data(), raw_lexeme_.size()};
		}
		if (raw_lexeme_.size() < 2) {
			return SV{buf.data(), 0};
		}
		SV body = raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
		if (!has_escapes_) {
			ranges::copy(body, buf.data());
			return SV{buf.data(), body.size()};
		}
		SZ written = 0;
		auto res = detail::decode_str_body(
			body,
			[&](SV chunk) {
				ranges::copy(chunk, buf.data() + written);
				written += chunk.size();
			},
			max_string_size_);
		if (!res) {
			return unexpected(move(res).error());
		}
		return SV{buf.data(), written};
	}
};
// ---------------------------------------------------------------------------
// JsonReader
// ---------------------------------------------------------------------------

export class JsonReader {
public:
	enum class Event : u8 {
		begin_object,
		end_object,
		begin_array,
		end_array,
		key,
		string_value,
		number_value,
		bool_value,
		null_value,
	};

private:
	struct StateFrame {
		enum class Kind : u8 {
			object,
			array,
		} kind;
		bool first{true};
		bool awaiting_value{false};
	};
	SV input_;
	JsonParseOptions opts_;
	SZ pos_{0};
	SZ line_{1};
	SZ col_{1};
	V<StateFrame> stack_;
	JsonStringToken key_token_{};
	JsonStringToken str_token_{};
	JsonNumberView num_val_{SV{}, 0, 0};
	bool bool_val_{false};
	bool has_error_{false};
	JsonError last_error_{};
	SZ value_start_{0};
	void set_error(
		JsonError e) noexcept {
		has_error_ = true;
		last_error_ = move(e);
	}
	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		S msg) const {
		return {
			.stage = JsonStage::parse,
			.code = code,
			.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
			.message = move(msg)
        };
	}
	void skip_ws() {
		for (;;) {
			while (pos_ < input_.size()) {
				char const c = input_[pos_];
				if (c == '\n') {
					++pos_;
					++line_;
					col_ = 1;
				} else if (c == ' ' || c == '\t' || c == '\r') {
					++pos_;
					++col_;
				} else {
					break;
				}
			}
			if (opts_.mode != ParseMode::json5 || pos_ + 1 >= input_.size() || input_[pos_] != '/') {
				return;
			}
			if (input_[pos_ + 1] == '/') {
				pos_ += 2;
				col_ += 2;
				while (pos_ < input_.size() && input_[pos_] != '\n') {
					++pos_;
					++col_;
				}
				continue;
			}
			if (input_[pos_ + 1] == '*') {
				SZ const comment_offset = pos_;
				SZ const comment_line = line_;
				SZ const comment_col = col_;
				pos_ += 2;
				col_ += 2;
				while (pos_ + 1 < input_.size()) {
					if (input_[pos_] == '*' && input_[pos_ + 1] == '/') {
						pos_ += 2;
						col_ += 2;
						goto next_reader_ws;
					}
					if (input_[pos_] == '\n') {
						++pos_;
						++line_;
						col_ = 1;
					} else {
						++pos_;
						++col_;
					}
				}
				pos_ = input_.size();
				set_error(
					JsonError{
						.stage = JsonStage::parse,
						.code = JsonIssueCode::unexpected_eof,
						.source =
							JsonSourceLocation{.offset = comment_offset, .line = comment_line, .column = comment_col},
						.message = "unterminated block comment"
                });
				return;
			}
			return;
next_reader_ws:;
		}
	}
	[[nodiscard]] expected<void, JsonError> skip_ws_checked() {
		skip_ws();
		if (has_error_) {
			return unexpected(last_error_);
		}
		return {};
	}
	void adv(
		SZ n = 1) noexcept {
		pos_ += n;
		col_ += n;
	}
	[[nodiscard]] expected<void, JsonError> parse_str_into_token(
		LimitOption max_sz,
		JsonStringToken &tok_out) {
		SZ const raw_start = pos_ - 1;
		bool has_esc = false;
		while (pos_ < input_.size()) {
			SZ const remaining = input_.size() - pos_;
			SZ const skip = detail::simd::scan_str_until_special(input_.data() + pos_, remaining);
			pos_ += skip;
			col_ += skip;
			if (pos_ >= input_.size()) {
				break;
			}
			auto const c = static_cast<unsigned char>(input_[pos_]);
			if (c == '"') {
				adv();
				SV raw_lex = input_.substr(raw_start, pos_ - raw_start);
				SZ const body_len = raw_lex.size() - 2;
				if (max_sz.exceeds(body_len, kDefaultMaxString)) {
					return unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
				}
				tok_out = JsonStringToken{raw_lex, has_esc, max_sz};
				return {};
			}
			if (c < 0x20U) {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				has_esc = true;
				adv();
				if (pos_ >= input_.size()) {
					return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				char const esc = input_[pos_];
				if (esc == 'u') {
					adv();
					if (pos_ + 4 > input_.size()) {
						return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
					}
					// NOLINTBEGIN(readability-magic-numbers)
					auto cp_opt = detail::hex4_from_sv(input_, pos_);
					if (!cp_opt) {
						return unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
					}
					u32 cp = *cp_opt;
					adv(4);
					if (cp >= 0xD800U && cp <= 0xDBFFU) {
						if (pos_ + 6 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
						}
						adv(2);
						auto lo_opt = detail::hex4_from_sv(input_, pos_);
						if (!lo_opt) {
							return unexpected(
								mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
						}
						u32 const lo = *lo_opt;
						adv(4);
						if (lo < 0xDC00U || lo > 0xDFFFU) {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
						}
					} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
						return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
					}
					// NOLINTEND(readability-magic-numbers)
				} else {
					if (esc != '"'
						&& esc != '\\'
						&& esc != '/'
						&& esc != 'b'
						&& esc != 'f'
						&& esc != 'n'
						&& esc != 'r'
						&& esc != 't') {
						return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
					}
					adv();
				}
				continue;
			}
			SZ const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos_ + seq > input_.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (SZ k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(input_[pos_ + k]))) {
					return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			pos_ += seq;
			col_ += 1;
		}
		return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}
	[[nodiscard]] expected<void, JsonError> parse_str_sq_into_token(
		LimitOption max_sz,
		JsonStringToken &tok_out) {
		SZ const raw_start = pos_ - 1;
		bool has_esc = false;
		while (pos_ < input_.size()) {
			auto const c = static_cast<unsigned char>(input_[pos_]);
			if (c == '\'') {
				adv();
				SV raw_lex = input_.substr(raw_start, pos_ - raw_start);
				SZ const body_len = raw_lex.size() - 2;
				if (max_sz.exceeds(body_len, kDefaultMaxString)) {
					return unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
				}
				tok_out = JsonStringToken{raw_lex, has_esc, max_sz};
				return {};
			}
			if (c < 0x20U) {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				has_esc = true;
				adv();
				if (pos_ >= input_.size()) {
					return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				char const esc = input_[pos_];
				if (esc == 'u') {
					adv();
					if (pos_ + 4 > input_.size()) {
						return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
					}
					auto cp_opt = detail::hex4_from_sv(input_, pos_);
					if (!cp_opt) {
						return unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
					}
					u32 cp = *cp_opt;
					adv(4);
					// NOLINTBEGIN(readability-magic-numbers)
					if (cp >= 0xD800U && cp <= 0xDBFFU) {
						if (pos_ + 6 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
						}
						adv(2);
						auto lo_opt = detail::hex4_from_sv(input_, pos_);
						if (!lo_opt) {
							return unexpected(
								mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
						}
						u32 const lo = *lo_opt;
						adv(4);
						if (lo < 0xDC00U || lo > 0xDFFFU) {
							return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
						}
					} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
						return unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
					}
					// NOLINTEND(readability-magic-numbers)
				} else {
					if (esc != '\''
						&& esc != '"'
						&& esc != '\\'
						&& esc != '/'
						&& esc != 'b'
						&& esc != 'f'
						&& esc != 'n'
						&& esc != 'r'
						&& esc != 't') {
						return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
					}
					adv();
				}
				continue;
			}
			SZ const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos_ + seq > input_.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (SZ k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(input_[pos_ + k]))) {
					return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			pos_ += seq;
			col_ += 1;
		}
		return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}
	[[nodiscard]] expected<void, JsonError> parse_number_into_val() {
		SZ const start = pos_;
		bool const neg = input_[pos_] == '-';
		if (neg) {
			adv();
		}
		if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
			return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after sign"));
		}
		bool const starts_zero = input_[pos_] == '0';
		adv();
		if (starts_zero && pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
			return unexpected(mk_err(JsonIssueCode::syntax_error, "leading zeros forbidden"));
		}
		while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
			adv();
		}
		if (pos_ < input_.size() && input_[pos_] == '.') {
			adv();
			if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
			}
			while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
				adv();
			}
		}
		if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
			adv();
			if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
				adv();
			}
			if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "digit required in exponent"));
			}
			while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
				adv();
			}
		}
		if (pos_ - start > kMaxNumberLexemeLen) {
			return unexpected(mk_err(JsonIssueCode::invalid_number, "number lexeme exceeds maximum length"));
		}
		SV const lex = input_.substr(start, pos_ - start);
		auto node = detail::build_number_node_from_lexeme(0, static_cast<u32>(lex.size()), 0, lex);
		if (!node) {
			auto err = move(node).error();
			err.stage = JsonStage::parse;
			return unexpected(move(err));
		}
		num_val_ = JsonNumberView{lex, node->flags, node->_raw};
		return {};
	}
	[[nodiscard]] expected<Event, JsonError> parse_value_event() {
		if (opts_.max_depth.exceeds(stack_.size(), kDefaultMaxDepth)) {
			return unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}
		if (pos_ >= input_.size()) {
			return unexpected(mk_err(JsonIssueCode::unexpected_eof, "unexpected end of input"));
		}
		char const c = input_[pos_];
		if (c == '"') {
			adv();
			auto res = parse_str_into_token(opts_.max_string_size, str_token_);
			if (!res) {
				return unexpected(move(res).error());
			}
			return Event::string_value;
		}
		if (c == '\'' && opts_.mode == ParseMode::json5) {
			adv();
			auto res = parse_str_sq_into_token(opts_.max_string_size, str_token_);
			if (!res) {
				return unexpected(move(res).error());
			}
			return Event::string_value;
		}
		if (c == '{') {
			adv();
			stack_.push_back(StateFrame{.kind = StateFrame::Kind::object, .first = true, .awaiting_value = false});
			return Event::begin_object;
		}
		if (c == '[') {
			adv();
			stack_.push_back(StateFrame{.kind = StateFrame::Kind::array, .first = true, .awaiting_value = false});
			return Event::begin_array;
		}
		if (c == 't') {
			if (input_.substr(pos_, 4) != "true") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(4);
			bool_val_ = true;
			return Event::bool_value;
		}
		if (c == 'f') {
			if (input_.substr(pos_, 5) != "false") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(5);
			bool_val_ = false;
			return Event::bool_value;
		}
		if (c == 'n') {
			if (input_.substr(pos_, 4) != "null") {
				return unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			adv(4);
			return Event::null_value;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			auto res = parse_number_into_val();
			if (!res) {
				return unexpected(move(res).error());
			}
			return Event::number_value;
		}
		return unexpected(mk_err(JsonIssueCode::syntax_error, format("unexpected character '{}'", c)));
	}

public:
	explicit JsonReader(
		SV input,
		JsonParseOptions const &opts = {})
		: input_{input}
		, opts_{opts} {}
	[[nodiscard]] expected<Opt<Event>, JsonError> next() {
		if (has_error_) {
			return unexpected(last_error_);
		}
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}

		if (stack_.empty()) {
			if (pos_ >= input_.size()) {
				return Opt<Event>{};
			}
			value_start_ = pos_;
			auto ev = parse_value_event();
			if (!ev) {
				set_error(ev.error());
				return unexpected(last_error_);
			}
			return Opt<Event>{*ev};
		}

		auto &top = stack_.back();

		if (top.kind == StateFrame::Kind::array) {
			if (auto ok = skip_ws_checked(); !ok) {
				return unexpected(move(ok).error());
			}
			if (pos_ < input_.size() && input_[pos_] == ']') {
				adv();
				stack_.pop_back();
				return Opt<Event>{Event::end_array};
			}
			if (!top.first) {
				if (pos_ >= input_.size() || input_[pos_] != ',') {
					auto e = mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'");
					set_error(e);
					return unexpected(last_error_);
				}
				adv();
				if (auto ok = skip_ws_checked(); !ok) {
					return unexpected(move(ok).error());
				}
				if (opts_.mode == ParseMode::json5 && pos_ < input_.size() && input_[pos_] == ']') {
					adv();
					stack_.pop_back();
					return Opt<Event>{Event::end_array};
				}
			}
			top.first = false;
			value_start_ = pos_;
			auto ev = parse_value_event();
			if (!ev) {
				set_error(ev.error());
				return unexpected(last_error_);
			}
			return Opt<Event>{*ev};
		}

		// object
		if (top.awaiting_value) {
			top.awaiting_value = false;
			if (auto ok = skip_ws_checked(); !ok) {
				return unexpected(move(ok).error());
			}
			value_start_ = pos_;
			auto ev = parse_value_event();
			if (!ev) {
				set_error(ev.error());
				return unexpected(last_error_);
			}
			return Opt<Event>{*ev};
		}

		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == '}') {
			adv();
			stack_.pop_back();
			return Opt<Event>{Event::end_object};
		}
		if (!top.first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') {
				auto e = mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'");
				set_error(e);
				return unexpected(last_error_);
			}
			adv();
			if (auto ok = skip_ws_checked(); !ok) {
				return unexpected(move(ok).error());
			}
			if (opts_.mode == ParseMode::json5 && pos_ < input_.size() && input_[pos_] == '}') {
				adv();
				stack_.pop_back();
				return Opt<Event>{Event::end_object};
			}
		}
		top.first = false;
		if (pos_ >= input_.size()) {
			auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object");
			set_error(e);
			return unexpected(last_error_);
		}
		expected<void, JsonError> str_res = unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
		if (input_[pos_] == '"') {
			adv();
			str_res = parse_str_into_token(opts_.max_string_size, key_token_);
		} else if (input_[pos_] == '\'' && opts_.mode == ParseMode::json5) {
			adv();
			str_res = parse_str_sq_into_token(opts_.max_string_size, key_token_);
		} else if (opts_.mode == ParseMode::json5) {
			SZ const key_start = pos_;
			char const fc = input_[pos_];
			if ((fc >= 'A' && fc <= 'Z') || (fc >= 'a' && fc <= 'z') || fc == '_' || fc == '$') {
				adv();
				while (pos_ < input_.size()) {
					char const ch = input_[pos_];
					if ((ch >= 'A' && ch <= 'Z')
						|| (ch >= 'a' && ch <= 'z')
						|| (ch >= '0' && ch <= '9')
						|| ch == '_'
						|| ch == '$') {
						adv();
					} else {
						break;
					}
				}
				SV raw_lex = input_.substr(key_start, pos_ - key_start);
				key_token_ = JsonStringToken{raw_lex, false, opts_.max_string_size};
				key_token_.unquoted_ = true;
				str_res = {};
			}
		}
		if (!str_res) {
			set_error(str_res.error());
			return unexpected(last_error_);
		}
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
		if (pos_ >= input_.size() || input_[pos_] != ':') {
			auto e = mk_err(JsonIssueCode::syntax_error, "expected ':'");
			set_error(e);
			return unexpected(last_error_);
		}
		adv();
		top.awaiting_value = true;
		return Opt<Event>{Event::key};
	}
	[[nodiscard]] JsonStringToken key_token() const noexcept {
		return key_token_;
	}
	[[nodiscard]] JsonStringToken string_token() const noexcept {
		return str_token_;
	}
	[[nodiscard]] JsonNumberView number_val() const noexcept {
		return num_val_;
	}
	[[nodiscard]] bool bool_val() const noexcept {
		return bool_val_;
	}
	[[nodiscard]] SV input() const noexcept {
		return input_;
	}
	[[nodiscard]] SZ depth() const noexcept {
		return stack_.size();
	}
	[[nodiscard]] bool has_error() const noexcept {
		return has_error_;
	}
	[[nodiscard]] SZ pos() const noexcept {
		return pos_;
	}
	[[nodiscard]] SZ value_start_pos() const noexcept {
		return value_start_;
	}
	void reset() noexcept {
		pos_ = 0;
		line_ = 1;
		col_ = 1;
		stack_.clear();
		has_error_ = false;
		last_error_ = {};
	}
	// Validating skip: consumes one value through the same event parser used by
	// next(), preserving the returned byte range for callers that need the slice.
	[[nodiscard]] expected<JsonByteRange, JsonError> skip_next_value() {
		if (has_error_) {
			return unexpected(last_error_);
		}
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
		SZ const start = pos_;
		auto ev = next();
		if (!ev) {
			return unexpected(move(ev).error());
		}
		if (!*ev) {
			auto e = JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::unexpected_eof,
				.source = JsonSourceLocation{.offset = start},
				.message = "unexpected end of input"};
			set_error(e);
			return unexpected(last_error_);
		}
		int depth = 1;
		if (**ev == Event::string_value
			|| **ev == Event::number_value
			|| **ev == Event::bool_value
			|| **ev == Event::null_value) {
			return JsonByteRange{start, pos_};
		}
		while (depth > 0) {
			auto ne = next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF while skipping");
				set_error(e);
				return unexpected(last_error_);
			}
			if (**ne == Event::begin_object || **ne == Event::begin_array) {
				++depth;
			} else if (**ne == Event::end_object || **ne == Event::end_array) {
				--depth;
			}
		}
		return JsonByteRange{start, pos_};
	}
};
// ---------------------------------------------------------------------------
// NodeRef
// ---------------------------------------------------------------------------

export class NodeRef {
	DocumentStorage const *storage_{};
	SZ idx_{};

	friend class Document;
	friend class ArenaDocument;
	friend class ObjectView;
	friend class ArrayView;
	friend class ObjectMemberRange;
	friend class ArrayElementRange;
	bool friend is_same_node(NodeRef, NodeRef) noexcept;
	bool friend is_value_equal(NodeRef, NodeRef);
	bool friend is_value_equal_exact(NodeRef, NodeRef);
	friend struct NodeIdentityHash;
	NodeRef(
		DocumentStorage const *s,
		SZ i) noexcept
		: storage_{s}
		, idx_{i} {}
	[[nodiscard]] Node const &rec() const noexcept {
		assert(storage_ && "NodeRef used in default-constructed state");
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
		case NodeKind::array_ : return JsonKind::array;
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
	[[nodiscard]] expected<SV, JsonError> as_string() const {
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
	[[nodiscard]] expected<i64, JsonError> as_i64() const {
		return as_number().and_then([](JsonNumberView n) { return n.to_i64(); });
	}
	[[nodiscard]] expected<u64, JsonError> as_u64() const {
		return as_number().and_then([](JsonNumberView n) { return n.to_u64(); });
	}
	[[nodiscard]] expected<double, JsonError> as_double() const {
		return as_number().and_then([](JsonNumberView n) { return n.to_f64(); });
	}
	[[nodiscard]] expected<NodeRef, JsonError> at(JsonPath const &path) const;
};
// ---------------------------------------------------------------------------
// ObjectMember (after NodeRef — NodeRef used by value)
// ---------------------------------------------------------------------------

export struct ObjectMember {
	SV name;
	NodeRef value;
};
// ---------------------------------------------------------------------------
// Phase 6 helpers — hash table build + lookup
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline u32 hash_name(
	SV name,
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
	u32 const target = count <= kEffectiveMax / 2U ? count * 2U : kEffectiveMax;
	u32 cap = 1;
	while (cap < target && cap < kEffectiveMax) {
		cap <<= 1;
	}
	if (cap < count) {
		return 0; // Object too large to index — fall back to linear scan.
	}
	return cap;
}
// Linear scan: returns val_node index or std::nullopt.
[[nodiscard]] inline Opt<SZ> lookup_linear(
	DocumentStorage const *storage,
	SZ mem_start,
	SZ mem_count,
	SV name) noexcept {
	for (SZ i = 0; i < mem_count; ++i) {
		auto const &m = storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		if (storage->member_name(m) == name) {
			return m.val_node;
		}
	}
	return nullopt;
}
// Probe hash table; fall back to linear if probe chain exceeds kProbeChainMax.
[[nodiscard]] inline Opt<SZ> lookup_in(
	ObjHashTable const &ht,
	DocumentStorage const *storage,
	SZ mem_start,
	SZ mem_count,
	SV name) noexcept {
	auto const h = hash_name(name, storage->hash_seed_);
	u32 const mask = ht.capacity - 1;
	u32 slot = h & mask;
	auto const *slots = ht.slots_data();
	auto const *const *ptr_cache = ht.ptr_cache_data();
	for (u32 probe = 0; probe < kProbeChainMax; ++probe) {
		auto const &s = slots[slot]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (s.member_index == kEmptySlot) {
			return nullopt;
		}
		if (s.name_hash == h) {
			auto const &m = storage->object_members
								[mem_start + s.member_index]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			// Item C: ptr_cache holds pre-resolved data pointer; no dispatch per probe
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			if (SV{ptr_cache[s.member_index], m.name_len} == name) {
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
	SZ mem_start,
	SZ mem_count) noexcept {
	u32 const mask = ht.capacity - 1;
	auto *slots = ht.slots_data();
	auto **ptr_cache = ht.ptr_cache_data();
	for (u32 i = 0; i < static_cast<u32>(mem_count); ++i) {
		auto const &m = storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		auto const sv = storage->member_name(m);
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		ptr_cache[i] = sv.data(); // Item C: cache pointer in ptr_cache (arena stable post-parse)
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
	SZ mem_start_{};
	SZ mem_count_{};
	SZ node_idx_{};

	friend class NodeRef;
	friend class Document;
	bool friend is_value_equal(NodeRef, NodeRef);
	bool friend is_value_equal_exact(NodeRef, NodeRef);
	ObjectView(
		DocumentStorage const *s,
		SZ start,
		SZ count,
		SZ node_idx) noexcept
		: storage_{s}
		, mem_start_{start}
		, mem_count_{count}
		, node_idx_{node_idx} {}

public:
	[[nodiscard]] SZ size() const noexcept { return mem_count_; }
	[[nodiscard]] Opt<NodeRef> find_member(
		SV name) const noexcept {
		auto to_ref = [&](Opt<SZ> idx) -> Opt<NodeRef> {
			if (!idx) {
				return nullopt;
			}
			return NodeRef{storage_, *idx};
		};
		if (mem_count_ < kHashThreshold) {
			return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
		}
		// Lazy hash table build via Atom CAS. The hash slot is the only
		// mutable surface on a published Document — see post-publication
		// freeze contract; const_cast is justified by that invariant.
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		auto &raw = const_cast<ObjHashTable *&>(storage_->nodes[node_idx_].hash_idx_raw);
		auto ref = std::atomic_ref<ObjHashTable *>{raw};
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
				owned = ObjHashTable::create(cap, static_cast<u32>(mem_count_), storage_->hash_mr_);
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
				auto _ = ref.compare_exchange_strong(
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
		SV name) const {
		auto found = find_member(name);
		if (!found) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::missing_member,
					.member_name = S{name},
					.message = format("missing member: {}", name)});
		}
		return *found;
	}
	[[nodiscard]] ObjectMemberRange members() const noexcept;
};
export class ArrayView {
	DocumentStorage const *storage_{};
	SZ child_start_{};
	SZ child_count_{};

	friend class NodeRef;
	bool friend is_value_equal(NodeRef, NodeRef);
	bool friend is_value_equal_exact(NodeRef, NodeRef);
	ArrayView(
		DocumentStorage const *s,
		SZ start,
		SZ count) noexcept
		: storage_{s}
		, child_start_{start}
		, child_count_{count} {}

public:
	[[nodiscard]] SZ size() const noexcept { return child_count_; }
	[[nodiscard]] expected<NodeRef, JsonError> element(
		SZ index) const {
		if (index >= child_count_) {
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::index_out_of_range,
					.requested_index = index,
					.container_size = child_count_,
					.message = format("index {} out of range (size={})", index, child_count_)});
		}
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
	if (rec().kind != NodeKind::array_) {
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
	for (SZ i = 0; i < path.size(); ++i) {
		auto const &seg = path.segment(i);
		auto set_path = [&](JsonError err) {
			err.path = JsonPath{};
			for (SZ j = 0; j <= i; ++j) {
				push_seg(err.path, path.segment(j));
			}
			return unexpected(move(err));
		};
		if (holds_alternative<JsonPathMember>(seg)) {
			auto const &name = get<JsonPathMember>(seg).name;
			if (cur.kind() == JsonKind::array) {
				bool all_digits = !name.empty() && (name.size() == 1 || name[0] != '0');
				for (SZ k = 0; all_digits && k < name.size(); ++k) {
					all_digits = name[k] >= '0' && name[k] <= '9';
				}
				if (!all_digits) {
					return set_path(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::wrong_kind,
							.expected_kind = JsonKind::object,
							.actual_kind = JsonKind::array,
							.message = "non-numeric JSON Pointer segment on array"});
				}
				SZ idx = 0;
				for (char const ch: name) {
					idx = idx * 10 + static_cast<SZ>(ch - '0');
				}
				auto arr = cur.as_array();
				if (!arr) {
					return set_path(move(arr).error());
				}
				auto child = arr->element(idx);
				if (!child) {
					return set_path(move(child).error());
				}
				cur = *child;
			} else {
				auto obj = cur.as_object();
				if (!obj) {
					return set_path(move(obj).error());
				}
				auto child = obj->member(name);
				if (!child) {
					return set_path(move(child).error());
				}
				cur = *child;
			}
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
	SZ start_{};
	SZ count_{};

	friend class ObjectView;
	ObjectMemberRange(
		DocumentStorage const *s,
		SZ start,
		SZ count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		SZ start_{};
		SZ idx_{};
		SZ count_{};
		friend class ObjectMemberRange;
		Iterator(
			DocumentStorage const *s,
			SZ st,
			SZ cnt,
			SZ i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = std::ptrdiff_t;
		using value_type = ObjectMember;
		using iterator_category = std::forward_iterator_tag;
		Iterator() = default;
		[[nodiscard]] ObjectMember operator *() const {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
	SZ start_{};
	SZ count_{};

	friend class ArrayView;
	ArrayElementRange(
		DocumentStorage const *s,
		SZ start,
		SZ count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		SZ start_{};
		SZ idx_{};
		SZ count_{};
		friend class ArrayElementRange;
		Iterator(
			DocumentStorage const *s,
			SZ st,
			SZ cnt,
			SZ i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = std::ptrdiff_t;
		using value_type = NodeRef;
		using iterator_category = std::forward_iterator_tag;
		Iterator() = default;
		[[nodiscard]] NodeRef operator *() const {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
	case NodeKind::array_:
		{
			ArrayView const av{a.storage_, a.rec().off, a.rec().len};
			ArrayView const bv{b.storage_, b.rec().off, b.rec().len};
			if (av.size() != bv.size()) {
				return false;
			}
			for (SZ i = 0; i < av.size(); ++i) {
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
	case NodeKind::array_:
		{
			ArrayView const av{a.storage_, a.rec().off, a.rec().len};
			ArrayView const bv{b.storage_, b.rec().off, b.rec().len};
			if (av.size() != bv.size()) {
				return false;
			}
			for (SZ i = 0; i < av.size(); ++i) {
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
	SZ operator ()(
		NodeRef n) const noexcept {
		return hash<void const *>{}(n.storage_) ^ (hash<SZ>{}(n.idx_) << 1U);
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
	char indent_char{' '};
	Opt<SZ> truncate_depth{};
};
export struct WarmIndexOptions {
	SZ max_objects{SIZE_MAX};
	SZ max_extra_bytes{SIZE_MAX};
};
export class Document {
	UP<DocumentStorage> storage_;

	friend class ValueBuilder;
	Document friend make_document(UP<DocumentStorage>) noexcept;
	explicit Document(
		UP<DocumentStorage> s) noexcept
		: storage_{move(s)} {}

public:
	Document() = default;
	Document(Document &&) noexcept = default;
	Document &operator =(Document &&) noexcept = default;
	Document(Document const &) = delete;
	Document &operator =(Document const &) = delete;
	[[nodiscard]] NodeRef root() const noexcept {
		assert(storage_ && "Document::root() called on empty Document");
		return NodeRef{storage_.get(), storage_->root_node};
	}
	[[nodiscard]] expected<S, JsonError> dump(JsonDumpOptions const &opts = {}) const;
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
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		auto &slot = storage_->nodes[ov.node_idx_].hash_idx_raw;
		auto ref = std::atomic_ref<ObjHashTable *>{slot};
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
			auto _ = ref.compare_exchange_strong(
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
			owned = ObjHashTable::create(cap, static_cast<u32>(ov.mem_count_), storage_->hash_mr_);
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
		} catch (std::bad_alloc const &) {
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
		SZ objects_warmed = 0;
		SZ bytes_allocated = 0;
		for (SZ i = 0; i < storage_->nodes.size(); ++i) {
			auto &n = storage_->nodes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			if (n.kind != NodeKind::object) {
				continue;
			}
			auto const mem_count = n.len;
			if (mem_count < kHashThreshold) {
				continue;
			}
			if (std::atomic_ref<ObjHashTable *>{n.hash_idx_raw}.load(memory_order_acquire) != nullptr) {
				continue; // already indexed or failed
			}
			u32 const cap = detail::clamped_capacity(static_cast<u32>(mem_count));
			SZ const est_bytes = cap > 0 ? sizeof(ObjHashTable) + static_cast<SZ>(cap) * sizeof(ObjHashSlot) : 0;
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
	UP<DocumentStorage> s) noexcept {
	return Document{move(s)};
}
// ─── Phase 5.2 — JsonArena / ArenaDocument ──────────────────────────────────

export struct JsonArenaOptions {
	SZ initial_slab{64 * 1024};
};
export class ArenaDocument {
	DocumentStorage const *storage_{};
	u32 generation_{};
	u32 const *arena_gen_{};

	friend class JsonArena;
	ArenaDocument(
		DocumentStorage const *s,
		u32 gen,
		u32 const *ag) noexcept
		: storage_{s}
		, generation_{gen}
		, arena_gen_{ag} {}
	void check_live() const noexcept { assert(storage_ != nullptr && *arena_gen_ == generation_); }

public:
	ArenaDocument() = default;
	[[nodiscard]] NodeRef root() const noexcept {
		check_live();
		return NodeRef{storage_, storage_->root_node};
	}
	[[nodiscard]] expected<S, JsonError> dump(JsonDumpOptions const &opts = {}) const;
};
export class JsonArena {
	SZ initial_slab_;
	std::pmr::monotonic_buffer_resource mbr_;
	UP<DocumentStorage> storage_;
	u32 generation_{0};

public:
	explicit JsonArena(
		JsonArenaOptions const &opts = {})
		: initial_slab_{opts.initial_slab}
		, mbr_{opts.initial_slab}
		, storage_{make_unique<DocumentStorage>(&mbr_)} {}
	JsonArena(JsonArena const &) = delete;
	JsonArena &operator =(JsonArena const &) = delete;
	JsonArena(JsonArena &&) = delete;
	JsonArena &operator =(JsonArena &&) = delete;

	[[nodiscard]] expected<ArenaDocument, JsonError> parse_into(SV input, JsonParseOptions const &opts = {});

	[[nodiscard]] expected<ArenaDocument, JsonError> parse_borrowed_into(SV input, JsonParseOptions const &opts = {});

	[[nodiscard]] expected<ArenaDocument, JsonError> parse_moved_into(S input, JsonParseOptions const &opts = {});

	void reset() noexcept;
	[[nodiscard]] SZ slab_capacity() const noexcept { return initial_slab_; }
	[[nodiscard]] SZ slab_used() const noexcept { return 0; }
};
// ---------------------------------------------------------------------------
// Field accessor helpers (Phase 1.1)
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline JsonError missing_err(
	SV name) {
	return JsonError{
		.stage = JsonStage::lookup,
		.code = JsonIssueCode::missing_member,
		.path =
			[&] {
				JsonPath p;
				p.push_member(name);
				return p;
			}(),
		.member_name = S{name},
		.message = format("missing member: {}", name)};
}
[[nodiscard]] inline JsonError type_err(
	SV name,
	JsonKind expected,
	JsonKind actual) {
	return JsonError{
		.stage = JsonStage::lookup,
		.code = JsonIssueCode::wrong_kind,
		.path =
			[&] {
				JsonPath p;
				p.push_member(name);
				return p;
			}(),
		.expected_kind = expected,
		.actual_kind = actual,
		.member_name = S{name},
		.message = format("member '{}' has wrong type", name)};
}

} // namespace detail
export [[nodiscard]] expected<S, JsonError> require_string(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node) {
		return unexpected(detail::missing_err(name));
	}
	auto sv = node->as_string();
	if (!sv) {
		return unexpected(detail::type_err(name, JsonKind::string, node->kind()));
	}
	return S{*sv};
}
export [[nodiscard]] expected<i64, JsonError> require_int(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node) {
		return unexpected(detail::missing_err(name));
	}
	auto v = node->as_i64();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return *v;
}
export [[nodiscard]] expected<u64, JsonError> require_uint(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node) {
		return unexpected(detail::missing_err(name));
	}
	auto v = node->as_u64();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return *v;
}
export [[nodiscard]] expected<double, JsonError> require_double(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node) {
		return unexpected(detail::missing_err(name));
	}
	auto v = node->as_double();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return *v;
}
export [[nodiscard]] expected<bool, JsonError> require_bool(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node) {
		return unexpected(detail::missing_err(name));
	}
	auto v = node->as_bool();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return *v;
}
export [[nodiscard]] expected<Opt<S>, JsonError> optional_string(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return Opt<S>{};
	}
	auto sv = node->as_string();
	if (!sv) {
		return unexpected(detail::type_err(name, JsonKind::string, node->kind()));
	}
	return Opt<S>{S{*sv}};
}
export [[nodiscard]] expected<Opt<i64>, JsonError> optional_int(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return Opt<i64>{};
	}
	auto v = node->as_i64();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return Opt<i64>{*v};
}
export [[nodiscard]] expected<Opt<u64>, JsonError> optional_uint(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return Opt<u64>{};
	}
	auto v = node->as_u64();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return Opt<u64>{*v};
}
export [[nodiscard]] expected<Opt<double>, JsonError> optional_double(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return Opt<double>{};
	}
	auto v = node->as_double();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return Opt<double>{*v};
}
export [[nodiscard]] expected<Opt<bool>, JsonError> optional_bool(
	ObjectView const &obj,
	SV name) {
	auto node = obj.find_member(name);
	if (!node || node->is_null()) {
		return Opt<bool>{};
	}
	auto v = node->as_bool();
	if (!v) {
		auto e = move(v).error();
		e.path.push_member(name);
		e.member_name = S{name};
		return unexpected(move(e));
	}
	return Opt<bool>{*v};
}
// ---------------------------------------------------------------------------
// Dump implementation
// ---------------------------------------------------------------------------

// NOLINTBEGIN(readability-magic-numbers)
// Fast-path dump for bytes already known to be a raw JSON S body
// (kRawJsonSlice set on parse-side unescaped strings/numbers): no scan,
// just bracket the slice with quotes. Caller must guarantee `flags &
// kRawJsonSlice` and !ascii_only (the latter would still need a
// byte-by-byte non-ASCII rewrite).
inline void dump_str_raw(
	SV sv,
	S &out) {
	out += '"';
	out.append(sv.data(), sv.size());
	out += '"';
}
// R3 — find the next byte in [p, n) that needs escaping in a JSON string body.
// ascii_only=false: stops at '"', '\\', or ctrl chars [0x00,0x1F].
// ascii_only=true:  also stops at high-bit bytes [0x80,0xFF].
[[nodiscard]] inline SZ scan_dump_safe_run(
	char const *p,
	SZ n,
	bool ascii_only) noexcept {
	SZ i = 0;
#if defined(CONFLUX_JSON_HAS_STDSIMD)
	return conflux_json_scan_dump_safe_run_stdsimd(p, n, ascii_only ? 1 : 0);
#elif defined(CONFLUX_JSON_HAS_AVX2)
	__m256i const v_quote = _mm256_set1_epi8('"');
	__m256i const v_back = _mm256_set1_epi8('\\');
	__m256i const v_lim = _mm256_set1_epi8(0x20);
	__m256i const v_1f = _mm256_set1_epi8(0x1F);
	while (i + 32 <= n) {
		__m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const *>(p + i));
		__m256i const eq_q = _mm256_cmpeq_epi8(v, v_quote);
		__m256i const eq_b = _mm256_cmpeq_epi8(v, v_back);
		__m256i mix = _mm256_or_si256(eq_q, eq_b);
		if (ascii_only) {
			mix = _mm256_or_si256(mix, _mm256_cmpgt_epi8(v_lim, v));
		} else {
			mix = _mm256_or_si256(mix, _mm256_cmpeq_epi8(_mm256_min_epu8(v, v_1f), v));
		}
		auto const mask = static_cast<unsigned>(_mm256_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask));
		}
		i += 32;
	}
	if (i + 16 <= n) {
		__m128i const v128 = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_quote));
		__m128i const eq_b = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_back));
		__m128i mix16 = _mm_or_si128(eq_q, eq_b);
		if (ascii_only) {
			mix16 = _mm_or_si128(mix16, _mm_cmplt_epi8(v128, _mm256_castsi256_si128(v_lim)));
		} else {
			mix16 = _mm_or_si128(mix16, _mm_cmpeq_epi8(_mm_min_epu8(v128, _mm256_castsi256_si128(v_1f)), v128));
		}
		auto const mask16 = static_cast<unsigned>(_mm_movemask_epi8(mix16));
		if (mask16 != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask16));
		}
		i += 16;
	}
#elif defined(CONFLUX_JSON_HAS_SSE2)
	__m128i const v_quote = _mm_set1_epi8('"');
	__m128i const v_back = _mm_set1_epi8('\\');
	__m128i const v_lim = _mm_set1_epi8(0x20);
	__m128i const v_1f = _mm_set1_epi8(0x1F);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back);
		__m128i mix = _mm_or_si128(eq_q, eq_b);
		if (ascii_only) {
			mix = _mm_or_si128(mix, _mm_cmplt_epi8(v, v_lim));
		} else {
			mix = _mm_or_si128(mix, _mm_cmpeq_epi8(_mm_min_epu8(v, v_1f), v));
		}
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask));
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
	SV sv,
	S &out,
	bool ascii_only) {
	out += '"';
	SZ i = 0;
	while (i < sv.size()) {
		auto const c = static_cast<unsigned char>(sv[i]);
		// Scalar pre-check: when the very next byte already needs escaping,
		// skip the SIMD chunk setup entirely. Avoids paying SIMD cost on
		// escape-dense payloads where every other byte is an escape.
		bool const needs_escape = (c == '"' || c == '\\' || c < 0x20U || (ascii_only && c >= 0x80U));
		if (!needs_escape) {
			// R3 — fast-forward over the safe-ASCII run.
			SZ const run = scan_dump_safe_run(sv.data() + i, sv.size() - i, ascii_only);
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
				// Decode UTF-8 to get code point, then emit \uXXXX or surrogate P.
				u32 cp = 0;
				SZ seq = 0;
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
				for (SZ k = 1; k < seq && i + k < sv.size(); ++k) {
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
	SZ node_idx,
	JsonDumpOptions const &opts,
	unsigned depth,
	S &out) {
	if (opts.truncate_depth.has_value() && static_cast<SZ>(depth) > *opts.truncate_depth) {
		out += "null";
		return;
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto const &n = store.nodes[node_idx];
	auto indent = [&](unsigned d) {
		if (!opts.pretty) {
			return;
		}
		out += '\n';
		out.append(static_cast<SZ>(d) * opts.indent, opts.indent_char);
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
	case NodeKind::array_:
		{
			out += '[';
			if (n.len > 0) {
				for (SZ i = 0; i < n.len; ++i) {
					if (i > 0) {
						out += ',';
					}
					indent(depth + 1);
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
				// R3 — only allocate the order V when sorting; the
				// unsorted path iterates members in source order directly.
				if (opts.sort_object_keys) {
					V<SZ> order(n.len);
					iota(order.begin(), order.end(), 0);
					sort(order.begin(), order.end(), [&](SZ x, SZ y) {
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
						auto const &mx = store.object_members[n.off + x];
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
						auto const &my = store.object_members[n.off + y];
						return store.member_name(mx) < store.member_name(my);
					});
					for (SZ i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
					for (SZ i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
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
expected<S, JsonError> Document::dump(
	JsonDumpOptions const &opts) const {
	S out;
	// R3 — skip the small-buffer doubling cycle. Empirically dump output
	// is roughly 1.05–1.2x the input size for compact corpora and within
	// 3x for pretty-printed; reserve from string_arena + nodes count.
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
expected<S, JsonError> ArenaDocument::dump(
	JsonDumpOptions const &opts) const {
	check_live();
	S out;
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

SZ utf8_seq_len(
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
// Phase 8 — SIMD scan for the Tokenizer hot path via std::experimental::simd.
// ---------------------------------------------------------------------------
namespace detail::simd {

[[nodiscard]] inline SZ scan_str_until_special(
	char const *p,
	SZ n) noexcept {
	SZ i = 0;
#if defined(CONFLUX_JSON_HAS_STDSIMD)
	return conflux_json_scan_str_until_special_stdsimd(p, n);
#elif defined(CONFLUX_JSON_HAS_AVX2)
	__m256i const v_quote = _mm256_set1_epi8('"');
	__m256i const v_back = _mm256_set1_epi8('\\');
	__m256i const v_lim = _mm256_set1_epi8(0x20);
	while (i + 32 <= n) {
		__m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const *>(p + i));
		__m256i const eq_q = _mm256_cmpeq_epi8(v, v_quote);
		__m256i const eq_b = _mm256_cmpeq_epi8(v, v_back);
		__m256i const lt_l = _mm256_cmpgt_epi8(v_lim, v);
		__m256i const mix = _mm256_or_si256(_mm256_or_si256(eq_q, eq_b), lt_l);
		auto const mask = static_cast<unsigned>(_mm256_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask));
		}
		i += 32;
	}
	if (i + 16 <= n) {
		__m128i const v128 = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_quote));
		__m128i const eq_b = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_back));
		__m128i const lt_l = _mm_cmplt_epi8(v128, _mm256_castsi256_si128(v_lim));
		__m128i const mix16 = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_l);
		auto const mask16 = static_cast<unsigned>(_mm_movemask_epi8(mix16));
		if (mask16 != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask16));
		}
		i += 16;
	}
#elif defined(CONFLUX_JSON_HAS_SSE2)
	__m128i const v_quote = _mm_set1_epi8('"');
	__m128i const v_back = _mm_set1_epi8('\\');
	__m128i const v_lim = _mm_set1_epi8(0x20);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back);
		__m128i const lt_lim = _mm_cmplt_epi8(v, v_lim);
		__m128i const mix = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_lim);
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<SZ>(__builtin_ctz(mask));
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
// Phase 3 — Tokenizer owns input bytes / source coordinates and emits S
// + number lexemes; TreeBuilder consumes those + structural punctuation and
// builds Nodes. Splitting them keeps the byte-level scan layer reusable
// (SIMD prerequisite) without changing semantics.
struct Tokenizer {
	SV src;
	SZ pos{};
	SZ line{1};
	SZ col{1};
	DocumentStorage &store;
	u32 bom_prefix_bytes;
	ParseMode mode{};
	bool unterminated_block_comment{};
	SZ unterminated_block_comment_pos{};
	SZ unterminated_block_comment_line{1};
	SZ unterminated_block_comment_col{1};
	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		S msg) const {
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
	[[nodiscard]] JsonError whitespace_error() const {
		return {
			.stage = JsonStage::parse,
			.code = JsonIssueCode::unexpected_eof,
			.source =
				JsonSourceLocation{
								   .offset = unterminated_block_comment_pos + bom_prefix_bytes,
								   .line = unterminated_block_comment_line,
								   .column = unterminated_block_comment_col},
			.message = "unterminated block comment"
        };
	}
	void skip_ws() {
		if (unterminated_block_comment) {
			return;
		}
		for (;;) {
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
			if (mode != ParseMode::json5 || pos + 1 >= src.size() || src[pos] != '/') {
				return;
			}
			if (src[pos + 1] == '/') {
				pos += 2;
				col += 2;
				while (pos < src.size() && src[pos] != '\n') {
					++pos;
					++col;
				}
				continue;
			}
			if (src[pos + 1] == '*') {
				SZ const comment_pos = pos;
				SZ const comment_line = line;
				SZ const comment_col = col;
				pos += 2;
				col += 2;
				while (pos + 1 < src.size()) {
					if (src[pos] == '*' && src[pos + 1] == '/') {
						pos += 2;
						col += 2;
						goto next_ws;
					}
					if (src[pos] == '\n') {
						++pos;
						++line;
						col = 1;
					} else {
						++pos;
						++col;
					}
				}
				pos = src.size();
				unterminated_block_comment = true;
				unterminated_block_comment_pos = comment_pos;
				unterminated_block_comment_line = comment_line;
				unterminated_block_comment_col = comment_col;
				return;
			}
			return;
next_ws:;
		}
	}
	void adv(
		SZ n = 1) noexcept {
		pos += n;
		col += n;
	}
	template<class Str>
	static void append_utf8(
		u32 cp,
		Str &out) {
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
		for (SZ i = 0; i < 4; ++i) {
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
			SZ const remaining = src.size() - pos;
			SZ const skip = detail::simd::scan_str_until_special(src.data() + pos, remaining);
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
				SZ const arena_off = store.string_arena.size();
				store.string_arena.append(src.data() + start_pos, pos - start_pos);
				return parse_str_decode_tail(arena_off);
			}
			SZ const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos + seq > src.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (SZ k = 1; k < seq; ++k) {
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
		SZ arena_off) {
		constexpr unsigned char kCtrlEnd = 0x20U;
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				adv();
				SZ const len = store.string_arena.size() - arena_off;
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
			SZ const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos + seq > src.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (SZ k = 1; k < seq; ++k) {
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
	// JSON5: single-quoted string. Scalar scan (no SIMD). Allows \' escape.
	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] expected<ParsedStr, JsonError> parse_str_body_sq() {
		constexpr unsigned char kCtrlEnd = 0x20U;
		SZ const arena_off = store.string_arena.size();
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '\'') {
				adv();
				SZ const len = store.string_arena.size() - arena_off;
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
				case '\'':
					store.string_arena += '\'';
					adv();
					break;
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
			SZ const seq = utf8_seq_len(c);
			if (seq == 0) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 byte"));
			}
			if (pos + seq > src.size()) {
				return unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (SZ k = 1; k < seq; ++k) {
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
	// JSON5: unquoted key — [A-Za-z_$][A-Za-z0-9_$]*
	[[nodiscard]] expected<ParsedStr, JsonError> parse_unquoted_key() {
		SZ const start = pos;
		char const first = src[pos];
		if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_' || first == '$')) {
			return unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key or identifier"));
		}
		adv();
		while (pos < src.size()) {
			char const ch = src[pos];
			if ((ch >= 'A' && ch <= 'Z')
				|| (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| ch == '_'
				|| ch == '$') {
				adv();
			} else {
				break;
			}
		}
		auto const len = static_cast<u32>(pos - start);
		return ParsedStr{static_cast<u32>(start), len, static_cast<u8>(kStorageInputView | kRawJsonSlice)};
	}
	// Scans a number lexeme per RFC 8259 grammar and returns the slice of `src`
	// covering it. Caller (TreeBuilder) classifies the value and stores the
	// node; the lexeme references input_view directly (Phase 1: zero-copy).
	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] expected<SV, JsonError> parse_number_lexeme() {
		SZ const start = pos;
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

	// Phase 4: shared staging buffers across nested A/object frames. Each
	// frame's slice is [frame.children_start .. staging.size()) for arrays and
	// [frame.members_start .. staging_members.size()) for objects; on close
	// the slice is moved to store.array_children / store.object_members and
	// the staging buffer truncated back. This eliminates per-frame heap
	// allocation that the v7-style local vectors paid for each container.
	V<u32> staging;
	V<MemberEntry> staging_members;
	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		S msg) const {
		return tok.mk_err(code, move(msg));
	}
	[[nodiscard]] expected<void, JsonError> skip_ws_checked() {
		tok.skip_ws();
		if (tok.unterminated_block_comment) {
			return unexpected(tok.whitespace_error());
		}
		return {};
	}
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<SZ, JsonError> parse_value(
		SZ depth) {
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
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
		if (c == '\'' && opts.mode == ParseMode::json5) {
			tok.adv();
			return parse_str_node_sq();
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
	[[nodiscard]] expected<SZ, JsonError> parse_str_node() {
		auto parsed = tok.parse_str_body();
		if (!parsed) {
			return unexpected(move(parsed).error());
		}
		if (opts.max_string_size.exceeds(parsed->len, kDefaultMaxString)) {
			return unexpected(mk_err(JsonIssueCode::string_too_large, "S exceeds max_string_size"));
		}
		store.nodes.push_back(detail::make_string(parsed->off, parsed->len, parsed->flags));
		return store.nodes.size() - 1;
	}
	[[nodiscard]] expected<SZ, JsonError> parse_str_node_sq() {
		auto parsed = tok.parse_str_body_sq();
		if (!parsed) {
			return unexpected(move(parsed).error());
		}
		if (opts.max_string_size.exceeds(parsed->len, kDefaultMaxString)) {
			return unexpected(mk_err(JsonIssueCode::string_too_large, "S exceeds max_string_size"));
		}
		store.nodes.push_back(detail::make_string(parsed->off, parsed->len, parsed->flags));
		return store.nodes.size() - 1;
	}
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] expected<SZ, JsonError> parse_array(
		SZ depth) {
		tok.adv(); // '['
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
			tok.adv();
			SZ const cs = store.array_children.size();
			store.nodes.push_back(detail::node_array(static_cast<u32>(cs), static_cast<u32>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: append child indices to shared staging[children_start..],
		// flush to array_children at close, then truncate staging.
		SZ const children_start = staging.size();
		while (true) {
			auto child = parse_value(depth + 1);
			if (!child) {
				return unexpected(move(child).error());
			}
			staging.push_back(static_cast<u32>(*child));
			if (auto ok = skip_ws_checked(); !ok) {
				return unexpected(move(ok).error());
			}
			if (tok.pos >= tok.src.size()) {
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in array"));
			}
			if (tok.src[tok.pos] == ']') {
				tok.adv();
				SZ const len = staging.size() - children_start;
				SZ const cs = store.array_children.size();
				store.array_children.insert(
					store.array_children.end(),
					staging.begin() + static_cast<std::ptrdiff_t>(children_start),
					staging.end());
				staging.resize(children_start);
				store.nodes.push_back(detail::node_array(static_cast<u32>(cs), static_cast<u32>(len)));
				return store.nodes.size() - 1;
			}
			if (tok.src[tok.pos] != ',') {
				staging.resize(children_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'"));
			}
			tok.adv();
			if (opts.mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					return unexpected(move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
					tok.adv();
					SZ const len = staging.size() - children_start;
					SZ const cs = store.array_children.size();
					store.array_children.insert(
						store.array_children.end(),
						staging.begin() + static_cast<std::ptrdiff_t>(children_start),
						staging.end());
					staging.resize(children_start);
					store.nodes.push_back(detail::node_array(static_cast<u32>(cs), static_cast<u32>(len)));
					return store.nodes.size() - 1;
				}
			}
		}
	}
	// Phase 5: linear dedup for n <= 8 (no allocation), lazy US
	// promotion above the threshold. The set is constructed only when the
	// object actually exceeds the linear-scan window — typical configs
	// (small flat objects) pay zero hash-table cost.
	static constexpr SZ kDedupLinearMax = 8;
	[[nodiscard]] bool dedup_member_present(
		SZ members_start,
		SV name,
		Opt<US<SV>> const &seen_hash) const {
		if (seen_hash.has_value()) {
			return seen_hash->contains(name);
		}
		for (SZ i = members_start; i < staging_members.size(); ++i) {
			auto const &m = staging_members[i];
			if (store.bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags)) == name) {
				return true;
			}
		}
		return false;
	}
	// Destroy hash tables in store.nodes[from..store.nodes.size()) before resize.
	void destroy_nodes_range(
		SZ from) noexcept {
		for (SZ i = from; i < store.nodes.size(); ++i) {
			auto const &n = store.nodes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
				ObjHashTable::destroy(n.hash_idx_raw);
			}
		}
	}
	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] expected<SZ, JsonError> parse_object(
		SZ depth) {
		struct StorageMark {
			SZ nodes;
			SZ string_arena;
			SZ array_children;
			SZ object_members;
		};
		tok.adv(); // '{'
		if (auto ok = skip_ws_checked(); !ok) {
			return unexpected(move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
			tok.adv();
			SZ const ms = store.object_members.size();
			store.nodes.push_back(detail::node_object(static_cast<u32>(ms), static_cast<u32>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: members go to shared staging_members[members_start..],
		// flushed to object_members at close.
		SZ const members_start = staging_members.size();
		// Phase 5: dedup is linear until size > kDedupLinearMax, then a
		// hash set is built once and reused for the remainder of this object.
		Opt<US<SV>> seen_hash;
		auto const dup_policy = opts.duplicate_key;
		while (true) {
			if (auto ok = skip_ws_checked(); !ok) {
				staging_members.resize(members_start);
				return unexpected(move(ok).error());
			}
			if (tok.pos >= tok.src.size()) {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			expected<Tokenizer::ParsedStr, JsonError> parsed_name =
				unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
			char const key_ch = tok.src[tok.pos];
			if (key_ch == '"') {
				tok.adv();
				parsed_name = tok.parse_str_body();
			} else if (key_ch == '\'' && opts.mode == ParseMode::json5) {
				tok.adv();
				parsed_name = tok.parse_str_body_sq();
			} else if (opts.mode == ParseMode::json5) {
				parsed_name = tok.parse_unquoted_key();
			}
			if (!parsed_name) {
				staging_members.resize(members_start);
				return unexpected(move(parsed_name).error());
			}
			SV const name_sv = store.bytes_at(parsed_name->off, parsed_name->len, parsed_name->flags);
			bool const is_dup = dedup_member_present(members_start, name_sv, seen_hash);
			if (is_dup && dup_policy == DuplicateKeyPolicy::reject) {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::duplicate_member, format("duplicate member: {}", name_sv)));
			}
			if (!is_dup && seen_hash.has_value()) {
				seen_hash->insert(name_sv);
			}

			if (auto ok = skip_ws_checked(); !ok) {
				staging_members.resize(members_start);
				return unexpected(move(ok).error());
			}
			if (tok.pos >= tok.src.size() || tok.src[tok.pos] != ':') {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ':'"));
			}
			tok.adv();

			// For first_wins, snapshot before parsing the duplicate value.
			StorageMark mark{};
			if (is_dup && dup_policy == DuplicateKeyPolicy::first_wins) {
				mark = StorageMark{
					store.nodes.size(),
					store.string_arena.size(),
					store.array_children.size(),
					store.object_members.size()};
			}

			auto val = parse_value(depth + 1);
			if (!val) {
				staging_members.resize(members_start);
				return unexpected(move(val).error());
			}

			if (is_dup) {
				if (dup_policy == DuplicateKeyPolicy::first_wins) {
					// Discard the newly parsed value; restore storage to mark.
					destroy_nodes_range(mark.nodes);
					store.nodes.resize(mark.nodes);
					store.string_arena.resize(mark.string_arena);
					store.array_children.resize(mark.array_children);
					store.object_members.resize(mark.object_members);
				} else {
					// last_wins: update the first occurrence's val_node.
					for (SZ i = members_start; i < staging_members.size(); ++i) {
						auto &m = staging_members[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
						if (store.bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags)) == name_sv) {
							m.val_node = static_cast<u32>(*val);
							break;
						}
					}
				}
			} else {
				staging_members.push_back(
					{parsed_name->off, parsed_name->len, static_cast<u32>(*val), parsed_name->flags});

				// Promote linear → hash once we cross the threshold.
				SZ const cur_count = staging_members.size() - members_start;
				if (!seen_hash.has_value() && cur_count > kDedupLinearMax) {
					seen_hash.emplace();
					SZ reserve_count = cur_count;
					if (cur_count <= NL<SZ>::max() - cur_count) {
						reserve_count += cur_count;
					}
					seen_hash->reserve(reserve_count);
					for (SZ i = members_start; i < staging_members.size(); ++i) {
						auto const &m = staging_members[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
						seen_hash->insert(store.bytes_at(m.name_off, m.name_len, static_cast<u8>(m.name_flags)));
					}
				}
			}

			if (auto ok = skip_ws_checked(); !ok) {
				staging_members.resize(members_start);
				return unexpected(move(ok).error());
			}
			if (tok.pos >= tok.src.size()) {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			if (tok.src[tok.pos] == '}') {
				tok.adv();
				SZ const len = staging_members.size() - members_start;
				SZ const ms = store.object_members.size();
				store.object_members.insert(
					store.object_members.end(),
					staging_members.begin() + static_cast<std::ptrdiff_t>(members_start),
					staging_members.end());
				staging_members.resize(members_start);
				store.nodes.push_back(detail::node_object(static_cast<u32>(ms), static_cast<u32>(len)));
				SZ const obj_node_idx = store.nodes.size() - 1;
				// Auto-warm if warm_threshold is set and object is large enough.
				if (opts.warm_threshold.has_value()
					&& len >= static_cast<SZ>(*opts.warm_threshold)
					&& len >= kHashThreshold) {
					u32 const cap = detail::clamped_capacity(static_cast<u32>(len));
					if (cap > 0) {
						ObjHashTable *ht = ObjHashTable::create(cap, static_cast<u32>(len), store.hash_mr_);
						if (ht != nullptr) {
							if (detail::build_table(*ht, &store, ms, len)) {
								store.nodes[obj_node_idx].hash_idx_raw =
									ht; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
							} else {
								ObjHashTable::destroy(ht);
								store.nodes[obj_node_idx].hash_idx_raw =
									kHashBuildFailedSentinel; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
							}
						}
					}
				}
				return obj_node_idx;
			}
			if (tok.src[tok.pos] != ',') {
				staging_members.resize(members_start);
				return unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'"));
			}
			tok.adv();
			if (opts.mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					staging_members.resize(members_start);
					return unexpected(move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
					tok.adv();
					SZ const len2 = staging_members.size() - members_start;
					SZ const ms2 = store.object_members.size();
					store.object_members.insert(
						store.object_members.end(),
						staging_members.begin() + static_cast<std::ptrdiff_t>(members_start),
						staging_members.end());
					staging_members.resize(members_start);
					store.nodes.push_back(detail::node_object(static_cast<u32>(ms2), static_cast<u32>(len2)));
					SZ const obj2 = store.nodes.size() - 1;
					if (opts.warm_threshold.has_value()
						&& len2 >= static_cast<SZ>(*opts.warm_threshold)
						&& len2 >= kHashThreshold) {
						u32 const cap2 = detail::clamped_capacity(static_cast<u32>(len2));
						if (cap2 > 0) {
							ObjHashTable *ht2 = ObjHashTable::create(cap2, static_cast<u32>(len2), store.hash_mr_);
							if (ht2 != nullptr) {
								if (detail::build_table(*ht2, &store, ms2, len2)) {
									store.nodes[obj2].hash_idx_raw =
										ht2; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
								} else {
									ObjHashTable::destroy(ht2);
									store.nodes[obj2].hash_idx_raw =
										kHashBuildFailedSentinel; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
								}
							}
						}
					}
					return obj2;
				}
			}
		}
	}
	[[nodiscard]] expected<SZ, JsonError> parse_number() {
		SZ const start = tok.pos;
		auto lex_result = tok.parse_number_lexeme();
		if (!lex_result) {
			return unexpected(move(lex_result).error());
		}
		SV const lex = *lex_result;
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
	SZ input_size,
	JsonParseOptions const &opts) noexcept {
	// 4 GiB hard ceiling — Fix F / Correction P. Unbypassable by
	// max_input_size = no_limit because Node::off / Node::len /
	// array_children entries are all u32.
	constexpr SZ kU32Ceiling = (SZ{1} << 32) - 1;
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
[[nodiscard]] inline expected<void, JsonError> parse_inplace(
	DocumentStorage &store,
	JsonParseOptions const &opts) {
	SZ const reserve_n = max<SZ>(64, store.input_view.size() / 16 + 16);
	store.nodes.reserve(reserve_n);
	store.array_children.reserve(reserve_n);
	store.object_members.reserve(reserve_n);
	store.string_arena.reserve(store.input_view.size());

	TreeBuilder tb{
		.tok =
			Tokenizer{
					  .src = store.input_view,
					  .store = store,
					  .bom_prefix_bytes = store.bom_prefix_bytes,
					  .mode = opts.mode},
		.store = store,
		.opts = opts,
		.staging = {},
		.staging_members = {}
    };
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return unexpected(move(ok).error());
	}
	if (tb.tok.pos >= store.input_view.size()) {
		return unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}
	auto root = tb.parse_value(0);
	if (!root) {
		return unexpected(move(root).error());
	}
	store.root_node = static_cast<u32>(*root);
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return unexpected(move(ok).error());
	}
	if (tb.tok.pos < store.input_view.size()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source =
					JsonSourceLocation{
									   .offset = tb.tok.pos + store.bom_prefix_bytes,
									   .line = tb.tok.line,
									   .column = tb.tok.col},
				.message = "trailing content after value"
        });
	}
	return {};
}
[[nodiscard]] inline expected<Document, JsonError> parse_with_storage(
	DocumentStorage &storage_ref,
	UP<DocumentStorage> storage,
	JsonParseOptions const &opts) {
	// R1 / Polish AA — pre-size the three growth vectors. JSON has roughly
	// one node per 8–16 bytes of input on typical payloads; reserving ahead
	// of the parse skips the geometric realloc cycle on >100 KB inputs.
	// Floor at 64 preserves the tiny-input baseline. A precise structural
	// prescan was tried and rejected — the branchful in-S scan
	// (~1 GB/s) cost more than the realloc copies it saved on the
	// 4 KB / 200 KB corpora in this bench.
	SZ const reserve_n = max<SZ>(64, storage_ref.input_view.size() / 16 + 16);
	storage->nodes.reserve(reserve_n);
	storage->array_children.reserve(reserve_n);
	storage->object_members.reserve(reserve_n);
	// Reserve string_arena up-front so it never reallocates mid-parse.
	// The dedup hash set in parse_object stores SVs into string_arena;
	// any realloc would dangle them (TSan UAF, json.cxx:2598). Decoded
	// strings are always ≤ input size (escapes only ever shrink), so the
	// input length is a safe upper bound.
	storage_ref.string_arena.reserve(storage_ref.input_view.size());

	TreeBuilder tb{
		.tok =
			Tokenizer{
					  .src = storage_ref.input_view,
					  .store = storage_ref,
					  .bom_prefix_bytes = storage_ref.bom_prefix_bytes,
					  .mode = opts.mode},
		.store = storage_ref,
		.opts = opts,
		.staging = {},
		.staging_members = {}
    };
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return unexpected(move(ok).error());
	}
	if (tb.tok.pos >= storage_ref.input_view.size()) {
		return unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}

	auto root = tb.parse_value(0);
	if (!root) {
		return unexpected(move(root).error());
	}
	storage->root_node = static_cast<u32>(*root);

	if (auto ok = tb.skip_ws_checked(); !ok) {
		return unexpected(move(ok).error());
	}
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
expected<ArenaDocument, JsonError> JsonArena::parse_into(
	SV input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}
	// Clear storage for reuse — hash tables use ::operator delete (safe before mbr release).
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_->nodes.clear();
	storage_->string_arena.clear();
	storage_->array_children.clear();
	storage_->object_members.clear();
	storage_->owned_input.reset();
	storage_->root_node = 0;
	storage_->bom_prefix_bytes = 0;

	storage_->owned_input = make_unique<S>(input);
	SV src = *storage_->owned_input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage_->input_view = src;

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return unexpected(move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
expected<ArenaDocument, JsonError> JsonArena::parse_borrowed_into(
	SV input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_->nodes.clear();
	storage_->string_arena.clear();
	storage_->array_children.clear();
	storage_->object_members.clear();
	storage_->owned_input.reset();
	storage_->root_node = 0;
	storage_->bom_prefix_bytes = 0;

	SV src = input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage_->input_view = src;

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return unexpected(move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
expected<ArenaDocument, JsonError> JsonArena::parse_moved_into(
	S input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_->nodes.clear();
	storage_->string_arena.clear();
	storage_->array_children.clear();
	storage_->object_members.clear();
	storage_->owned_input.reset();
	storage_->root_node = 0;
	storage_->bom_prefix_bytes = 0;

	storage_->owned_input = make_unique<S>(move(input));
	SV src = *storage_->owned_input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage_->input_view = src;

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return unexpected(move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
void JsonArena::reset() noexcept {
	++generation_;
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_ = nullptr; // ~DocumentStorage: pmr dealloc is no-op on monotonic
	mbr_.release(); // actually frees the slab
	storage_ = make_unique<DocumentStorage>(&mbr_);
}
export namespace conflux::json {

// Copies the input into the Document's owned buffer. Number lexemes index
// directly into that buffer (zero-copy on read paths).
expected<Document, JsonError> parse(
	SV input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	storage->owned_input = make_unique<S>(input);
	SV src = *storage->owned_input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}
// Move-in overload: avoids the input copy. Constrained to actual S
// rvalues so that const char[N] literals select the (SV) overload
// without ambiguity.
template<class S>
	requires same_as<std::remove_cvref_t<S>, S> && (!std::is_lvalue_reference_v<S>)
expected<Document, JsonError> parse(
	S &&input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	storage->owned_input = make_unique<S>(forward<S>(input));
	SV src = *storage->owned_input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
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
	SV input,
	JsonParseOptions const &opts = {}) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}

	auto storage = make_unique<DocumentStorage>();
	SV src = input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}
expected<Document, JsonError> parse_borrowed_unsafe(
	SV input,
	JsonParseOptions const &opts = {}) {
	return parse_borrowed(input, opts);
}
expected<Document, JsonError> parse_view(
	SV input,
	JsonParseOptions const &opts = {}) {
	return parse_borrowed(input, opts);
}

// Deleted rvalue overload (Correction T) — borrowing requires the caller to
// own the bytes. Constrained the same way as the parse(S&&) overload
// so const char[N] still selects parse_borrowed(SV).
template<class S>
	requires same_as<std::remove_cvref_t<S>, S> && (!std::is_lvalue_reference_v<S>)
expected<Document, JsonError> parse_borrowed(S &&, JsonParseOptions const & = {}) = delete;
template<class S>
	requires same_as<std::remove_cvref_t<S>, S> && (!std::is_lvalue_reference_v<S>)
expected<Document, JsonError> parse_borrowed_unsafe(S &&, JsonParseOptions const & = {}) = delete;
template<class S>
	requires same_as<std::remove_cvref_t<S>, S> && (!std::is_lvalue_reference_v<S>)
expected<Document, JsonError> parse_view(S &&, JsonParseOptions const & = {}) = delete;
// pmr-injecting overloads — caller supplies the memory resource.
// The resource must outlive every Document (and NodeRef) derived from it.
expected<Document, JsonError> parse(
	SV input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}
	auto storage = make_unique<DocumentStorage>(resource);
	storage->owned_input = make_unique<S>(input);
	SV src = *storage->owned_input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}
expected<Document, JsonError> parse_borrowed(
	SV input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return unexpected(move(ok).error());
	}
	auto storage = make_unique<DocumentStorage>(resource);
	SV src = input;
	constexpr SV kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<u32>(kBOM.size());
	}
	storage->input_view = src;
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, move(storage), opts);
}
expected<Document, JsonError> parse_borrowed_unsafe(
	SV input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}
expected<Document, JsonError> parse_view(
	SV input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}

} // namespace conflux::json

// ---------------------------------------------------------------------------
// has_json_codec — forward-declared here so builders can use it in requires
// ---------------------------------------------------------------------------

export template<class M>
using JsonConstraintFn = expected<void, JsonError> (*)(M const &);

export template<class T>
struct JsonMembers;
export template<class T>
struct JsonCodec;
namespace detail {

template<class T, class = void>
struct has_codec_spec : std::false_type {};
template<class T, class = void>
struct has_members_spec : std::false_type {};
template<class T>
struct is_optional : std::false_type {};
template<class T>
struct is_optional<Opt<T>> : std::true_type {};
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
struct is_vector_of : std::false_type {};
template<class T>
struct is_vector_of<V<T>> : std::true_type {};
template<class T>
constexpr bool is_vector_of_v = is_vector_of<T>::value;
template<class T>
struct is_std_array : std::false_type {};
template<class T, SZ N>
struct is_std_array<A<T, N>> : std::true_type {};
template<class T>
constexpr bool is_std_array_v = is_std_array<T>::value;
template<class T>
struct is_pair : std::false_type {};
template<class A, class B>
struct is_pair<P<A, B>> : std::true_type {};
template<class T>
constexpr bool is_pair_v = is_pair<T>::value;
template<class T>
struct is_tuple_of : std::false_type {};
template<class... Ts>
struct is_tuple_of<Tup<Ts...>> : std::true_type {};
template<class T>
constexpr bool is_tuple_of_v = is_tuple_of<T>::value;
template<class T>
struct is_map_type : std::false_type {};
template<class K, class Vt>
struct is_map_type<M<K, Vt>> : std::true_type {};
template<class T>
constexpr bool is_map_type_v = is_map_type<T>::value;
template<class T>
struct is_unordered_map_type : std::false_type {};
template<class K, class Vt>
struct is_unordered_map_type<UM<K, Vt>> : std::true_type {};
template<class T>
constexpr bool is_unordered_map_type_v = is_unordered_map_type<T>::value;
struct PathFrame {
	enum class Kind : u8 {
		member,
		index,
	} kind;
	SV member_name{};
	SZ index{};
};
[[nodiscard]] inline JsonPath materialize_path(
	span<PathFrame const> frames) {
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
	S built_input;
	bool root_set{false};
	SZ root_node{};
	bool child_active{false}; // true when any descendant of ValueBuilder is open
	SZ active_depth{}; // depth of the innermost currently-active builder
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
	SZ name_off{}; // insert_member: name offset in string_arena
	SZ name_len{}; // insert_member: name length
	SZ arena_start{}; // rollback point for string_arena
	bool saved_root_set{}; // set_root only: root_set value before child was opened
	V<SZ> *parent_local_children{}; // append_child only: parent's staging V
	V<MemberEntry> *parent_local_members{}; // insert_member only: parent's staging V
};
// Holds the active object/A being built:
struct ChildFrame {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : u8 {
		object,
		A,
	};
	Kind kind;
	SZ depth{}; // this builder's own depth level (1 = direct child of ValueBuilder)
	BuilderState *state{};
	bool committed{};
	ParentSlot parent; // parent.arena_start is the rollback point for string_arena
	V<SZ> local_children; // staged A child node indices (A builders only)
	V<MemberEntry> local_members; // staged object members (object builders only)
	V<char const *> local_external_ptrs_; // parallel to local_members; non-null only for kMemberExternalView entries
	// Per-session duplicate detection for ObjectBuilder (kind==object only).
	UM<S, SZ> dup_check;
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
	expected<void, JsonError> do_insert_node(SV name, SZ node_idx);
	expected<void, JsonError> do_insert_node_view(SV name, SZ node_idx);

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
	~ObjectBuilder() noexcept { abort_if_open(); }
	expected<void, JsonError> insert_null(SV name);
	expected<void, JsonError> insert_bool(SV name, bool v);
	expected<void, JsonError> insert_string(SV name, SV value);
	expected<void, JsonError> insert_string_checked(SV name, SV value);
	expected<void, JsonError> insert_string_borrowed_name(SV name, SV value);
	expected<void, JsonError> insert_string_borrowed(SV name, SV value);
	expected<void, JsonError> insert_number(SV name, SV lexeme);
	expected<void, JsonError> insert_i64(SV name, i64 v);
	expected<void, JsonError> insert_u64(SV name, u64 v);
	expected<void, JsonError> insert_f64(SV name, double v);

	expected<ObjectBuilder, JsonError> insert_object(SV name);
	expected<ArrayBuilder, JsonError> insert_array(SV name);

	template<class T>
		requires has_json_codec<T>
	expected<void, JsonError> insert(SV name, T const &value);
	// NOLINTNEXTLINE(bugprone-exception-escape)
	void commit() && noexcept {
		if ((frame_.state == nullptr) || frame_.committed) {
			return;
		}
		auto *st = frame_.state;
		SZ const mem_start = st->store.object_members.size();
		for (auto m: frame_.local_members) { // copy: may patch name_off for external ptrs
			if ((m.name_flags & kMemberExternalView) != 0) {
				// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
				char const *ptr = frame_.local_external_ptrs_[m.name_off];
				m.name_off = static_cast<u32>(st->store.external_ptrs_.size());
				st->store.external_ptrs_.push_back(ptr);
			}
			st->store.object_members.push_back(m);
		}
		SZ const cnt = frame_.local_members.size();
		st->store.nodes.push_back(detail::node_object(static_cast<u32>(mem_start), static_cast<u32>(cnt)));
		SZ const node_idx = st->store.nodes.size() - 1;
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
		frame_.local_external_ptrs_.clear();
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
			  .kind = ChildFrame::Kind::A,
			  .state = st,
			  .parent = parent,
			  .local_children = {},
			  .local_members = {},
			  .local_external_ptrs_ = {},
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
	expected<void, JsonError> append_string(SV value);
	expected<void, JsonError> append_string_checked(SV value);
	expected<void, JsonError> append_string_borrowed(SV value);
	expected<void, JsonError> append_number(SV lexeme);
	expected<void, JsonError> append_i64(i64 v);
	expected<void, JsonError> append_u64(u64 v);
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
		SZ const child_start = st->store.array_children.size();
		for (SZ const idx: frame_.local_children) {
			st->store.array_children.push_back(static_cast<u32>(idx));
		}
		SZ const cnt = frame_.local_children.size();
		st->store.nodes.push_back(detail::node_array(static_cast<u32>(child_start), static_cast<u32>(cnt)));
		SZ const node_idx = st->store.nodes.size() - 1;
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
expected<SZ, JsonError> encode_into(BuilderState *st, T const &value);

template<class T>
expected<void, JsonError> encode_dispatch(ValueBuilder &b, T const &value);

} // namespace detail
export class ValueBuilder {
	UP<BuilderState> owned_;
	BuilderState *state_{};

	friend class ObjectBuilder;
	friend class ArrayBuilder;
	template<class T>
	expected<SZ, JsonError> friend detail::encode_into(BuilderState *, T const &);
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
		SV sv) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		SZ const off = state_->built_input.size();
		state_->built_input.append(sv.data(), sv.size());
		return set_node(detail::make_string(static_cast<u32>(off), static_cast<u32>(sv.size()), kStorageInputView));
	}
	expected<void, JsonError> set_number(SV lexeme);
	expected<void, JsonError> set_i64(
		i64 v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		SZ const off = state_->built_input.size();
		A<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
		SZ const len = state_->built_input.size() - off;
		return set_node(
			detail::make_number_int(
				static_cast<u32>(off),
				static_cast<u32>(len),
				kStorageInputView | kRawJsonSlice,
				v));
	}
	expected<void, JsonError> set_u64(
		u64 v) {
		auto ok = check_can_set();
		if (!ok) {
			return ok;
		}
		SZ const off = state_->built_input.size();
		A<char, 22> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
		SZ const len = state_->built_input.size() - off;
		if (v <= static_cast<u64>(NL<i64>::max())) {
			return set_node(
				detail::make_number_int(
					static_cast<u32>(off),
					static_cast<u32>(len),
					kStorageInputView | kRawJsonSlice,
					static_cast<i64>(v)));
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
		SZ const off = state_->built_input.size();
		A<char, 32> buf{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		auto [p, ec] = to_chars(buf.data(), buf.data() + buf.size(), v);
		state_->built_input.append(buf.data(), static_cast<SZ>(p - buf.data()));
		SZ const len = state_->built_input.size() - off;
		SV const lex = SV{state_->built_input.data() + off, len};
		bool const is_int = lex.find_first_of(".eE") == SV::npos;
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
		constexpr SZ kU32Ceiling = (SZ{1} << 32) - 1;
		if (state_->built_input.size() >= kU32Ceiling) {
			return unexpected(
				JsonError{
					.stage = JsonStage::build,
					.code = JsonIssueCode::input_too_large,
					.message = "Builder input buffer exceeds 4 GiB hard ceiling"});
		}
		auto storage = make_unique<DocumentStorage>(move(state_->store));
		storage->root_node = static_cast<u32>(state_->root_node);
		storage->owned_input = make_unique<S>(move(state_->built_input));
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
expected<SZ, JsonError> encode_into(
	BuilderState *st,
	T const &value) {
	SZ const nodes_saved = st->store.nodes.size();
	SZ const arena_saved = st->built_input.size();
	SZ const arr_saved = st->store.array_children.size();
	SZ const obj_saved = st->store.object_members.size();
	bool const root_set_saved = st->root_set;
	bool const child_active_saved = st->child_active;
	SZ const active_depth_saved = st->active_depth;

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
	SZ const node_idx = st->root_node;
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
// ---------------------------------------------------------------------------
// Nullable<T>
// ---------------------------------------------------------------------------

export template<class T>
class Nullable {
	Opt<T> val_;

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
		requires std::convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) const & {
		return val_ ? *val_ : static_cast<T>(forward<U>(fallback));
	}
	template<class U>
		requires std::convertible_to<U, T>
	[[nodiscard]] constexpr T value_or(
		U &&fallback) && {
		return val_ ? move(*val_) : static_cast<T>(forward<U>(fallback));
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
	SZ operator ()(
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
	SV name;
	M T::*pointer;
};
export template<class T, class M>
constexpr JsonMember<T, M> json_member(
	SV name,
	M T::*p) {
	return {name, p};
}
namespace detail {

template<class T>
struct has_codec_spec<
	T,
	std::void_t<
		decltype(JsonCodec<T>::decode(std::declval<NodeRef>())),
		decltype(JsonCodec<T>::encode(std::declval<ValueBuilder &>(), std::declval<T const &>()))>> : std::true_type {};
template<class T>
struct has_members_spec<T, std::void_t<decltype(JsonMembers<T>::members())>>
	: std::bool_constant<std::default_initializable<T>> {};
// Overload set: extract JsonMember from plain or constrained member entry.
template<class T, class M>
[[nodiscard]] constexpr JsonMember<T, M> const &jm_member(
	JsonMember<T, M> const &jm) noexcept {
	return jm;
}
template<class T, class M>
[[nodiscard]] constexpr JsonMember<T, M> const &jm_member(
	Tup<JsonMember<T, M>, JsonConstraintFn<M>> const &t) noexcept {
	return get<0>(t);
}
// Overload set: extract constraint fn-ptr (nullptr = none).
template<class T, class M>
[[nodiscard]] constexpr JsonConstraintFn<M> jm_constraint(
	JsonMember<T, M> const &) noexcept {
	return nullptr;
}
template<class T, class M>
[[nodiscard]] constexpr JsonConstraintFn<M> jm_constraint(
	Tup<JsonMember<T, M>, JsonConstraintFn<M>> const &t) noexcept {
	return get<1>(t);
}

} // namespace detail

// Built-in specializations declared here, defined below.
template<>
struct JsonCodec<bool>;
template<>
struct JsonCodec<i64>;
template<>
struct JsonCodec<u64>;
template<>
struct JsonCodec<double>;
template<>
struct JsonCodec<S>;
template<>
struct JsonCodec<SV>;
template<class T>
struct JsonCodec<Opt<T>>;
template<class T>
struct JsonCodec<Nullable<T>>;
template<class T>
struct JsonCodec<V<T>>;
template<class T, SZ N>
struct JsonCodec<A<T, N>>;
template<class A, class B>
struct JsonCodec<P<A, B>>;
template<class... Ts>
struct JsonCodec<Tup<Ts...>>;
template<class T>
struct JsonCodec<M<S, T>>;
template<class T>
struct JsonCodec<UM<S, T>>;

export template<class T>
expected<T, JsonError> decode(NodeRef root, JsonDecodeOptions const &opts = {});

export template<class T>
expected<T, JsonError> decode(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
expected<T, JsonError> decode_next(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
expected<T, JsonError> decode_full(JsonReader &reader, JsonDecodeOptions const &opts = {});
export template<class T>
expected<T, JsonError>
decode_full(SV input, JsonParseOptions const &parse_opts = {}, JsonDecodeOptions const &decode_opts = {});
export template<class T>
expected<T, JsonError> decode(
	Document const &d,
	JsonDecodeOptions const &opts = {}) {
	return decode<T>(d.root(), opts);
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
	static constexpr SV type_name() { return "bool"; }
};
template<>
struct JsonCodec<i64> {
	static expected<i64, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return unexpected(move(num).error());
		}
		return num->to_i64();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		i64 v) {
		return b.set_i64(v);
	}
	static constexpr SV type_name() { return "i64"; }
};
template<>
struct JsonCodec<u64> {
	static expected<u64, JsonError> decode(
		NodeRef n) {
		auto num = n.as_number();
		if (!num) {
			return unexpected(move(num).error());
		}
		return num->to_u64();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		u64 v) {
		return b.set_u64(v);
	}
	static constexpr SV type_name() { return "u64"; }
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
	static constexpr SV type_name() { return "double"; }
};
template<>
struct JsonCodec<S> {
	static expected<S, JsonError> decode(
		NodeRef n) {
		auto sv = n.as_string();
		if (!sv) {
			return unexpected(move(sv).error());
		}
		return S{*sv};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		S const &v) {
		return b.set_string(v);
	}
	static constexpr SV type_name() { return "string"; }
};
template<>
struct JsonCodec<SV> {
	static expected<SV, JsonError> decode(
		NodeRef n) {
		return n.as_string();
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		SV v) {
		return b.set_string(v);
	}
	static constexpr SV type_name() { return "SV"; }
};
template<class T>
struct JsonCodec<Opt<T>> {
	static expected<Opt<T>, JsonError> decode(
		NodeRef n) {
		if (n.is_null()) {
			if constexpr (detail::is_nullable_type<T>::value) {
				auto v = ::decode<T>(n);
				if (!v) {
					return unexpected(move(v).error());
				}
				return Opt<T>{move(*v)};
			} else {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::wrong_kind,
						.expected_kind = JsonKind::null,
						.actual_kind = JsonKind::null,
						.message =
							"explicit JSON null is not accepted for Opt<T>; use Nullable<T> for nullable fields"});
			}
		}
		auto v = ::decode<T>(n);
		if (!v) {
			return unexpected(move(v).error());
		}
		return Opt<T>{move(*v)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		Opt<T> const &v) {
		if (!v) {
			return b.set_null();
		}
		return JsonCodec<T>::encode(b, *v);
	}
	static constexpr SV type_name() { return "Opt"; }
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
	static constexpr SV type_name() { return "Nullable"; }
};
namespace detail {

template<class T>
expected<V<T>, JsonError> decode_array_elements(
	ArrayView const &arr) {
	V<T> result;
	result.reserve(arr.size());
	for (SZ i = 0; i < arr.size(); ++i) {
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
struct JsonCodec<V<T>> {
	static expected<V<T>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		return detail::decode_array_elements<T>(*arr);
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		V<T> const &v) {
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
	static constexpr SV type_name() { return "V"; }
};
template<class T, SZ N>
struct JsonCodec<A<T, N>> {
	static expected<A<T, N>, JsonError> decode(
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
					.target_type = S{type_name()},
					.container_size = N,
					.message = format("expected array of length {}, got {}", N, arr->size())});
		}
		A<T, N> result{};
		for (SZ i = 0; i < N; ++i) {
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
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			result[i] = move(*v);
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		A<T, N> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		for (SZ i = 0; i < N; ++i) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			if (auto ok = arr.template append<T>(v[i]); !ok) {
				return unexpected(move(ok).error());
			}
		}
		move(arr).commit();
		return {};
	}
	static constexpr SV type_name() { return "array"; }
};
template<class A, class B>
struct JsonCodec<P<A, B>> {
	static expected<P<A, B>, JsonError> decode(
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
					.target_type = S{type_name()},
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
		return P<A, B>{move(*first), move(*second)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		P<A, B> const &v) {
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
	static constexpr SV type_name() { return "P"; }
};
template<class... Ts>
struct JsonCodec<Tup<Ts...>> {
	static expected<Tup<Ts...>, JsonError> decode(
		NodeRef n) {
		auto arr = n.as_array();
		if (!arr) {
			return unexpected(move(arr).error());
		}
		constexpr SZ N = sizeof...(Ts);
		if (arr->size() != N) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.target_type = S{type_name()},
					.container_size = N,
					.message = format("expected array of length {}, got {}", N, arr->size())});
		}
		Tup<Ts...> result{};
		bool ok = true;
		JsonError first_err;
		[&]<SZ... Is>(std::index_sequence<Is...>) {
			(([&]<SZ I>() {
				 if (!ok) {
					 return;
				 }
				 auto elem = arr->element(I);
				 if (!elem) {
					 ok = false;
					 first_err = move(elem).error();
					 return;
				 }
				 auto v = ::decode<TEt<I, Tup<Ts...>>>(*elem);
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
		}(std::make_index_sequence<N>{});
		if (!ok) {
			return unexpected(move(first_err));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		Tup<Ts...> const &v) {
		auto arr_res = b.begin_array();
		if (!arr_res) {
			return unexpected(move(arr_res).error());
		}
		auto &arr = *arr_res;
		bool ok = true;
		JsonError first_err;
		[&]<SZ... Is>(std::index_sequence<Is...>) {
			(([&]<SZ I>() {
				 if (!ok) {
					 return;
				 }
				 auto res = arr.template append<TEt<I, Tup<Ts...>>>(get<I>(v));
				 if (!res) {
					 ok = false;
					 first_err = move(res).error();
				 }
			 }.template operator ()<Is>()),
			 ...);
		}(std::make_index_sequence<sizeof...(Ts)>{});
		if (!ok) {
			return unexpected(move(first_err));
		}
		move(arr).commit();
		return {};
	}
	static constexpr SV type_name() { return "Tup"; }
};
template<class T>
struct JsonCodec<M<S, T>> {
	static expected<M<S, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		M<S, T> result;
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return unexpected(move(v).error().with_prefix(prefix));
			}
			result.emplace(S{name}, move(*v));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		M<S, T> const &v) {
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
	static constexpr SV type_name() { return "M"; }
};
template<class T>
struct JsonCodec<UM<S, T>> {
	static expected<UM<S, T>, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}
		UM<S, T> result;
		for (auto const &[name, val]: obj->members()) {
			auto v = ::decode<T>(val);
			if (!v) {
				JsonPath prefix;
				prefix.push_member(name);
				return unexpected(move(v).error().with_prefix(prefix));
			}
			result.emplace(S{name}, move(*v));
		}
		return result;
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		UM<S, T> const &v) {
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
	static constexpr SV type_name() { return "UM"; }
};
// ---------------------------------------------------------------------------
// JsonReader-based decode
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline expected<void, JsonError> skip_remaining_reader(
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
			return unexpected(move(ne).error());
		}
		if (!*ne) {
			return unexpected(
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
expected<T, JsonError> decode_from_event(JsonReader &r, JsonReader::Event ev, JsonDecodeOptions const &opts);
template<class T>
expected<T, JsonError> decode_with_reader(
	JsonReader &r,
	JsonDecodeOptions const &opts) {
	auto ne = r.next();
	if (!ne) {
		return unexpected(move(ne).error());
	}
	if (!*ne) {
		return unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::unexpected_eof,
				.message = "unexpected end of input"});
	}
	return decode_from_event<T>(r, **ne, opts);
}
template<class T>
expected<T, JsonError> decode_from_event(
	JsonReader &r,
	JsonReader::Event ev,
	JsonDecodeOptions const &opts) {
	using Ev = JsonReader::Event;

	if constexpr (same_as<T, bool>) {
		if (ev != Ev::bool_value) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected bool"});
		}
		return r.bool_val();
	} else if constexpr (same_as<T, i64>) {
		if (ev != Ev::number_value) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		return r.number_val().to_i64();
	} else if constexpr (same_as<T, u64>) {
		if (ev != Ev::number_value) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		return r.number_val().to_u64();
	} else if constexpr (same_as<T, double>) {
		if (ev != Ev::number_value) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		return r.number_val().to_f64();
	} else if constexpr (same_as<T, S>) {
		if (ev != Ev::string_value) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected string"});
		}
		S out;
		auto res = r.string_token().append_decoded_to(out);
		if (!res) {
			return unexpected(move(res).error());
		}
		return out;
	} else if constexpr (same_as<T, SV>) {
		static_assert(!same_as<T, SV>, "decode<string_view>(JsonReader&) is deleted; use std::string");
	} else if constexpr (is_optional<T>::value) {
		using Inner = typename T::value_type;
		if (ev == Ev::null_value) {
			return T{};
		}
		auto v = decode_from_event<Inner>(r, ev, opts);
		if (!v) {
			return unexpected(move(v).error());
		}
		return T{move(*v)};
	} else if constexpr (is_nullable_type<T>::value) {
		if (ev == Ev::null_value) {
			return T{};
		}
		using Inner = nullable_inner_t<T>;
		auto v = decode_from_event<Inner>(r, ev, opts);
		if (!v) {
			return unexpected(move(v).error());
		}
		return T{move(*v)};
	} else if constexpr (is_vector_of_v<T>) {
		using E = typename T::value_type;
		if (ev != Ev::begin_array) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		T result;
		while (true) {
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return result;
			}
			auto elem = decode_from_event<E>(r, **ne, opts);
			if (!elem) {
				return unexpected(move(elem).error());
			}
			result.push_back(move(*elem));
		}
	} else if constexpr (is_std_array_v<T>) {
		using E = typename T::value_type;
		constexpr SZ N = std::tuple_size_v<T>;
		if (ev != Ev::begin_array) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		T result;
		for (SZ i = 0; i < N; ++i) {
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**ne == Ev::end_array) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = format("expected array of length {}", N)});
			}
			auto elem = decode_from_event<E>(r, **ne, opts);
			if (!elem) {
				return unexpected(move(elem).error());
			}
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
			result[i] = move(*elem);
		}
		auto ne = r.next();
		if (!ne) {
			return unexpected(move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = format("expected array of length {}", N)});
		}
		return result;
	} else if constexpr (is_pair_v<T>) {
		using FA = typename T::first_type;
		using FB = typename T::second_type;
		if (ev != Ev::begin_array) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		T result;
		{
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in pair"});
			}
			auto v = decode_from_event<FA>(r, **ne, opts);
			if (!v) {
				return unexpected(move(v).error());
			}
			result.first = move(*v);
		}
		{
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in pair"});
			}
			auto v = decode_from_event<FB>(r, **ne, opts);
			if (!v) {
				return unexpected(move(v).error());
			}
			result.second = move(*v);
		}
		{
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne || **ne != Ev::end_array) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = "expected pair of length 2"});
			}
		}
		return result;
	} else if constexpr (is_tuple_of_v<T>) {
		if (ev != Ev::begin_array) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		T result;
		bool ok = true;
		JsonError first_err;
		constexpr SZ N = std::tuple_size_v<T>;
		[&]<SZ... Is>(std::index_sequence<Is...>) {
			(([&]() {
				 if (!ok) {
					 return;
				 }
				 auto ne = r.next();
				 if (!ne) {
					 ok = false;
					 first_err = move(ne).error();
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
						 .message = format("expected tuple of length {}", N)};
					 return;
				 }
				 using E = std::tuple_element_t<Is, T>;
				 auto v = decode_from_event<E>(r, **ne, opts);
				 if (!v) {
					 ok = false;
					 first_err = move(v).error();
					 return;
				 }
				 get<Is>(result) = move(*v);
			 })(),
			 ...);
		}(std::make_index_sequence<N>{});
		if (!ok) {
			return unexpected(move(first_err));
		}
		auto ne = r.next();
		if (!ne) {
			return unexpected(move(ne).error());
		}
		if (!*ne || **ne != Ev::end_array) {
			return unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = format("expected tuple of length {}", N)});
		}
		return result;
	} else if constexpr (is_map_type_v<T> || is_unordered_map_type_v<T>) {
		using Vt = typename T::mapped_type;
		if (ev != Ev::begin_object) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected object"});
		}
		T result;
		while (true) {
			auto ne = r.next();
			if (!ne) {
				return unexpected(move(ne).error());
			}
			if (!*ne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object"});
			}
			if (**ne == Ev::end_object) {
				return result;
			}
			if (**ne != Ev::key) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::syntax_error,
						.message = "expected key"});
			}
			S key;
			auto key_res = r.key_token().append_decoded_to(key);
			if (!key_res) {
				return unexpected(move(key_res).error());
			}
			auto vne = r.next();
			if (!vne) {
				return unexpected(move(vne).error());
			}
			if (!*vne) {
				return unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object value"});
			}
			auto val = decode_from_event<Vt>(r, **vne, opts);
			if (!val) {
				return unexpected(move(val).error());
			}
			result.emplace(move(key), move(*val));
		}
	} else if constexpr (same_as<T, Document>) {
		SZ const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader(r, ev); !ok) {
			return unexpected(move(ok).error());
		}
		SV const slice = r.input().substr(start, r.pos() - start);
		return conflux::json::parse(slice);
	} else if constexpr (has_members_spec<T>::value) {
		if (ev != Ev::begin_object) {
			return unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected object"});
		}
		T result{};
		auto const members = JsonMembers<T>::members();
		bool ok = true;
		JsonError first_err;
		SZ const member_count = std::apply([](auto const &...ms) { return sizeof...(ms); }, members);
		V<bool> found(member_count, false);

		while (ok) {
			auto ne = r.next();
			if (!ne) {
				ok = false;
				first_err = move(ne).error();
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
					.message = "expected key"};
				break;
			}
			S key_name;
			if (auto kr = r.key_token().append_decoded_to(key_name); !kr) {
				ok = false;
				first_err = move(kr).error();
				break;
			}

			bool matched = false;
			apply(
				[&](auto const &...ms) {
					SZ idx = 0;
					(([&](auto const &entry) {
						 if (matched || !ok) {
							 ++idx;
							 return;
						 }
						 auto const &m = jm_member(entry);
						 if (SV{key_name} == m.name) {
							 matched = true;
							 // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
							 found[idx] = true;
							 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
							 auto vne = r.next();
							 if (!vne || !*vne) {
								 ok = false;
								 first_err = !vne ? move(vne).error() :
													JsonError{
														.stage = JsonStage::decode,
														.code = JsonIssueCode::unexpected_eof,
														.message = "EOF in object value"};
								 ++idx;
								 return;
							 }
							 auto decoded = decode_from_event<M>(r, **vne, opts);
							 if (!decoded) {
								 ok = false;
								 first_err = move(decoded).error();
								 ++idx;
								 return;
							 }
							 result.*m.pointer = move(*decoded);
							 auto cfn = jm_constraint(entry);
							 if (cfn != nullptr) {
								 if (auto cr = cfn(result.*m.pointer); !cr) {
									 ok = false;
									 first_err = move(cr).error();
									 first_err.member_name = S{m.name};
								 }
							 }
						 }
						 ++idx;
					 })(ms),
					 ...);
				},
				members);

			if (!matched && ok) {
				if (opts.unknown_members == UnknownMemberPolicy::reject) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.member_name = key_name,
						.message = format("unknown member: {}", key_name)};
				} else {
					auto vne = r.next();
					if (!vne) {
						ok = false;
						first_err = move(vne).error();
					} else if (!*vne) {
						ok = false;
						first_err = JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::unexpected_eof,
							.message = "EOF in object value"};
					} else if (auto skip_res = skip_remaining_reader(r, **vne); !skip_res) {
						ok = false;
						first_err = move(skip_res).error();
					}
				}
			}
		}

		if (!ok) {
			return unexpected(move(first_err));
		}

		apply(
			[&](auto const &...ms) {
				SZ idx = 0;
				(([&](auto const &entry) {
					 if (!ok) {
						 ++idx;
						 return;
					 }
					 auto const &m = jm_member(entry);
					 using M = std::remove_reference_t<decltype(result.*m.pointer)>;
					 // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
					 if (!found[idx] && !is_optional<M>::value) {
						 ok = false;
						 first_err = JsonError{
							 .stage = JsonStage::decode,
							 .code = JsonIssueCode::missing_member,
							 .member_name = S{m.name},
							 .message = format("missing member: {}", m.name)};
					 }
					 ++idx;
				 })(ms),
				 ...);
			},
			members);

		if (!ok) {
			return unexpected(move(first_err));
		}
		return result;
	} else if constexpr (has_codec_spec<T>::value) {
		// Generic fallback: re-parse as DOM and delegate to JsonCodec<T>::decode.
		// Used by any type with a custom JsonCodec that has no dedicated streaming branch.
		SZ const start = r.value_start_pos();
		if (auto ok = skip_remaining_reader(r, ev); !ok) {
			return unexpected(move(ok).error());
		}
		SV const slice = r.input().substr(start, r.pos() - start);
		auto doc = conflux::json::parse(slice);
		if (!doc) {
			return unexpected(move(doc).error());
		}
		return JsonCodec<T>::decode(doc->root());
	} else {
		static_assert(!same_as<T, T>, "No JsonReader support for type T");
	}
}

} // namespace detail
export template<class T>
expected<T, JsonError> decode(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	return decode_full<T>(reader, opts);
}
export template<class T>
expected<T, JsonError> decode_next(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	return detail::decode_with_reader<T>(reader, opts);
}
export template<class T>
expected<T, JsonError> decode_full(
	JsonReader &reader,
	JsonDecodeOptions const &opts) {
	auto value = decode_next<T>(reader, opts);
	if (!value) {
		return unexpected(move(value).error());
	}
	auto next = reader.next();
	if (!next) {
		return unexpected(move(next).error());
	}
	if (*next) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source = JsonSourceLocation{.offset = reader.value_start_pos()},
				.message = "trailing JSON value after document root"});
	}
	return move(value);
}
export template<class T>
expected<T, JsonError> decode_full(
	SV input,
	JsonParseOptions const &parse_opts,
	JsonDecodeOptions const &decode_opts) {
	JsonReader reader{input, parse_opts};
	return decode_full<T>(reader, decode_opts);
}
namespace detail {

template<class T>
expected<T, JsonError> decode_with_frames(
	NodeRef root,
	V<PathFrame> &frames,
	JsonDecodeOptions const &opts) {
	if constexpr (has_codec_spec<T>::value) {
		return JsonCodec<T>::decode(root);
	} else if constexpr (has_members_spec<T>::value) {
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
								 .member_name = S{m.name},
								 .message = format("missing member: {}", m.name)};
						 }
						 return;
					 }
					 frames.push_back({PathFrame::Kind::member, m.name, 0});
					 auto decoded = decode_with_frames<M>(*val, frames, opts);
					 if (!decoded) {
						 ok = false;
						 first_err = move(decoded).error();
						 if (first_err.path.empty()) {
							 first_err.path = materialize_path(frames);
						 }
						 frames.pop_back();
						 return;
					 }
					 frames.pop_back();
					 result.*m.pointer = move(*decoded);
					 auto cfn = jm_constraint(entry);
					 if (cfn != nullptr) {
						 if (auto cr = cfn(result.*m.pointer); !cr) {
							 ok = false;
							 first_err = move(cr).error();
							 first_err.member_name = S{m.name};
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
			return unexpected(move(first_err));
		}
		if (opts.unknown_members == UnknownMemberPolicy::reject) {
			for (auto const &[name, val]: obj->members()) {
				bool found = false;
				apply([&](auto const &...ms) { ((found = found || name == jm_member(ms).name), ...); }, members);
				if (!found) {
					return unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_value,
							.path = materialize_path(frames),
							.member_name = S{name},
							.message = format("unknown member: {}", name)});
				}
			}
		}
		return result;
	} else {
		static_assert(false, "No JsonCodec<T> or JsonMembers<T> found for T");
	}
}

} // namespace detail
// decode<T> dispatch
export template<class T>
expected<T, JsonError> decode(
	NodeRef root,
	JsonDecodeOptions const &opts) {
	V<detail::PathFrame> frames;
	frames.reserve(16);
	return detail::decode_with_frames<T>(root, frames, opts);
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
	SV name,
	T const &value) {
	if (auto ok = check_can_insert(); !ok) {
		return ok;
	}
	// Spec: duplicate-name rejection happens before dispatching to JsonCodec<T>::encode.
	if (frame_.dup_check.contains(S{name})) {
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::duplicate_member,
				.member_name = S{name},
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
					 using M = std::remove_cvref_t<decltype(value.*m.pointer)>;
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
// ---------------------------------------------------------------------------
// Phase 8.3 — schema_for / validate
// ---------------------------------------------------------------------------

namespace detail {

template<class M>
constexpr SV json_type_name() noexcept {
	using Raw = std::remove_cvref_t<M>;
	if constexpr (same_as<Raw, bool>) {
		return "boolean";
	} else if constexpr (
		same_as<Raw, i64>
		|| same_as<Raw, u64>
		|| same_as<Raw, i32>
		|| same_as<Raw, u32>
		|| same_as<Raw, i16>
		|| same_as<Raw, u16>
		|| same_as<Raw, i8>
		|| same_as<Raw, u8>) {
		return "integer";
	} else if constexpr (same_as<Raw, double> || same_as<Raw, float>) {
		return "number";
	} else if constexpr (same_as<Raw, S> || same_as<Raw, SV>) {
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
void schema_insert_type(
	ObjectBuilder &obj) {
	using Raw = std::remove_cvref_t<M>;
	if constexpr (is_optional<Raw>::value) {
		using Inner = typename Raw::value_type;
		auto _ = obj.insert_string("type", json_type_name<Inner>());
	} else if constexpr (is_nullable_type<Raw>::value) {
		using Inner = nullable_inner_t<Raw>;
		auto _ = obj.insert_string("type", json_type_name<Inner>());
		auto _ = obj.insert_bool("nullable", true);
	} else {
		auto _ = obj.insert_string("type", json_type_name<Raw>());
	}
}

} // namespace detail
export template<class T>
	requires(detail::has_members_spec<T>::value || detail::has_codec_spec<T>::value)
expected<Document, JsonError> schema_for() {
	ValueBuilder vb;
	auto obj_r = vb.begin_object();
	if (!obj_r) {
		return unexpected(move(obj_r).error());
	}
	auto &schema = *obj_r;
	auto _ = schema.insert_string("type", "object");

	if constexpr (detail::has_members_spec<T>::value) {
		auto props_r = schema.insert_object("properties");
		auto &props = *props_r;
		auto const members = JsonMembers<T>::members();

		apply(
			[&](auto const &...ms) {
				(([&](auto const &entry) {
					 auto const &m = detail::jm_member(entry);
					 using M = std::remove_reference_t<decltype(std::declval<T>().*m.pointer)>;
					 auto field_r = props.insert_object(m.name);
					 auto &field = *field_r;
					 detail::schema_insert_type<M>(field);
					 move(field).commit();
				 })(ms),
				 ...);
			},
			members);
		move(props).commit();

		auto req_r = schema.insert_array("required");
		auto &req = *req_r;
		apply(
			[&](auto const &...ms) {
				(([&](auto const &entry) {
					 auto const &m = detail::jm_member(entry);
					 using M = std::remove_reference_t<decltype(std::declval<T>().*m.pointer)>;
					 if constexpr (!detail::is_optional<std::remove_cvref_t<M>>::value) {
						 auto _ = req.append_string(m.name);
					 }
				 })(ms),
				 ...);
			},
			members);
		move(req).commit();
	}

	move(schema).commit();
	return move(vb).finish();
}
export [[nodiscard]] expected<void, JsonError> validate(
	NodeRef root,
	NodeRef schema) {
	auto schema_obj = schema.as_object();
	if (!schema_obj) {
		return {};
	}
	auto type_node = schema_obj->find_member("type");
	if (!type_node) {
		return {};
	}
	auto type_sv = type_node->as_string();
	if (!type_sv) {
		return {};
	}
	SV const expected_type = *type_sv;

	auto kind_matches = [&]() -> bool {
		if (expected_type == "object") {
			return root.kind() == JsonKind::object;
		}
		if (expected_type == "array") {
			return root.kind() == JsonKind::array;
		}
		if (expected_type == "string") {
			return root.kind() == JsonKind::string;
		}
		if (expected_type == "integer") {
			return root.kind() == JsonKind::number;
		}
		if (expected_type == "number") {
			return root.kind() == JsonKind::number;
		}
		if (expected_type == "boolean") {
			return root.kind() == JsonKind::boolean;
		}
		if (expected_type == "any") {
			return true;
		}
		return true;
	};

	if (root.is_null()) {
		auto nullable_node = schema_obj->find_member("nullable");
		if (nullable_node && nullable_node->as_bool().value_or(false)) {
			return {};
		}
	}

	if (!root.is_null() && !kind_matches()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::wrong_kind,
				.message = format(
					"expected type '{}', got '{}'",
					expected_type,
					root.kind() == JsonKind::object  ? "object" :
					root.kind() == JsonKind::array   ? "array" :
					root.kind() == JsonKind::string  ? "string" :
					root.kind() == JsonKind::number  ? "number" :
					root.kind() == JsonKind::boolean ? "boolean" :
					root.kind() == JsonKind::null    ? "null" :
													   "unknown")});
	}

	if (expected_type == "object" && root.kind() == JsonKind::object) {
		auto obj = *root.as_object();
		auto req_node = schema_obj->find_member("required");
		if (req_node) {
			if (auto req_arr = req_node->as_array()) {
				for (NodeRef const el: req_arr->elements()) {
					if (auto name = el.as_string()) {
						if (!obj.find_member(*name)) {
							return unexpected(
								JsonError{
									.stage = JsonStage::decode,
									.code = JsonIssueCode::missing_member,
									.member_name = S{*name},
									.message = format("missing required member: {}", *name)});
						}
					}
				}
			}
		}
		auto props_node = schema_obj->find_member("properties");
		if (props_node) {
			if (auto props_obj = props_node->as_object()) {
				for (auto const &[name, val]: obj.members()) {
					auto field_schema = props_obj->find_member(name);
					if (field_schema) {
						auto r = validate(val, *field_schema);
						if (!r) {
							auto e = move(r).error();
							if (!e.member_name.has_value()) {
								e.member_name = S{name};
							}
							return unexpected(move(e));
						}
					}
				}
			}
		}
	}

	return {};
}

// ---------------------------------------------------------------------------
// JsonWritable concept + make_object / make_array factories (Phase 1.4)
// ---------------------------------------------------------------------------

export template<class T>
concept JsonWritable = same_as<std::remove_cvref_t<T>, bool>
					|| has_json_codec<std::remove_cvref_t<T>>
					|| same_as<std::remove_cvref_t<T>, S>
					|| std::convertible_to<std::remove_cvref_t<T>, SV>
					|| (std::integral<std::remove_cvref_t<T>>
						&& !same_as<std::remove_cvref_t<T>, bool>
						&& !same_as<std::remove_cvref_t<T>, char>
						&& !same_as<std::remove_cvref_t<T>, char8_t>
						&& !same_as<std::remove_cvref_t<T>, signed char>
						&& !same_as<std::remove_cvref_t<T>, unsigned char>
						&& !same_as<std::remove_cvref_t<T>, wchar_t>
						&& !same_as<std::remove_cvref_t<T>, char16_t>
						&& !same_as<std::remove_cvref_t<T>, char32_t>)
					|| std::floating_point<std::remove_cvref_t<T>>;

export template<class P>
concept JsonObjectPair = std::tuple_size<std::remove_cvref_t<P>>::value == 2
					  && std::convertible_to<TEt<0, std::remove_cvref_t<P>>, SV>
					  && JsonWritable<TEt<1, std::remove_cvref_t<P>>>;
// Internal dispatch: encode a JsonWritable value into ObjectBuilder.
namespace detail {

template<class T>
expected<void, JsonError> write_writable(
	ObjectBuilder &obj,
	SV name,
	T const &value) {
	using U = std::remove_cvref_t<T>;
	if constexpr (same_as<U, bool>) {
		return obj.insert_bool(name, value);
	} else if constexpr (has_json_codec<U>) {
		return obj.template insert<U>(name, value);
	} else if constexpr (same_as<U, S>) {
		return obj.insert_string(name, SV{value});
	} else if constexpr (std::convertible_to<U, SV>) {
		SV sv = static_cast<SV>(value);
		if constexpr (same_as<U, char const *> || std::is_pointer_v<U>) {
			if (sv.data() == nullptr) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_value,
						.member_name = S{name},
						.message = format("null pointer for member '{}'", name)});
			}
		}
		return obj.insert_string(name, sv);
	} else if constexpr (std::is_signed_v<U>) {
		if constexpr (sizeof(U) < sizeof(i64)) {
			return obj.insert_i64(name, static_cast<i64>(value));
		} else {
			if (value < static_cast<U>(NL<i64>::min()) || value > static_cast<U>(NL<i64>::max())) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::number_out_of_range,
						.member_name = S{name},
						.message = format("value out of i64 range for member '{}'", name)});
			}
			return obj.insert_i64(name, static_cast<i64>(value));
		}
	} else if constexpr (std::is_unsigned_v<U>) {
		return obj.insert_u64(name, static_cast<u64>(value));
	} else {
		return obj.insert_f64(name, static_cast<double>(value));
	}
}
template<class T>
expected<void, JsonError> write_writable_arr(
	ArrayBuilder &arr,
	T const &value) {
	using U = std::remove_cvref_t<T>;
	if constexpr (same_as<U, bool>) {
		return arr.append_bool(value);
	} else if constexpr (has_json_codec<U>) {
		return arr.template append<U>(value);
	} else if constexpr (same_as<U, S>) {
		return arr.append_string(SV{value});
	} else if constexpr (std::convertible_to<U, SV>) {
		SV sv = static_cast<SV>(value);
		if constexpr (same_as<U, char const *> || std::is_pointer_v<U>) {
			if (sv.data() == nullptr) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::invalid_value,
						.message = "null pointer in array element"});
			}
		}
		return arr.append_string(sv);
	} else if constexpr (std::is_signed_v<U>) {
		if constexpr (sizeof(U) < sizeof(i64)) {
			return arr.append_i64(static_cast<i64>(value));
		} else {
			if (value < static_cast<U>(NL<i64>::min()) || value > static_cast<U>(NL<i64>::max())) {
				return unexpected(
					JsonError{
						.stage = JsonStage::build,
						.code = JsonIssueCode::number_out_of_range,
						.message = "value out of i64 range"});
			}
			return arr.append_i64(static_cast<i64>(value));
		}
	} else if constexpr (std::is_unsigned_v<U>) {
		return arr.append_u64(static_cast<u64>(value));
	} else {
		return arr.append_f64(static_cast<double>(value));
	}
}

} // namespace detail
// Heterogeneous variadic form.
export template<class... Pairs>
	requires(JsonObjectPair<std::remove_cvref_t<Pairs>> && ...)
[[nodiscard]] expected<Document, JsonError> make_object(
	Pairs &&...pairs) {
	ValueBuilder vb;
	auto obj_or = vb.begin_object();
	if (!obj_or) {
		return unexpected(move(obj_or).error());
	}
	auto &obj = *obj_or;
	bool ok = true;
	JsonError first_err;
	(([&](auto &&p) {
		 if (!ok) {
			 return;
		 }
		 SV const key = static_cast<SV>(std::get<0>(p));
		 auto res = detail::write_writable(obj, key, std::get<1>(p));
		 if (!res) {
			 ok = false;
			 first_err = move(res).error();
		 }
	 })(forward<Pairs>(pairs)),
	 ...);
	if (!ok) {
		return unexpected(move(first_err));
	}
	move(obj).commit();
	return move(vb).finish();
}
// Homogeneous initializer_list form.
export template<class V>
	requires JsonWritable<V>
[[nodiscard]] expected<Document, JsonError> make_object(
	std::initializer_list<P<SV, V>> pairs) {
	ValueBuilder vb;
	auto obj_or = vb.begin_object();
	if (!obj_or) {
		return unexpected(move(obj_or).error());
	}
	auto &obj = *obj_or;
	for (auto const &[k, v]: pairs) {
		auto res = detail::write_writable(obj, k, v);
		if (!res) {
			return unexpected(move(res).error());
		}
	}
	move(obj).commit();
	return move(vb).finish();
}
// Heterogeneous variadic array form.
export template<class... Elems>
	requires(JsonWritable<std::remove_cvref_t<Elems>> && ...)
[[nodiscard]] expected<Document, JsonError> make_array(
	Elems &&...elems) {
	ValueBuilder vb;
	auto arr_or = vb.begin_array();
	if (!arr_or) {
		return unexpected(move(arr_or).error());
	}
	auto &arr = *arr_or;
	bool ok = true;
	JsonError first_err;
	(([&](auto &&e) {
		 if (!ok) {
			 return;
		 }
		 auto res = detail::write_writable_arr(arr, forward<decltype(e)>(e));
		 if (!res) {
			 ok = false;
			 first_err = move(res).error();
		 }
	 })(forward<Elems>(elems)),
	 ...);
	if (!ok) {
		return unexpected(move(first_err));
	}
	move(arr).commit();
	return move(vb).finish();
}
// Homogeneous initializer_list array form.
export template<class V>
	requires JsonWritable<V>
[[nodiscard]] expected<Document, JsonError> make_array(
	std::initializer_list<V> elems) {
	ValueBuilder vb;
	auto arr_or = vb.begin_array();
	if (!arr_or) {
		return unexpected(move(arr_or).error());
	}
	auto &arr = *arr_or;
	for (auto const &e: elems) {
		auto res = detail::write_writable_arr(arr, e);
		if (!res) {
			return unexpected(move(res).error());
		}
	}
	move(arr).commit();
	return move(vb).finish();
}

// ─── Phase 3 — SAX / Event Interface ────────────────────────────────────────

export template<class R>
concept HandlerReturn = same_as<R, void> || std::convertible_to<R, expected<void, JsonError>>;

export template<class H>
concept JsonHandler = requires(H &h, SV sv, i64 i, u64 u, double d, bool b) {
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
	expected<void, JsonError> on_null() { return {}; }
	expected<void, JsonError> on_bool(
		bool) {
		return {};
	}
	expected<void, JsonError> on_string(
		SV) {
		return {};
	}
	expected<void, JsonError> on_i64(
		i64) {
		return {};
	}
	expected<void, JsonError> on_u64(
		u64) {
		return {};
	}
	expected<void, JsonError> on_double(
		double) {
		return {};
	}
	expected<void, JsonError> on_begin_object() { return {}; }
	expected<void, JsonError> on_key(
		SV) {
		return {};
	}
	expected<void, JsonError> on_end_object() { return {}; }
	expected<void, JsonError> on_begin_array() { return {}; }
	expected<void, JsonError> on_end_array() { return {}; }
	// on_number_raw intentionally absent
};
namespace detail {

// Invoke callable, normalize return to expected<void,JsonError>.
// Avoids passing void as a function argument.
template<class F>
[[nodiscard]] inline expected<void, JsonError> invoke_handler(
	F &&f) {
	using R = decltype(std::forward<F>(f)());
	if constexpr (same_as<R, void>) {
		forward<F>(f)();
		return {};
	} else {
		expected<void, JsonError> e = forward<F>(f)();
		return e;
	}
}
// Dispatch a number event to the handler.
// If H provides on_number_raw: call it only (raw bytes, no typed conversion).
// Otherwise dispatch on_i64 / on_u64 / on_double based on value kind.
template<JsonHandler H>
[[nodiscard]] expected<void, JsonError> dispatch_number(
	H &h,
	JsonNumberView nv) {
	if constexpr (requires { h.on_number_raw(SV{}); }) {
		static_assert(
			HandlerReturn<decltype(h.on_number_raw(SV{}))>,
			"on_number_raw must return void or expected<void,JsonError>");
		return invoke_handler([&] { return h.on_number_raw(nv.lexeme()); });
	} else {
		if (nv.form() == JsonNumberForm::non_integer) {
			auto d = nv.to_f64();
			if (!d) {
				return unexpected(move(d).error());
			}
			return invoke_handler([&] { return h.on_double(*d); });
		}
		// Integer form: try i64 → u64 → f64 (huge integer).
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
			return unexpected(move(dv).error());
		}
		return invoke_handler([&] { return h.on_double(*dv); });
	}
}
// Decode a JsonStringToken to string_view or S, call cb(SV).
template<class Cb>
[[nodiscard]] expected<void, JsonError> dispatch_string_cb(
	JsonStringToken const &tok,
	Cb &&cb) {
	if (auto borrow = tok.unescaped_borrow()) {
		return forward<Cb>(cb)(*borrow);
	}
	S buf;
	buf.reserve(tok.max_decoded_size());
	auto r = tok.append_decoded_to(buf);
	if (!r) {
		return unexpected(move(r).error());
	}
	return forward<Cb>(cb)(SV{buf});
}

} // namespace detail
export template<JsonHandler H>
[[nodiscard]] expected<void, JsonError> parse_sax(
	SV input,
	H &handler,
	JsonParseOptions const &opts = {}) {
	using Ev = JsonReader::Event;
	JsonReader reader{input, opts};

	for (;;) {
		auto ev_or = reader.next();
		if (!ev_or) {
			return unexpected(move(ev_or).error());
		}
		if (!*ev_or) {
			break; // EOF
		}

		Ev ev = **ev_or;
		expected<void, JsonError> res{};

		switch (ev) {
		case Ev::begin_object: res = detail::invoke_handler([&] { return handler.on_begin_object(); }); break;
		case Ev::end_object  : res = detail::invoke_handler([&] { return handler.on_end_object(); }); break;
		case Ev::begin_array : res = detail::invoke_handler([&] { return handler.on_begin_array(); }); break;
		case Ev::end_array   : res = detail::invoke_handler([&] { return handler.on_end_array(); }); break;
		case Ev::key:
			res = detail::dispatch_string_cb(reader.key_token(), [&](SV sv) {
				return detail::invoke_handler([&] { return handler.on_key(sv); });
			});
			break;
		case Ev::string_value:
			res = detail::dispatch_string_cb(reader.string_token(), [&](SV sv) {
				return detail::invoke_handler([&] { return handler.on_string(sv); });
			});
			break;
		case Ev::number_value: res = detail::dispatch_number(handler, reader.number_val()); break;
		case Ev::bool_value  : res = detail::invoke_handler([&] { return handler.on_bool(reader.bool_val()); }); break;
		case Ev::null_value  : res = detail::invoke_handler([&] { return handler.on_null(); }); break;
		}

		if (!res) {
			return unexpected(move(res).error());
		}
	}
	return {};
}
// ─── Phase 7 — Streaming & NDJSON ───────────────────────────────────────────

export class NdjsonRange {
	SV input_;
	JsonParseOptions opts_;

public:
	explicit NdjsonRange(
		SV input,
		JsonParseOptions const &opts = {}) noexcept
		: input_{input}
		, opts_{opts} {}
	struct Iterator {
		using iterator_category = std::input_iterator_tag;
		using value_type = expected<Document, JsonError>;
		using difference_type = std::ptrdiff_t;
		using pointer = value_type const *;
		using reference = value_type const &;

	private:
		SV remaining_;
		JsonParseOptions opts_;
		Opt<value_type> cache_;
		void advance_one() noexcept {
			cache_.reset();
			while (!remaining_.empty()) {
				auto pos = remaining_.find('\n');
				SV line;
				if (pos == SV::npos) {
					line = remaining_;
					remaining_ = {};
				} else {
					line = remaining_.substr(0, pos);
					remaining_.remove_prefix(pos + 1);
				}
				// strip trailing CR
				if (!line.empty() && line.back() == '\r') {
					line.remove_suffix(1);
				}
				if (line.empty()) {
					continue;
				}
				cache_ = conflux::json::parse_borrowed_unsafe(line, opts_);
				return;
			}
		}

		friend class NdjsonRange;
		Iterator(
			SV remaining,
			JsonParseOptions const &opts) noexcept
			: remaining_{remaining}
			, opts_{opts} {
			advance_one();
		}

	public:
		[[nodiscard]] reference operator *() const noexcept { return *cache_; }
		[[nodiscard]] pointer operator ->() const noexcept { return &*cache_; }
		Iterator &operator ++() noexcept {
			advance_one();
			return *this;
		}
		void operator ++(
			int) noexcept {
			++*this;
		}
		[[nodiscard]] bool operator ==(
			std::default_sentinel_t) const noexcept {
			return !cache_.has_value();
		}
	};
	[[nodiscard]] Iterator begin() const noexcept { return {input_, opts_}; }
	[[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};
export class JsonAccumulator {
	S buf_;
	JsonParseOptions opts_;

public:
	explicit JsonAccumulator(
		JsonParseOptions const &opts = {}) noexcept
		: opts_{opts} {}
	[[nodiscard]] expected<void, JsonError> feed(
		SV chunk) {
		constexpr SZ kU32Ceiling = (SZ{1} << 32) - 1;
		SZ const hard_cap = kU32Ceiling - 1;
		SZ const configured_cap = opts_.max_input_size.is_unlimited() ?
									  hard_cap :
									  min(opts_.max_input_size.explicit_value().value_or(kDefaultMaxInput), hard_cap);
		if (buf_.size() > configured_cap || chunk.size() > configured_cap - buf_.size()) {
			return unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::input_too_large,
					.message = "accumulated size exceeds max_input_size"});
		}
		buf_.append(chunk);
		return {};
	}
	[[nodiscard]] expected<Document, JsonError> finish() { return conflux::json::parse(move(buf_), opts_); }
	void reset() noexcept { buf_.clear(); }
	[[nodiscard]] SZ buffered_bytes() const noexcept { return buf_.size(); }
};
// (reflect codec moved to src/json_reflect.cxx — separate module conflux.json.reflect)
