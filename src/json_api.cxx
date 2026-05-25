module;
#include <cassert>
#include <cstdint>
// stdlib.h and sys/random.h pull in pthreadtypes.h which conflicts with the
// std module BMI under GCC -freflection; forward-declare what we need instead
extern "C" {
struct __locale_struct;
using locale_t = __locale_struct *;
locale_t newlocale(int, char const *, locale_t) noexcept;
double strtod_l(char const *, char **, locale_t) noexcept;
long getrandom(void *, unsigned long, unsigned int);
}
#if defined(CONFLUX_JSON_HASH_PROVIDER_XXHASH)
	#include <xxhash.h>
#endif
// <immintrin.h> → mm_malloc.h → stdlib.h → pthreadtypes.h conflicts with the
// std module BMI under GCC -freflection; scalar fallback used in that build.
#include "json_simd_backend.hxx"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
export module conflux.json:api;
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
export class JsonStreamReader;
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
	json_patch,
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
	invalid_patch,
	patch_op_missing,
	patch_op_unknown,
	patch_path_missing,
	patch_path_invalid,
	patch_from_missing,
	patch_from_invalid,
	patch_test_failed,
	patch_target_missing,
	patch_parent_missing,
	patch_array_index_invalid,
	patch_array_index_out_of_range,
	patch_move_into_child,
	patch_remove_document_root,
	patch_too_many_operations,
	patch_pointer_too_deep,
};
export struct JsonSourceLocation {
	std::size_t offset{};
	std::size_t line{1};
	std::size_t column{1};
};

// NOLINTNEXTLINE(performance-enum-size)
export enum class DuplicateKeyPolicy : std::uint8_t {
	reject, // RFC 8259 recommended; current default
	last_wins, // keep last value; first occurrence's name position preserved
	first_wins, // keep first value; duplicate parsed for syntax, then discarded
};
// ---------------------------------------------------------------------------
// JsonPath
// ---------------------------------------------------------------------------

export struct JsonPathMember {
	std::string name;
};
export struct JsonPathIndex {
	std::size_t index{};
};
export using JsonPathSegment = std::variant<JsonPathMember, JsonPathIndex>;
export class JsonPath {
	std::vector<JsonPathSegment> segs_;

public:
	static JsonPath root() { return {}; }
	JsonPath() = default;
	JsonPath(JsonPath const &) = default;
	JsonPath(JsonPath &&) noexcept = default;
	JsonPath &operator =(JsonPath const &) = default;
	JsonPath &operator =(JsonPath &&) noexcept = default;
	[[nodiscard]] bool empty() const noexcept { return segs_.empty(); }
	[[nodiscard]] std::size_t size() const noexcept { return segs_.size(); }
	void reserve(
		std::size_t n) {
		segs_.reserve(n);
	}
	void push_member(
		std::string_view name) {
		segs_.emplace_back(JsonPathMember{std::string{name}});
	}
	void push_index(
		std::size_t idx) {
		segs_.emplace_back(JsonPathIndex{idx});
	}
	void pop() noexcept {
		if (!segs_.empty()) {
			segs_.pop_back();
		}
	}
	[[nodiscard]] JsonPathSegment const &segment(
		std::size_t i) const {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		return segs_[i];
	}
	[[nodiscard]] auto begin() const noexcept { return segs_.begin(); }
	[[nodiscard]] auto end() const noexcept { return segs_.end(); }
	[[nodiscard]] std::string to_pointer() const;
	static std::expected<JsonPath, JsonError> from_pointer(std::string_view sv);

	bool friend operator ==(JsonPath const &, JsonPath const &) = default;
};
template<>
struct std::hash<JsonPath> {
	std::size_t operator ()(
		JsonPath const &p) const noexcept {
		std::size_t h = 0;
		for (auto const &seg: p) {
			std::size_t const sh = holds_alternative<JsonPathMember>(seg) ?
									   std::hash<std::string>{}(get<JsonPathMember>(seg).name) :
									   std::hash<std::size_t>{}(get<JsonPathIndex>(seg).index);
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
	std::optional<JsonSourceLocation> source{};
	std::optional<JsonKind> expected_kind{};
	std::optional<JsonKind> actual_kind{};
	std::optional<std::string> member_name{};
	std::optional<std::string> target_type{};
	std::optional<std::size_t> requested_index{};
	std::optional<std::size_t> container_size{};
	std::optional<std::size_t> operation_index{};
	std::optional<std::string> operation{};
	std::optional<std::string> pointer{};
	std::optional<std::string> from_pointer{};
	std::string message{};
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
		copy.path = std::move(full);
		return copy;
	}
	[[nodiscard]] JsonError with_prefix(
		JsonPath const &prefix) && {
		return static_cast<JsonError const &>(*this).with_prefix(prefix);
	}
};

// ---------------------------------------------------------------------------
// LimitOption / JsonParseOptions
// ---------------------------------------------------------------------------

export struct NoLimit {};
export inline constexpr NoLimit no_limit{};
export class LimitOption {
	enum class Tag : std::uint8_t {
		default_,
		unlimited,
		bound,
	};
	Tag tag_{Tag::default_};
	std::size_t value_{};

public:
	constexpr LimitOption() noexcept = default;
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr LimitOption(
		NoLimit) noexcept
		: tag_{Tag::unlimited} {}
	constexpr explicit LimitOption(
		std::size_t v) noexcept
		: tag_{Tag::bound}
		, value_{v} {}
	[[nodiscard]] static constexpr LimitOption bound(
		std::size_t v) noexcept {
		return LimitOption{v};
	}
	[[nodiscard]] constexpr bool is_default() const noexcept { return tag_ == Tag::default_; }
	[[nodiscard]] constexpr bool is_unlimited() const noexcept { return tag_ == Tag::unlimited; }
	[[nodiscard]] constexpr std::optional<std::size_t> explicit_value() const noexcept {
		if (tag_ == Tag::bound) {
			return value_;
		}
		return std::nullopt;
	}
	[[nodiscard]] constexpr bool exceeds(
		std::size_t n,
		std::size_t default_cap) const noexcept {
		if (tag_ == Tag::unlimited) {
			return false;
		}
		if (tag_ == Tag::default_) {
			return n > default_cap;
		}
		return n > value_;
	}
};

export enum class ParseMode : std::uint8_t {
	strict,
	json5,
};
export struct JsonParseOptions {
	LimitOption max_depth;
	LimitOption max_input_size;
	LimitOption max_string_size;
	DuplicateKeyPolicy duplicate_key{DuplicateKeyPolicy::reject};
	std::optional<std::uint32_t> warm_threshold{};
	ParseMode mode{ParseMode::strict};
};

export struct JsonParseStorageStats {
	std::size_t input_bytes{};
	std::size_t nodes_size{};
	std::size_t nodes_capacity{};
	std::size_t array_children_size{};
	std::size_t array_children_capacity{};
	std::size_t object_members_size{};
	std::size_t object_members_capacity{};
	std::size_t string_arena_size{};
	std::size_t string_arena_capacity{};
	std::size_t string_arena_reserve_bytes{};
	std::size_t duplicate_hash_promotions{};
	std::size_t duplicate_hash_inserts{};
	std::size_t duplicate_member_hits{};
	std::size_t first_wins_rollbacks{};
	std::size_t last_wins_updates{};
};

// NOLINTNEXTLINE(performance-enum-size)
export enum class UnknownMemberPolicy : std::uint8_t {
	reject,
	ignore,
};
export struct JsonDecodeOptions {
	UnknownMemberPolicy unknown_members{UnknownMemberPolicy::reject};
};

export struct JsonDecodeScratch {
	std::pmr::memory_resource *resource{std::pmr::get_default_resource()};
	std::array<char, 256> key_inline{};
	std::pmr::vector<char> key_overflow{resource};
	std::array<char, 256> string_inline{};
	std::pmr::vector<char> string_overflow{resource};
	std::pmr::vector<std::uint64_t> found_bits{resource};

	void reset_resource(
		std::pmr::memory_resource *mr = std::pmr::get_default_resource()) {
		resource = mr != nullptr ? mr : std::pmr::get_default_resource();
		std::destroy_at(std::addressof(key_overflow));
		std::construct_at(std::addressof(key_overflow), resource);
		std::destroy_at(std::addressof(string_overflow));
		std::construct_at(std::addressof(string_overflow), resource);
		std::destroy_at(std::addressof(found_bits));
		std::construct_at(std::addressof(found_bits), resource);
	}
};

// The parser/DOM prototype policy makes the intended replacement architecture
// explicit without starting a broad parser rewrite. The current implementation
// below already provides the three storage routes we want to preserve: borrowed
// view documents, PMR-backed owned/caller-resource documents, and reusable arena
// documents. Future tokenizer/DOM work should keep these policy names stable and
// replace implementations behind them.
export enum class JsonDomInputOwnership : std::uint8_t {
	borrowed_view,
	owned_copy,
	owned_move,
};

export enum class JsonDomStorageModel : std::uint8_t {
	standalone_document,
	caller_pmr_document,
	reusable_arena,
};

export enum class JsonDomStringModel : std::uint8_t {
	view_unescaped_copy_decoded,
};

export enum class JsonDomNumberModel : std::uint8_t {
	preserve_lexeme_parse_on_access,
};

export enum class JsonDomUtf8Model : std::uint8_t {
	strict_validate_on_parse,
};

export enum class JsonDomErrorModel : std::uint8_t {
	expected_json_error,
};

export enum class JsonDomObjectIndexModel : std::uint8_t {
	preserve_order_warm_hash_on_demand,
};

export struct JsonDomPolicy {
	JsonDomInputOwnership input{JsonDomInputOwnership::borrowed_view};
	JsonDomStorageModel storage{JsonDomStorageModel::standalone_document};
	JsonDomStringModel strings{JsonDomStringModel::view_unescaped_copy_decoded};
	JsonDomNumberModel numbers{JsonDomNumberModel::preserve_lexeme_parse_on_access};
	JsonDomUtf8Model utf8{JsonDomUtf8Model::strict_validate_on_parse};
	JsonDomErrorModel errors{JsonDomErrorModel::expected_json_error};
	JsonDomObjectIndexModel object_index{JsonDomObjectIndexModel::preserve_order_warm_hash_on_demand};
	JsonParseOptions parse{};

	[[nodiscard]] static constexpr JsonDomPolicy view_first(
		JsonParseOptions parse_opts = {}) noexcept {
		return JsonDomPolicy{
			.input = JsonDomInputOwnership::borrowed_view,
			.storage = JsonDomStorageModel::standalone_document,
			.parse = parse_opts};
	}

	[[nodiscard]] static constexpr JsonDomPolicy owning_document(
		JsonParseOptions parse_opts = {}) noexcept {
		return JsonDomPolicy{
			.input = JsonDomInputOwnership::owned_copy,
			.storage = JsonDomStorageModel::standalone_document,
			.parse = parse_opts};
	}

	[[nodiscard]] static constexpr JsonDomPolicy caller_pmr(
		JsonParseOptions parse_opts = {}) noexcept {
		return JsonDomPolicy{
			.input = JsonDomInputOwnership::owned_copy,
			.storage = JsonDomStorageModel::caller_pmr_document,
			.parse = parse_opts};
	}

	[[nodiscard]] static constexpr JsonDomPolicy arena_reuse(
		JsonParseOptions parse_opts = {}) noexcept {
		return JsonDomPolicy{
			.input = JsonDomInputOwnership::owned_copy,
			.storage = JsonDomStorageModel::reusable_arena,
			.parse = parse_opts};
	}

	[[nodiscard]] static constexpr JsonDomPolicy arena_borrowed(
		JsonParseOptions parse_opts = {}) noexcept {
		return JsonDomPolicy{
			.input = JsonDomInputOwnership::borrowed_view,
			.storage = JsonDomStorageModel::reusable_arena,
			.parse = parse_opts};
	}
};

export struct JsonByteRange {
	std::size_t start;
	std::size_t end;
};

// ---------------------------------------------------------------------------
// Internal storage
// ---------------------------------------------------------------------------

enum class NodeKind : std::uint8_t {
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

constexpr std::uint8_t kStorageInputView = 0x01; // off,len index into input_view
constexpr std::uint8_t kRawJsonSlice = 0x02; // bytes are raw JSON content (dump-safe memcpy)
constexpr std::uint8_t kValueExternalView =
	0x80; // off indexes external_ptrs_, value is caller-owned (string nodes only)

// Number value-kind flags (at most one of kValKind* set on a number node).
constexpr std::uint8_t kLexIntForm = 0x08; // lexeme matches -?(0|[1-9][0-9]*)
constexpr std::uint8_t kValKindInt = 0x10; // ival valid
constexpr std::uint8_t kValKindUint = 0x20; // uval valid
constexpr std::uint8_t kValKindF64 = 0x40; // dval valid
constexpr std::uint8_t kValKindDeferred = 0x04; // range-error f64 ≤ 4 KiB; std::from_chars deferred to to_f64()

// All three kValKind* clear on a number node = f64-overflow (lexeme preserved).

// kMemberExternalView: name is caller-owned. name_off indexes DocumentStorage::external_ptrs_.
constexpr std::uint32_t kMemberExternalView = 0x04u;
struct MemberEntry {
	std::uint32_t name_off; // arena offset; or external_ptrs_ index when kMemberExternalView
	std::uint32_t name_len;
	std::uint32_t val_node;
	std::uint32_t name_flags; // 0=arena; kStorageInputView=0x01; kMemberExternalView=0x04
};
static_assert(sizeof(MemberEntry) == 16);
static_assert(std::is_trivially_copyable_v<MemberEntry>);

// ---------------------------------------------------------------------------
// Phase 6 — ObjHashTable (v14 HHH–JJJ, v15 RRR–SSS)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kEmptySlot = ~std::uint32_t{};
struct ObjHashSlot {
	std::uint32_t member_index{kEmptySlot};
	std::uint32_t name_hash{0};
};
static_assert(sizeof(ObjHashSlot) == 8);
struct ObjHashTable {
	std::uint32_t capacity;
	std::uint32_t member_count;
	std::uint64_t hash_seed;
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
		std::uint32_t capacity,
		std::uint32_t member_count,
		std::uint64_t hash_seed,
		std::pmr::memory_resource *mr = std::pmr::new_delete_resource()) noexcept;
	static void destroy(ObjHashTable *t) noexcept;
};
constexpr std::uint32_t kHashThreshold = 32;
constexpr std::uint32_t kProbeChainMax = 64;
constexpr std::uint32_t kMaxHashTableCapacity = 1u << 30;
// FI-7 — practical std::byte budget on the per-object std::hash index to bound
// DoS payloads. 256 MiB / 8 B per slot = 32 Mi slots; well above any
// realistic object size.
constexpr std::size_t kMaxHashIndexBytes = 256ULL * 1024 * 1024;
// FI-1 — sentinel value stashed into hash_idx_raw when a previous build
// attempt failed (probe-cap reached or budget exceeded). Subsequent
// find_member calls observe the sentinel and short-circuit straight to
// the linear scan, avoiding repeated build attempts. The Document
// destructor and published-table reads must treat this sentinel as
// "no table" (i.e. neither dereference nor delete it).
inline ObjHashTable *const kHashBuildFailedSentinel = reinterpret_cast<ObjHashTable *>(static_cast<uintptr_t>(1));
// ---------------------------------------------------------------------------
// Phase 0 (v11) — Node, 24 B, std::uint32_t offsets, union payload.
//
// The 8-std::byte union's active member is determined by (kind, flags):
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
	std::uint8_t flags;
	std::uint16_t _pad0;
	std::uint32_t off;
	std::uint32_t len;
	std::uint32_t _pad1;
	union {
		bool bool_val;
		std::int64_t ival;
		std::uint64_t uval;
		double dval;
		ObjHashTable *hash_idx_raw;
		std::uint64_t _raw;
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
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t flags) noexcept {
	return Node{.kind = NodeKind::string_, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}
[[nodiscard]] inline Node node_array(
	std::uint32_t off,
	std::uint32_t len) noexcept {
	return Node{.kind = NodeKind::array_, .flags = 0, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}
[[nodiscard]] inline Node node_object(
	std::uint32_t off,
	std::uint32_t len) noexcept {
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
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags,
	std::int64_t v) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<std::uint8_t>(storage_flags | kLexIntForm | kValKindInt),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		.ival = v};
}
[[nodiscard]] inline Node make_number_uint(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags,
	std::uint64_t v) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<std::uint8_t>(storage_flags | kLexIntForm | kValKindUint),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		.uval = v};
}
[[nodiscard]] inline Node make_number_f64(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags,
	double v,
	bool int_form) noexcept {
	std::uint8_t const flags = static_cast<std::uint8_t>(storage_flags | (int_form ? kLexIntForm : 0) | kValKindF64);
	return Node{.kind = NodeKind::number, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, .dval = v};
}
[[nodiscard]] inline Node make_number_overflow(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags,
	bool int_form) noexcept {
	std::uint8_t const flags = static_cast<std::uint8_t>(storage_flags | (int_form ? kLexIntForm : 0));
	return Node{.kind = NodeKind::number, .flags = flags, ._pad0 = 0, .off = off, .len = len, ._pad1 = 0, ._raw = 0};
}
[[nodiscard]] inline Node make_number_deferred(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags) noexcept {
	return Node{
		.kind = NodeKind::number,
		.flags = static_cast<std::uint8_t>(storage_flags | kValKindDeferred),
		._pad0 = 0,
		.off = off,
		.len = len,
		._pad1 = 0,
		._raw = 0};
}
[[nodiscard]] inline std::uint64_t make_hash_seed() noexcept {
	std::uint64_t seed{};
	if (::getrandom(&seed, sizeof(seed), 0) != static_cast<long>(sizeof(seed))) {
		seed = reinterpret_cast<uintptr_t>(&seed) ^ UINT64_C(0x517cc1b727220a95);
	}
	return seed;
}

} // namespace detail
struct DocumentStorage {
	std::pmr::vector<Node> nodes;
	std::pmr::string string_arena;
	std::pmr::vector<std::uint32_t> array_children;
	std::pmr::vector<MemberEntry> object_members;
	std::vector<char const *> external_ptrs_; // indexed by MemberEntry::name_off when kMemberExternalView
	std::string owned_input;
	std::string_view input_view;
	std::uint32_t root_node{0};
	std::uint32_t bom_prefix_bytes{0};
	std::pmr::memory_resource *hash_mr_{std::pmr::new_delete_resource()};
	JsonParseStorageStats parse_stats{};
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
	DocumentStorage(
		std::pmr::memory_resource *storage_resource,
		std::pmr::memory_resource *hash_resource)
		: nodes(storage_resource)
		, string_arena(storage_resource)
		, array_children(storage_resource)
		, object_members(storage_resource)
		, hash_mr_(hash_resource) {}
	DocumentStorage(DocumentStorage const &) = delete;
	DocumentStorage &operator =(DocumentStorage const &) = delete;
	DocumentStorage(DocumentStorage &&) noexcept = default;
	DocumentStorage &operator =(DocumentStorage &&) noexcept = default;
	~DocumentStorage() noexcept;
	void reset() noexcept;
	[[nodiscard]] std::string_view str_at(std::uint32_t off, std::uint32_t len) const noexcept;
	[[nodiscard]] std::string_view bytes_at(std::uint32_t off, std::uint32_t len, std::uint8_t flags) const noexcept;
	[[nodiscard]] std::string_view member_name(MemberEntry const &m) const noexcept;
	[[nodiscard]] JsonParseStorageStats storage_stats() const noexcept;
};
// ---------------------------------------------------------------------------
// Phase 0 — slow-path f64 classifier (v14 AAA–EEE, v15 QQQ)
// ---------------------------------------------------------------------------

// v15 TTT: lexemes longer than this are conservatively classified as
// overflow_infinite → number_out_of_range. v7 would have returned 0.0 for
// pathological underflow tokens like "0." + 4000+ zeros + "1". DoS hardening
// against megabyte-long number tokens.
constexpr std::size_t kSlowFloatLexemeCopyLimit = 4096;
constexpr std::size_t kMaxNumberLexemeLen = 1024;
constexpr std::size_t kDefaultMaxDepth = 128;
constexpr std::size_t kDefaultMaxInput = 128ULL * 1024 * 1024;
constexpr std::size_t kDefaultMaxString = 64ULL * 1024 * 1024;
namespace detail {

constexpr int kLcAllMask = 8127;

struct CLocaleHolder {
	::locale_t loc;
	bool ok;
};
[[nodiscard]] inline CLocaleHolder const &c_locale_holder() noexcept {
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	static CLocaleHolder const h = [] {
		::locale_t l = ::newlocale(kLcAllMask, "C", nullptr);
		return CLocaleHolder{l, l != nullptr};
	}();
	// Intentional: process-lifetime singleton, never freelocale'd.
	// This is the only permitted hidden process-lifetime state in conflux.json;
	// see docs/json-design.md.
	return h;
}
struct ClassifiedDouble {
	enum class Kind : std::uint8_t {
		underflow_finite,
		overflow_infinite,
	} kind;
	double value;
};
[[nodiscard]] inline std::expected<ClassifiedDouble, JsonError> classify_range_error_slow(
	char const *first,
	char const *last) noexcept {
	auto const n = static_cast<std::size_t>(last - first);
	if (n > kSlowFloatLexemeCopyLimit) {
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}
	double dv{};
	auto const [p, ec] = std::from_chars(first, last, dv, std::chars_format::general);
	if (ec == std::errc{} && p == last) {
		if (std::isfinite(dv)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::underflow_finite, dv};
		}
		return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
	}
	if (ec == std::errc::result_out_of_range) {
		// libc++ sets dv=inf for overflow; libstdc++ sets dv=0 for both cases.
		// When std::from_chars is informative (std::isinf), use it directly.
		if (std::isinf(dv)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
		}

		auto const &lh = c_locale_holder();
		if (!lh.ok) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::resource_exhausted,
					.message = "newlocale(C) failed at startup; strtod_l unavailable"});
		}
		std::array<char, 128> stack_buf{};
		std::unique_ptr<char[]> heap_buf;
		char *cp = nullptr;
		if (n + 1 <= stack_buf.size()) {
			cp = stack_buf.data();
		} else {
			heap_buf = std::unique_ptr<char[]>{new (std::nothrow) char[n + 1]};
			if (!heap_buf) {
				return std::unexpected(
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
			return std::unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::invalid_number,
					.message = "strtod_l rejected deferred lexeme"});
		}
		if (std::isinf(v)) {
			return ClassifiedDouble{ClassifiedDouble::Kind::overflow_infinite, 0.0};
		}
		return ClassifiedDouble{ClassifiedDouble::Kind::underflow_finite, v};
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::parse,
			.code = JsonIssueCode::invalid_number,
			.message = std::format("std::from_chars rejected deferred lexeme: {}", std::string_view{first, last})});
}
// Pre-parsed number factory: takes a syntactically valid JSON number lexeme
// (caller validates) plus its arena offset/length, runs the two-stage parse
// (std::int64_t → std::uint64_t → f64 with std::from_chars fallback for range-error f64), and returns
// the appropriate Node.
[[nodiscard]] inline std::expected<Node, JsonError> build_number_node_from_lexeme(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t storage_flags,
	std::string_view lex) noexcept {
	bool const int_form = lex.find_first_of(".eE") == std::string_view::npos;
	bool const neg = !lex.empty() && lex.front() == '-';
	auto const *b = lex.data();
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	auto const *e = lex.data() + lex.size();

	if (int_form) {
		std::int64_t iv{};
		if (auto [p, ec] = std::from_chars(b, e, iv); ec == std::errc{} && p == e) {
			return make_number_int(off, len, storage_flags, iv);
		}
		if (!neg) {
			std::uint64_t uv{};
			if (auto [p2, ec2] = std::from_chars(b, e, uv); ec2 == std::errc{} && p2 == e) {
				return make_number_uint(off, len, storage_flags, uv);
			}
		}
	}

	double dv{};
	auto const [p, ec] = std::from_chars(b, e, dv, std::chars_format::general);
	if (ec == std::errc{} && p == e) {
		if (std::isfinite(dv)) {
			return make_number_f64(off, len, storage_flags, dv, int_form);
		}
		return make_number_overflow(off, len, storage_flags, int_form);
	}
	if (ec == std::errc::result_out_of_range) {
		if (static_cast<std::size_t>(e - b) > kSlowFloatLexemeCopyLimit) {
			return make_number_overflow(off, len, storage_flags, int_form);
		}
		return make_number_deferred(off, len, storage_flags);
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::parse,
			.code = JsonIssueCode::invalid_number,
			.message = std::format("number rejected by std::from_chars: {}", lex)});
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
	std::string_view lexeme_;
	std::uint64_t raw_payload_; // bit-cast<i64/u64/double> selected by flags_
	std::uint8_t flags_; // kLexIntForm | kValKindInt|Uint|F64

	friend class NodeRef;
	friend class JsonReader;
	bool friend is_value_equal(NodeRef, NodeRef);
	JsonNumberView(
		std::string_view lex,
		std::uint8_t flags,
		std::uint64_t raw) noexcept
		: lexeme_{lex}
		, raw_payload_{raw}
		, flags_{flags} {}

public:
	[[nodiscard]] std::string_view lexeme() const noexcept { return lexeme_; }
	[[nodiscard]] JsonNumberForm form() const noexcept {
		return (flags_ & kLexIntForm) != 0 ? JsonNumberForm::integer : JsonNumberForm::non_integer;
	}
	[[nodiscard]] std::expected<std::int64_t, JsonError> to_i64() const;
	[[nodiscard]] std::expected<std::uint64_t, JsonError> to_u64() const;
	[[nodiscard]] std::expected<double, JsonError> to_f64() const;
	template<class T>
		requires((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>)
	[[nodiscard]] std::expected<T, JsonError> get_as() const {
		if constexpr (std::floating_point<T>) {
			return to_f64().transform([](double v) noexcept { return static_cast<T>(v); });
		} else if constexpr (std::is_signed_v<T>) {
			auto v = to_i64();
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			if constexpr (sizeof(T) < sizeof(std::int64_t)) {
				if (*v < static_cast<std::int64_t>(std::numeric_limits<T>::min())
					|| *v > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
					return std::unexpected(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::number_out_of_range,
							.message = std::format("value {} out of range for target type", lexeme_)});
				}
			}
			return static_cast<T>(*v);
		} else {
			auto v = to_u64();
			if (!v) {
				return std::unexpected(std::move(v).error());
			}
			if constexpr (sizeof(T) < sizeof(std::uint64_t)) {
				if (*v > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
					return std::unexpected(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::number_out_of_range,
							.message = std::format("value {} out of range for target type", lexeme_)});
				}
			}
			return static_cast<T>(*v);
		}
	}
};

[[nodiscard]] bool validate_number_lexeme(std::string_view lex) noexcept;
// Forward declarations for parser helpers used by JsonStringToken/JsonReader.
std::size_t utf8_seq_len(unsigned char lead) noexcept;
bool is_cont(unsigned char c) noexcept;
namespace detail::simd {

[[nodiscard]] std::size_t scan_str_until_special(char const *p, std::size_t n) noexcept;

} // namespace detail::simd
// ---------------------------------------------------------------------------
// decode_str_body helper (used by JsonStringToken)
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline std::optional<std::uint32_t> hex4_from_sv(
	std::string_view body,
	std::size_t pos) noexcept {
	std::uint32_t out = 0;
	for (std::size_t i = 0; i < 4; ++i) {
		char const c = body[pos + i];
		std::uint32_t d;
		constexpr std::uint32_t kA = 10;
		if (c >= '0' && c <= '9') {
			d = static_cast<std::uint32_t>(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			d = static_cast<std::uint32_t>(c - 'a') + kA;
		} else if (c >= 'A' && c <= 'F') {
			d = static_cast<std::uint32_t>(c - 'A') + kA;
		} else {
			return std::nullopt;
		}
		out = (out << 4U) | d;
	}
	return out;
}
inline void append_utf8_to_sv(
	std::uint32_t cp,
	auto &&writer) {
	// NOLINTBEGIN(readability-magic-numbers)
	char buf[4];
	std::size_t len = 0;
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
	// NOLINTEND(readability-magic-numbers)
	writer(std::string_view{buf, len});
}
template<class Writer>
[[nodiscard]] std::expected<std::size_t, JsonError> decode_str_body(
	std::string_view body,
	Writer &&writer,
	LimitOption max_sz)
	noexcept(
		false) {
	std::size_t total = 0;
	std::size_t i = 0;
	while (i < body.size()) {
		auto const c = static_cast<unsigned char>(body[i]);
		if (c != '\\') {
			std::size_t const run_start = i;
			while (i < body.size() && static_cast<unsigned char>(body[i]) != '\\') {
				++i;
			}
			std::string_view chunk = body.substr(run_start, i - run_start);
			writer(chunk);
			total += chunk.size();
			if (max_sz.exceeds(total, kDefaultMaxString)) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::string_too_large,
						.message = "decoded string exceeds max_string_size"});
			}
			continue;
		}
		++i; // skip '\\'
		if (i >= body.size()) {
			return std::unexpected(
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
					return std::unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_unicode_escape,
							.message = "invalid \\uXXXX"});
				}
				auto cp_opt = hex4_from_sv(body, i);
				if (!cp_opt) {
					return std::unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::invalid_unicode_escape,
							.message = "invalid hex digit in \\uXXXX"});
				}
				std::uint32_t cp = *cp_opt;
				i += 4;
				// NOLINTBEGIN(readability-magic-numbers)
				if (cp >= 0xD800U && cp <= 0xDBFFU) {
					if (i + 6 > body.size() || body[i] != '\\' || body[i + 1] != 'u') {
						return std::unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "unpaired high surrogate"});
					}
					i += 2;
					auto lo_opt = hex4_from_sv(body, i);
					if (!lo_opt) {
						return std::unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "invalid hex digit in \\uXXXX"});
					}
					std::uint32_t const lo = *lo_opt;
					i += 4;
					if (lo < 0xDC00U || lo > 0xDFFFU) {
						return std::unexpected(
							JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::invalid_unicode_escape,
								.message = "invalid low surrogate"});
					}
					cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
				}
				// NOLINTEND(readability-magic-numbers)
				append_utf8_to_sv(cp, [&](std::string_view chunk) {
					writer(chunk);
					total += chunk.size();
				});
				if (max_sz.exceeds(total, kDefaultMaxString)) {
					return std::unexpected(
						JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::string_too_large,
							.message = "decoded string exceeds max_string_size"});
				}
				break;
			}
		default:
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::syntax_error,
					.message = "invalid escape"});
		}
		if (simple) {
			++i;
			writer(std::string_view{esc_char, 1});
			total += 1;
			if (max_sz.exceeds(total, kDefaultMaxString)) {
				return std::unexpected(
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
	std::string_view raw_lexeme_{};
	bool has_escapes_{false};
	bool unquoted_{false};
	LimitOption max_string_size_{};

	friend class JsonReader;
	JsonStringToken(
		std::string_view raw_lex,
		bool has_esc,
		LimitOption max_sz) noexcept
		: raw_lexeme_{raw_lex}
		, has_escapes_{has_esc}
		, max_string_size_{max_sz} {}

public:
	JsonStringToken() = default;
	[[nodiscard]] std::string_view raw_lexeme() const noexcept { return raw_lexeme_; }
	[[nodiscard]] bool has_escapes() const noexcept { return has_escapes_; }
	[[nodiscard]] std::optional<std::string_view> unescaped_borrow() const noexcept {
		if (has_escapes_) {
			return std::nullopt;
		}
		if (unquoted_) {
			return raw_lexeme_;
		}
		return raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
	}
	[[nodiscard]] std::size_t max_decoded_size() const noexcept {
		if (unquoted_) {
			return raw_lexeme_.size();
		}
		return raw_lexeme_.size() >= 2 ? raw_lexeme_.size() - 2 : 0;
	}
	[[nodiscard]] std::expected<void, JsonError> append_decoded_to(std::string &out) const;
	[[nodiscard]] std::expected<std::string_view, JsonError> decode_into(std::span<char> buf) const;
};
// ---------------------------------------------------------------------------
// JsonReader
// ---------------------------------------------------------------------------

export class JsonReader {
public:
	enum class Event : std::uint8_t {
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
	friend class JsonStreamReader;
	struct StateFrame {
		enum class Kind : std::uint8_t {
			object,
			array,
		} kind;
		bool first{true};
		bool awaiting_value{false};
	};
	struct Checkpoint {
		std::size_t pos{};
		std::size_t line{1};
		std::size_t col{1};
		std::vector<StateFrame> stack{};
		JsonStringToken key_token{};
		JsonStringToken str_token{};
		JsonNumberView num_val{std::string_view{}, 0, 0};
		bool bool_val{false};
		bool has_error{false};
		JsonError last_error{};
		std::size_t value_start{};
	};
	std::string_view input_;
	JsonParseOptions opts_;
	std::size_t pos_{0};
	std::size_t line_{1};
	std::size_t col_{1};
	std::vector<StateFrame> stack_;
	JsonStringToken key_token_{};
	JsonStringToken str_token_{};
	JsonNumberView num_val_{std::string_view{}, 0, 0};
	bool bool_val_{false};
	bool has_error_{false};
	JsonError last_error_{};
	std::size_t value_start_{0};

	void set_error(JsonError e) noexcept;
	[[nodiscard]] JsonError mk_err(JsonIssueCode code, std::string msg) const;
	void skip_ws();
	[[nodiscard]] std::expected<void, JsonError> skip_ws_checked();
	void adv(std::size_t n = 1) noexcept;
	[[nodiscard]] std::expected<void, JsonError> parse_str_into_token(LimitOption max_sz, JsonStringToken &tok_out);
	[[nodiscard]] std::expected<void, JsonError> parse_str_sq_into_token(LimitOption max_sz, JsonStringToken &tok_out);
	[[nodiscard]] std::expected<void, JsonError> parse_number_into_val();
	[[nodiscard]] std::expected<Event, JsonError> parse_value_event();
	[[nodiscard]] Checkpoint checkpoint() const;
	void restore(Checkpoint checkpoint);
	void replace_input(std::string_view input) noexcept;

public:
	explicit JsonReader(std::string_view input, JsonParseOptions const &opts = {});
	explicit JsonReader(std::span<std::byte const> input, JsonParseOptions const &opts = {});
	[[nodiscard]] std::expected<std::optional<Event>, JsonError> next();
	[[nodiscard]] JsonStringToken key_token() const noexcept;
	[[nodiscard]] JsonStringToken string_token() const noexcept;
	[[nodiscard]] JsonNumberView number_val() const noexcept;
	[[nodiscard]] bool bool_val() const noexcept;
	[[nodiscard]] std::string_view input() const noexcept;
	[[nodiscard]] JsonParseOptions const &parse_options() const noexcept;
	[[nodiscard]] std::size_t depth() const noexcept;
	[[nodiscard]] bool has_error() const noexcept;
	[[nodiscard]] std::size_t pos() const noexcept;
	[[nodiscard]] std::size_t value_start_pos() const noexcept;
	void reset() noexcept;
	[[nodiscard]] std::expected<JsonByteRange, JsonError> skip_next_value();
};
export class JsonStreamReader {
public:
	using Event = JsonReader::Event;

private:
	std::string buf_{};
	JsonParseOptions opts_{};
	JsonReader reader_{std::string_view{}};
	bool closed_{false};
	bool has_error_{false};
	JsonError last_error_{};

	void refresh_reader_input() noexcept;
	void set_error(JsonError e) noexcept;
	[[nodiscard]] std::size_t configured_cap() const noexcept;
	[[nodiscard]] JsonError make_stream_error(JsonIssueCode code, std::string message) const;
	[[nodiscard]] bool tail_is_prefix_of_literal(std::size_t start) const noexcept;
	[[nodiscard]] bool trailing_utf8_prefix_needs_more() const noexcept;
	[[nodiscard]] bool recoverable_need_more(std::size_t checkpoint_pos, JsonError const &error) const noexcept;
	[[nodiscard]] bool event_needs_more_before_emit(Event event) const noexcept;

public:
	explicit JsonStreamReader(JsonParseOptions const &opts = {});
	[[nodiscard]] std::expected<void, JsonError> feed(std::string_view chunk);
	[[nodiscard]] std::expected<void, JsonError> feed(std::span<std::byte const> chunk);
	[[nodiscard]] std::expected<void, JsonError> close();
	[[nodiscard]] std::expected<std::optional<Event>, JsonError> next();
	[[nodiscard]] JsonStringToken key_token() const noexcept;
	[[nodiscard]] JsonStringToken string_token() const noexcept;
	[[nodiscard]] JsonNumberView number_val() const noexcept;
	[[nodiscard]] bool bool_val() const noexcept;
	[[nodiscard]] std::string_view input() const noexcept;
	[[nodiscard]] std::size_t depth() const noexcept;
	[[nodiscard]] bool has_error() const noexcept;
	[[nodiscard]] bool closed() const noexcept;
	[[nodiscard]] std::size_t pos() const noexcept;
	[[nodiscard]] std::size_t value_start_pos() const noexcept;
	[[nodiscard]] std::size_t buffered_bytes() const noexcept;
	void reset() noexcept;
};
// ---------------------------------------------------------------------------
// NodeRef
// ---------------------------------------------------------------------------

export class NodeRef {
	DocumentStorage const *storage_{};
	std::size_t idx_{};

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
		std::size_t i) noexcept
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
	[[nodiscard]] JsonKind kind() const noexcept;
	[[nodiscard]] bool is_null() const noexcept { return rec().kind == NodeKind::null_; }
	[[nodiscard]] std::expected<ObjectView, JsonError> as_object() const;
	[[nodiscard]] std::expected<ArrayView, JsonError> as_array() const;
	[[nodiscard]] std::expected<bool, JsonError> as_bool() const;
	[[nodiscard]] std::expected<std::string_view, JsonError> as_string() const;
	[[nodiscard]] std::expected<JsonNumberView, JsonError> as_number() const;
	[[nodiscard]] std::expected<std::int64_t, JsonError> as_i64() const;
	[[nodiscard]] std::expected<std::uint64_t, JsonError> as_u64() const;
	[[nodiscard]] std::expected<double, JsonError> as_double() const;
	template<class T>
	[[nodiscard]] std::expected<T, JsonError> as(JsonDecodeOptions const &opts = {}) const;
	[[nodiscard]] std::expected<NodeRef, JsonError> at(JsonPath const &path) const;
	[[nodiscard]] std::expected<NodeRef, JsonError> at_pointer(std::string_view pointer) const;
};
// ---------------------------------------------------------------------------
// ObjectMember (after NodeRef — NodeRef used by value)
// ---------------------------------------------------------------------------

export struct ObjectMember {
	std::string_view name;
	NodeRef value;
};
// ---------------------------------------------------------------------------
// Phase 6 helpers — std::hash table build + lookup
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline std::uint32_t hash_name(
	std::string_view name,
	std::uint64_t seed) noexcept {
#if defined(CONFLUX_JSON_HASH_PROVIDER_XXHASH)
	return static_cast<std::uint32_t>(XXH3_64bits_withSeed(name.data(), name.size(), seed));
#else
	std::uint64_t h = UINT64_C(1469598103934665603) ^ seed;
	for (char c: name) {
		auto const byte = static_cast<unsigned char>(c);
		h ^= static_cast<std::uint64_t>(byte);
		h *= UINT64_C(1099511628211);
	}
	h ^= h >> 33U;
	h *= UINT64_C(0xff51afd7ed558ccd);
	h ^= h >> 33U;
	return static_cast<std::uint32_t>(h);
#endif
}
// Smallest power-of-two >= 2*count, capped at kMaxHashTableCapacity AND
// at kMaxHashIndexBytes / sizeof(ObjHashSlot) (FI-7 — std::byte-budget cap).
// Returns 0 on overflow so the caller can fall back to linear scan.
[[nodiscard]] inline std::uint32_t clamped_capacity(
	std::uint32_t count) noexcept {
	constexpr std::uint32_t kSlotMax = static_cast<std::uint32_t>(kMaxHashIndexBytes / sizeof(ObjHashSlot));
	constexpr std::uint32_t kEffectiveMax = kMaxHashTableCapacity < kSlotMax ? kMaxHashTableCapacity : kSlotMax;
	std::uint32_t const target = count <= kEffectiveMax / 2U ? count * 2U : kEffectiveMax;
	std::uint32_t cap = 1;
	while (cap < target && cap < kEffectiveMax) {
		cap <<= 1;
	}
	if (cap < count) {
		return 0; // Object too large to index — fall back to linear scan.
	}
	return cap;
}
// Linear scan: returns val_node index or std::nullopt.
[[nodiscard]] inline std::optional<std::size_t> lookup_linear(
	DocumentStorage const *storage,
	std::size_t mem_start,
	std::size_t mem_count,
	std::string_view name) noexcept {
	for (std::size_t i = 0; i < mem_count; ++i) {
		auto const &m = storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		if (storage->member_name(m) == name) {
			return m.val_node;
		}
	}
	return std::nullopt;
}
// Probe std::hash table; fall back to linear if probe chain exceeds kProbeChainMax.
[[nodiscard]] inline std::optional<std::size_t> lookup_in(
	ObjHashTable const &ht,
	DocumentStorage const *storage,
	std::size_t mem_start,
	std::size_t mem_count,
	std::string_view name) noexcept {
	auto const h = hash_name(name, ht.hash_seed);
	std::uint32_t const mask = ht.capacity - 1;
	std::uint32_t slot = h & mask;
	auto const *slots = ht.slots_data();
	auto const *const *ptr_cache = ht.ptr_cache_data();
	for (std::uint32_t probe = 0; probe < kProbeChainMax; ++probe) {
		auto const &s = slots[slot]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (s.member_index == kEmptySlot) {
			return std::nullopt;
		}
		if (s.name_hash == h) {
			auto const &m = storage->object_members
								[mem_start + s.member_index]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			// Item C: ptr_cache holds pre-resolved data pointer; no dispatch per probe
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			if (std::string_view{ptr_cache[s.member_index], m.name_len} == name) {
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
	std::size_t mem_start,
	std::size_t mem_count) noexcept {
	std::uint32_t const mask = ht.capacity - 1;
	auto *slots = ht.slots_data();
	auto **ptr_cache = ht.ptr_cache_data();
	for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mem_count); ++i) {
		auto const &m = storage->object_members[mem_start + i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		auto const sv = storage->member_name(m);
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		ptr_cache[i] = sv.data(); // Item C: cache pointer in ptr_cache (arena stable post-parse)
		auto const h = hash_name(sv, ht.hash_seed);
		std::uint32_t slot = h & mask;
		bool inserted = false;
		for (std::uint32_t probe = 0; probe < kProbeChainMax; ++probe) {
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
	std::size_t mem_start_{};
	std::size_t mem_count_{};
	std::size_t node_idx_{};

	friend class NodeRef;
	friend class Document;
	friend std::expected<void, JsonError> warm_member_index_impl(DocumentStorage *, NodeRef);
	bool friend is_value_equal(NodeRef, NodeRef);
	bool friend is_value_equal_exact(NodeRef, NodeRef);
	ObjectView(
		DocumentStorage const *s,
		std::size_t start,
		std::size_t count,
		std::size_t node_idx) noexcept
		: storage_{s}
		, mem_start_{start}
		, mem_count_{count}
		, node_idx_{node_idx} {}

public:
	[[nodiscard]] std::size_t size() const noexcept { return mem_count_; }
	[[nodiscard]] std::optional<NodeRef> find_member(std::string_view name) const noexcept;
	[[nodiscard]] std::expected<NodeRef, JsonError> member(std::string_view name) const;
	template<class T>
	[[nodiscard]] std::expected<T, JsonError> required(std::string_view name, JsonDecodeOptions const &opts = {}) const;
	template<class T>
	[[nodiscard]] std::expected<std::optional<T>, JsonError>
	optional(std::string_view name, JsonDecodeOptions const &opts = {}) const;
	[[nodiscard]] ObjectMemberRange members() const noexcept;
};
export class ArrayView {
	DocumentStorage const *storage_{};
	std::size_t child_start_{};
	std::size_t child_count_{};

	friend class NodeRef;
	bool friend is_value_equal(NodeRef, NodeRef);
	bool friend is_value_equal_exact(NodeRef, NodeRef);
	ArrayView(
		DocumentStorage const *s,
		std::size_t start,
		std::size_t count) noexcept
		: storage_{s}
		, child_start_{start}
		, child_count_{count} {}

public:
	[[nodiscard]] std::size_t size() const noexcept { return child_count_; }
	[[nodiscard]] std::expected<NodeRef, JsonError> element(std::size_t index) const;
	[[nodiscard]] ArrayElementRange elements() const noexcept;
};
// ---------------------------------------------------------------------------
// Iteration ranges
// ---------------------------------------------------------------------------

export class ObjectMemberRange {
	DocumentStorage const *storage_{};
	std::size_t start_{};
	std::size_t count_{};

	friend class ObjectView;
	ObjectMemberRange(
		DocumentStorage const *s,
		std::size_t start,
		std::size_t count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		std::size_t start_{};
		std::size_t idx_{};
		std::size_t count_{};
		friend class ObjectMemberRange;
		Iterator(
			DocumentStorage const *s,
			std::size_t st,
			std::size_t cnt,
			std::size_t i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = std::ptrdiff_t;
		using value_type = ObjectMember;
		using iterator_category = std::forward_iterator_tag;
		Iterator() = default;
		[[nodiscard]] ObjectMember operator *() const;
		Iterator &operator ++() noexcept;
		Iterator operator ++(int) noexcept;
		[[nodiscard]] bool operator ==(Sentinel) const noexcept;
		[[nodiscard]] bool operator ==(Iterator const &o) const noexcept;
	};
	[[nodiscard]] Iterator begin() const noexcept;
	[[nodiscard]] Sentinel end() const noexcept;
};
export class ArrayElementRange {
	DocumentStorage const *storage_{};
	std::size_t start_{};
	std::size_t count_{};

	friend class ArrayView;
	ArrayElementRange(
		DocumentStorage const *s,
		std::size_t start,
		std::size_t count) noexcept
		: storage_{s}
		, start_{start}
		, count_{count} {}

public:
	struct Sentinel {};
	class Iterator {
		DocumentStorage const *storage_{};
		std::size_t start_{};
		std::size_t idx_{};
		std::size_t count_{};
		friend class ArrayElementRange;
		Iterator(
			DocumentStorage const *s,
			std::size_t st,
			std::size_t cnt,
			std::size_t i) noexcept
			: storage_{s}
			, start_{st}
			, idx_{i}
			, count_{cnt} {}

	public:
		using difference_type = std::ptrdiff_t;
		using value_type = NodeRef;
		using iterator_category = std::forward_iterator_tag;
		Iterator() = default;
		[[nodiscard]] NodeRef operator *() const;
		Iterator &operator ++() noexcept;
		Iterator operator ++(int) noexcept;
		[[nodiscard]] bool operator ==(Sentinel) const noexcept;
		[[nodiscard]] bool operator ==(Iterator const &o) const noexcept;
	};
	[[nodiscard]] Iterator begin() const noexcept;
	[[nodiscard]] Sentinel end() const noexcept;
};
// ---------------------------------------------------------------------------
// Comparison free functions + identity helpers
// ---------------------------------------------------------------------------

export bool is_same_node(NodeRef a, NodeRef b) noexcept;
// NOLINTNEXTLINE(misc-no-recursion)
export bool is_value_equal(NodeRef a, NodeRef b);
// NOLINTNEXTLINE(misc-no-recursion)
export bool is_value_equal_exact(NodeRef a, NodeRef b);
export struct NodeIdentityHash {
	std::size_t operator ()(NodeRef n) const noexcept;
};
export struct NodeIdentityEqual {
	bool operator ()(NodeRef a, NodeRef b) const noexcept;
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
	std::optional<std::size_t> truncate_depth{};
};
export struct WarmIndexOptions {
	std::size_t max_objects{SIZE_MAX};
	std::size_t max_extra_bytes{SIZE_MAX};
};
[[nodiscard]] std::expected<void, JsonError> warm_member_index_impl(DocumentStorage *storage, NodeRef node);
export class Document {
	std::unique_ptr<DocumentStorage> storage_;

	friend class ValueBuilder;
	Document friend make_document(std::unique_ptr<DocumentStorage>) noexcept;
	explicit Document(
		std::unique_ptr<DocumentStorage> s) noexcept
		: storage_{std::move(s)} {}

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
	template<class T>
	[[nodiscard]] std::expected<T, JsonError> get(std::string_view pointer, JsonDecodeOptions const &opts = {}) const;
	[[nodiscard]] std::expected<std::string, JsonError> dump(JsonDumpOptions const &opts = {}) const;
	// Pre-build std::hash index for the given object node (idempotent, std::thread-safe).
	[[nodiscard]] std::expected<void, JsonError> warm_member_index(NodeRef node) const;
	// Pre-build std::hash indices for every object node in the document.
	[[nodiscard]] std::expected<void, JsonError> warm_member_indices(WarmIndexOptions const &opts = {}) const;
	[[nodiscard]] JsonParseStorageStats parse_storage_stats() const noexcept {
		assert(storage_ && "Document::parse_storage_stats() called on empty Document");
		return storage_->storage_stats();
	}
};
// Module-private factory: parse and builder use this to construct Documents.
Document make_document(std::unique_ptr<DocumentStorage> s) noexcept;
// ─── Phase 5.2 — JsonArena / ArenaDocument ──────────────────────────────────

export struct JsonArenaOptions {
	std::size_t initial_slab{64 * 1024};
	std::pmr::memory_resource *hash_index_resource{nullptr};
};
export class ArenaDocument {
	DocumentStorage const *storage_{};
	std::uint32_t generation_{};
	std::uint32_t const *arena_gen_{};

	friend class JsonArena;
	ArenaDocument(
		DocumentStorage const *s,
		std::uint32_t gen,
		std::uint32_t const *ag) noexcept
		: storage_{s}
		, generation_{gen}
		, arena_gen_{ag} {}
	[[nodiscard]] bool live_() const noexcept {
		return storage_ != nullptr && arena_gen_ != nullptr && *arena_gen_ == generation_;
	}
	void check_live() const noexcept {
		if (!live_()) {
			std::terminate();
		}
	}

public:
	ArenaDocument() = default;
	[[nodiscard]] bool is_live() const noexcept { return live_(); }
	[[nodiscard]] NodeRef root() const noexcept {
		check_live();
		return NodeRef{storage_, storage_->root_node};
	}
	[[nodiscard]] std::expected<void, JsonError> warm_member_index(NodeRef node) const;
	[[nodiscard]] std::expected<std::string, JsonError> dump(JsonDumpOptions const &opts = {}) const;
	[[nodiscard]] JsonParseStorageStats parse_storage_stats() const noexcept {
		check_live();
		return storage_->storage_stats();
	}
};
export class JsonArena {
	std::size_t initial_slab_;
	std::pmr::monotonic_buffer_resource mbr_;
	std::pmr::memory_resource *hash_index_resource_;
	std::unique_ptr<DocumentStorage> storage_;
	std::uint32_t generation_{0};
	void reset_storage_for_reuse() noexcept;

public:
	explicit JsonArena(
		JsonArenaOptions const &opts = {})
		: initial_slab_{opts.initial_slab}
		, mbr_{opts.initial_slab}
		, hash_index_resource_{(opts.hash_index_resource != nullptr) ? opts.hash_index_resource : &mbr_}
		, storage_{std::make_unique<DocumentStorage>(&mbr_, hash_index_resource_)} {}
	JsonArena(JsonArena const &) = delete;
	JsonArena &operator =(JsonArena const &) = delete;
	JsonArena(JsonArena &&) = delete;
	JsonArena &operator =(JsonArena &&) = delete;

	[[nodiscard]] std::expected<ArenaDocument, JsonError>
	parse_into(std::string_view input, JsonParseOptions const &opts = {});

	[[nodiscard]] std::expected<ArenaDocument, JsonError>
	parse_borrowed_into(std::string_view input, JsonParseOptions const &opts = {});

	[[nodiscard]] std::expected<ArenaDocument, JsonError>
	parse_moved_into(std::string input, JsonParseOptions const &opts = {});

	void reset() noexcept;
	[[nodiscard]] std::size_t slab_capacity() const noexcept { return initial_slab_; }
	[[nodiscard]] std::size_t slab_used() const noexcept {
		if (!storage_) {
			return 0;
		}
		auto const stats = storage_->storage_stats();
		return stats.nodes_size * sizeof(Node)
			 + stats.array_children_size * sizeof(std::uint32_t)
			 + stats.object_members_size * sizeof(MemberEntry)
			 + stats.string_arena_size;
	}
};
// ---------------------------------------------------------------------------
// Field accessor helpers (Phase 1.1)
// ---------------------------------------------------------------------------

export [[nodiscard]] std::expected<std::string, JsonError> require_string(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::int64_t, JsonError> require_int(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::uint64_t, JsonError> require_uint(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<double, JsonError> require_double(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<bool, JsonError> require_bool(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::optional<std::string>, JsonError>
optional_string(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::optional<std::int64_t>, JsonError>
optional_int(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::optional<std::uint64_t>, JsonError>
optional_uint(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::optional<double>, JsonError>
optional_double(ObjectView const &obj, std::string_view name);
export [[nodiscard]] std::expected<std::optional<bool>, JsonError>
optional_bool(ObjectView const &obj, std::string_view name);
// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Phase 8 — SIMD scan for the Tokenizer hot path via std::experimental::simd.
// ---------------------------------------------------------------------------
namespace detail::simd {

[[nodiscard]] inline std::size_t scan_str_until_special(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
#if defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold) {
		return conflux_json_scan_str_until_special_stdsimd(p, n);
	}
#elif defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME && defined(CONFLUX_JSON_STDSIMD_IFUNC)
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold) {
		return conflux_json_scan_str_until_special_stdsimd(p, n);
	}
#elif defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
		return conflux_json_scan_str_until_special_stdsimd(p, n);
	}
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
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
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
			return i + static_cast<std::size_t>(__builtin_ctz(mask16));
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
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
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

extern "C" std::size_t conflux_json_scan_str_until_special_auto(
	char const *p,
	std::size_t n) noexcept {
	return detail::simd::scan_str_until_special(p, n);
}

// Phase 3 — parser implementation lives in src/json_parse.cxx. The primary
// module keeps only the exported overload set so importers see the same
// `conflux.json` surface while parser edits avoid rebuilding unrelated BMI text.
export namespace conflux::json {

// Explicit owning parse: copies input into the Document's owned buffer.
// Number lexemes index directly into that buffer (zero-copy on read paths).
std::expected<Document, JsonError> parse_copy(std::string_view input, JsonParseOptions const &opts = {});
// Move-in owning overload: avoids the input copy. Keep this a concrete
// std::string rvalue overload so unrelated string-like temporaries continue to
// select parse_copy(string_view) instead of trying to become owned storage.
std::expected<Document, JsonError> parse_copy(std::string &&input, JsonParseOptions const &opts = {});
// Borrow-only overload: caller guarantees the bytes outlive the Document.
// Rvalue overload is deleted to prevent obvious lifetime mistakes.
std::expected<Document, JsonError> parse_borrowed(std::string_view input, JsonParseOptions const &opts = {});
std::expected<Document, JsonError> parse_borrowed_unsafe(std::string_view input, JsonParseOptions const &opts = {});
std::expected<Document, JsonError> parse_view(std::string_view input, JsonParseOptions const &opts = {});
// Performance-default parse: borrows/view-parses from stable caller-owned
// storage. Use parse_copy(...) when the returned Document must own the bytes.
std::expected<Document, JsonError> parse(std::string_view input, JsonParseOptions const &opts = {});

// Deleted std::string rvalue overloads (Correction T) — borrowing requires
// caller-owned bytes. String literals and string_view temporaries still select
// the string_view overloads; only owned string temporaries are rejected.
template<typename T>
	requires(std::same_as<std::remove_cvref_t<T>, std::string> && !std::is_lvalue_reference_v<T>)
std::expected<Document, JsonError> parse(T &&, JsonParseOptions const & = {}) = delete;
template<typename T>
	requires(std::same_as<std::remove_cvref_t<T>, std::string> && !std::is_lvalue_reference_v<T>)
std::expected<Document, JsonError> parse_borrowed(T &&, JsonParseOptions const & = {}) = delete;
template<typename T>
	requires(std::same_as<std::remove_cvref_t<T>, std::string> && !std::is_lvalue_reference_v<T>)
std::expected<Document, JsonError> parse_borrowed_unsafe(T &&, JsonParseOptions const & = {}) = delete;
template<typename T>
	requires(std::same_as<std::remove_cvref_t<T>, std::string> && !std::is_lvalue_reference_v<T>)
std::expected<Document, JsonError> parse_view(T &&, JsonParseOptions const & = {}) = delete;

// pmr-injecting overloads — caller supplies the memory resource.
// The resource must outlive every Document (and NodeRef) derived from it.
std::expected<Document, JsonError>
parse_copy(std::string_view input, JsonParseOptions const &opts, std::pmr::memory_resource *resource);
std::expected<Document, JsonError>
parse_borrowed(std::string_view input, JsonParseOptions const &opts, std::pmr::memory_resource *resource);
std::expected<Document, JsonError>
parse_borrowed_unsafe(std::string_view input, JsonParseOptions const &opts, std::pmr::memory_resource *resource);
std::expected<Document, JsonError>
parse_view(std::string_view input, JsonParseOptions const &opts, std::pmr::memory_resource *resource);
std::expected<Document, JsonError>
parse(std::string_view input, JsonParseOptions const &opts, std::pmr::memory_resource *resource);

template<typename T>
	requires(std::same_as<std::remove_cvref_t<T>, std::string> && !std::is_lvalue_reference_v<T>)
std::expected<Document, JsonError> parse(T &&, JsonParseOptions const &, std::pmr::memory_resource *) = delete;

} // namespace conflux::json

export namespace conflux::json {

[[nodiscard]] std::expected<Document, JsonError>
parse_dom(std::string_view input, JsonDomPolicy const &policy = JsonDomPolicy::view_first());
[[nodiscard]] std::expected<Document, JsonError>
parse_dom(std::string &&input, JsonDomPolicy const &policy = JsonDomPolicy::owning_document());
[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string_view input,
	std::pmr::memory_resource *resource,
	JsonDomPolicy const &policy = JsonDomPolicy::caller_pmr());
[[nodiscard]] std::expected<ArenaDocument, JsonError>
parse_dom(JsonArena &arena, std::string_view input, JsonDomPolicy const &policy = JsonDomPolicy::arena_reuse());
[[nodiscard]] std::expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	std::string &&input,
	JsonDomPolicy const &policy =
		JsonDomPolicy{.input = JsonDomInputOwnership::owned_move, .storage = JsonDomStorageModel::reusable_arena});

} // namespace conflux::json
