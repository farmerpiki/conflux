module;
#include <cassert>

module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

// ---------------------------------------------------------------------------
// JsonPath::from_pointer (after JsonError definition)
// ---------------------------------------------------------------------------

std::optional<std::string> decode_json_pointer_token(
	std::string_view token) {
	std::string decoded;
	decoded.reserve(token.size());
	for (std::size_t i = 0; i < token.size(); ++i) {
		if (token[i] != '~') {
			decoded += token[i];
			continue;
		}
		if (i + 1 >= token.size()) {
			return std::nullopt;
		}
		++i;
		if (token[i] == '0') {
			decoded += '~';
		} else if (token[i] == '1') {
			decoded += '/';
		} else {
			return std::nullopt;
		}
	}
	return decoded;
}

std::string JsonPath::to_pointer() const {
	if (segs_.empty()) {
		return "";
	}
	std::string out;
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
			out += std::to_string(get<JsonPathIndex>(seg).index);
		}
	}
	return out;
}

std::expected<JsonPath, JsonError> JsonPath::from_pointer(
	std::string_view sv) {
	if (sv.empty()) {
		return JsonPath{};
	}
	if (sv[0] != '/') {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::invalid_pointer,
				.message = "JSON Pointer must start with '/' or be empty"});
	}
	JsonPath result;
	std::size_t pos = 1;
	while (pos <= sv.size()) {
		std::size_t slash = sv.find('/', pos);
		if (slash == std::string_view::npos) {
			slash = sv.size();
		}
		auto name = decode_json_pointer_token(sv.substr(pos, slash - pos));
		if (!name) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::invalid_pointer,
					.message = "invalid '~' escape in JSON Pointer"});
		}
		result.push_member(*name);
		pos = slash + 1;
	}
	return result;
}
// ---------------------------------------------------------------------------
// Implement NodeRef methods that need ObjectView/ArrayView
// ---------------------------------------------------------------------------

JsonKind NodeRef::kind() const noexcept {
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

std::expected<ObjectView, JsonError> NodeRef::as_object() const {
	if (rec().kind != NodeKind::object) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::object,
				.actual_kind = kind(),
				.message = "std::expected object"});
	}
	return ObjectView{storage_, rec().off, rec().len, idx_};
}
std::expected<ArrayView, JsonError> NodeRef::as_array() const {
	if (rec().kind != NodeKind::array_) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::array,
				.actual_kind = kind(),
				.message = "std::expected array"});
	}
	return ArrayView{storage_, rec().off, rec().len};
}

std::expected<bool, JsonError> NodeRef::as_bool() const {
	if (rec().kind != NodeKind::boolean) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::boolean,
				.actual_kind = kind(),
				.message = "std::expected boolean"});
	}
	return rec().bool_val;
}

std::expected<std::string_view, JsonError> NodeRef::as_string() const {
	if (rec().kind != NodeKind::string_) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::string,
				.actual_kind = kind(),
				.message = "std::expected string"});
	}
	return storage_->bytes_at(rec().off, rec().len, rec().flags);
}

std::expected<JsonNumberView, JsonError> NodeRef::as_number() const {
	if (rec().kind != NodeKind::number) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::number,
				.actual_kind = kind(),
				.message = "std::expected number"});
	}
	return JsonNumberView{storage_->bytes_at(rec().off, rec().len, rec().flags), rec().flags, rec()._raw};
}

std::expected<std::int64_t, JsonError> NodeRef::as_i64() const {
	return as_number().and_then([](JsonNumberView n) { return n.to_i64(); });
}

std::expected<std::uint64_t, JsonError> NodeRef::as_u64() const {
	return as_number().and_then([](JsonNumberView n) { return n.to_u64(); });
}

std::expected<double, JsonError> NodeRef::as_double() const {
	return as_number().and_then([](JsonNumberView n) { return n.to_f64(); });
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
std::expected<NodeRef, JsonError> NodeRef::at_pointer(
	std::string_view pointer) const {
	auto path = JsonPath::from_pointer(pointer);
	if (!path) {
		return std::unexpected(std::move(path).error());
	}
	return at(*path);
}
std::expected<NodeRef, JsonError> NodeRef::at(
	JsonPath const &path) const {
	NodeRef cur = *this;
	for (std::size_t i = 0; i < path.size(); ++i) {
		auto const &seg = path.segment(i);
		auto set_path = [&](JsonError err) {
			err.path = JsonPath{};
			for (std::size_t j = 0; j <= i; ++j) {
				push_seg(err.path, path.segment(j));
			}
			return std::unexpected(std::move(err));
		};
		if (holds_alternative<JsonPathMember>(seg)) {
			auto const &name = get<JsonPathMember>(seg).name;
			if (cur.kind() == JsonKind::array) {
				bool all_digits = !name.empty() && (name.size() == 1 || name[0] != '0');
				for (std::size_t k = 0; all_digits && k < name.size(); ++k) {
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
				std::size_t idx = 0;
				for (char const ch: name) {
					idx = idx * 10 + static_cast<std::size_t>(ch - '0');
				}
				auto arr = cur.as_array();
				if (!arr) {
					return set_path(std::move(arr).error());
				}
				auto child = arr->element(idx);
				if (!child) {
					return set_path(std::move(child).error());
				}
				cur = *child;
			} else {
				auto obj = cur.as_object();
				if (!obj) {
					return set_path(std::move(obj).error());
				}
				auto child = obj->member(name);
				if (!child) {
					return set_path(std::move(child).error());
				}
				cur = *child;
			}
		} else {
			auto arr = cur.as_array();
			if (!arr) {
				return set_path(std::move(arr).error());
			}
			auto child = arr->element(get<JsonPathIndex>(seg).index);
			if (!child) {
				return set_path(std::move(child).error());
			}
			cur = *child;
		}
	}
	return cur;
}

std::optional<NodeRef> ObjectView::find_member(
	std::string_view name) const noexcept {
	auto to_ref = [&](std::optional<std::size_t> idx) -> std::optional<NodeRef> {
		if (!idx) {
			return std::nullopt;
		}
		return NodeRef{storage_, *idx};
	};
	if (mem_count_ < kHashThreshold) {
		return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
	}
	// Lazy std::hash table build via Atom CAS. The std::hash slot is the only
	// mutable surface on a published Document.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto &raw = const_cast<ObjHashTable *&>(storage_->nodes[node_idx_].hash_idx_raw);
	auto ref = std::atomic_ref<ObjHashTable *>{raw};
	auto *ht = ref.load(std::memory_order_acquire);
	if (ht == kHashBuildFailedSentinel) {
		return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
	}
	if (ht == nullptr) {
		std::uint32_t const cap = detail::clamped_capacity(static_cast<std::uint32_t>(mem_count_));
		bool build_ok = false;
		ObjHashTable *owned = nullptr;
		if (cap > 0) {
			owned = ObjHashTable::create(
				cap,
				static_cast<std::uint32_t>(mem_count_),
				detail::make_hash_seed(),
				storage_->hash_mr_);
			if (owned != nullptr && detail::build_table(*owned, storage_, mem_start_, mem_count_)) {
				ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
				if (ref.compare_exchange_strong(
						expected_null,
						owned,
						std::memory_order_release,
						std::memory_order_acquire)) {
					ht = owned;
					owned = nullptr;
					build_ok = true;
				} else {
					ht = (expected_null == kHashBuildFailedSentinel) ? nullptr : expected_null;
					build_ok = (ht != nullptr);
				}
			}
		}
		if (!build_ok) {
			ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
			auto _ = ref.compare_exchange_strong(
				expected_null,
				kHashBuildFailedSentinel,
				std::memory_order_release,
				std::memory_order_acquire);
		}
		ObjHashTable::destroy(owned);
	}
	if (ht != nullptr) {
		return to_ref(detail::lookup_in(*ht, storage_, mem_start_, mem_count_, name));
	}
	return to_ref(detail::lookup_linear(storage_, mem_start_, mem_count_, name));
}

std::expected<NodeRef, JsonError> ObjectView::member(
	std::string_view name) const {
	auto found = find_member(name);
	if (!found) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::missing_member,
				.member_name = std::string{name},
				.message = std::format("missing member: {}", name)});
	}
	return *found;
}

ObjectMemberRange ObjectView::members() const noexcept {
	return {storage_, mem_start_, mem_count_};
}

std::expected<NodeRef, JsonError> ArrayView::element(
	std::size_t index) const {
	if (index >= child_count_) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::index_out_of_range,
				.requested_index = index,
				.container_size = child_count_,
				.message = std::format("index {} out of range (size={})", index, child_count_)});
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	return NodeRef{storage_, storage_->array_children[child_start_ + index]};
}

ArrayElementRange ArrayView::elements() const noexcept {
	return {storage_, child_start_, child_count_};
}

ObjectMember ObjectMemberRange::Iterator::operator *() const {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto const &m = storage_->object_members[start_ + idx_];
	return {
		storage_->member_name(m),
		NodeRef{storage_, m.val_node}
    };
}

ObjectMemberRange::Iterator &ObjectMemberRange::Iterator::operator ++() noexcept {
	++idx_;
	return *this;
}

ObjectMemberRange::Iterator ObjectMemberRange::Iterator::operator ++(
	int) noexcept {
	auto t = *this;
	++idx_;
	return t;
}

bool ObjectMemberRange::Iterator::operator ==(
	Sentinel) const noexcept {
	return idx_ >= count_;
}

bool ObjectMemberRange::Iterator::operator ==(
	Iterator const &o) const noexcept {
	return idx_ == o.idx_;
}

ObjectMemberRange::Iterator ObjectMemberRange::begin() const noexcept {
	return {storage_, start_, count_, 0};
}

ObjectMemberRange::Sentinel ObjectMemberRange::end() const noexcept {
	return {};
}

NodeRef ArrayElementRange::Iterator::operator *() const {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	return NodeRef{storage_, storage_->array_children[start_ + idx_]};
}

ArrayElementRange::Iterator &ArrayElementRange::Iterator::operator ++() noexcept {
	++idx_;
	return *this;
}

ArrayElementRange::Iterator ArrayElementRange::Iterator::operator ++(
	int) noexcept {
	auto t = *this;
	++idx_;
	return t;
}

bool ArrayElementRange::Iterator::operator ==(
	Sentinel) const noexcept {
	return idx_ >= count_;
}

bool ArrayElementRange::Iterator::operator ==(
	Iterator const &o) const noexcept {
	return idx_ == o.idx_;
}

ArrayElementRange::Iterator ArrayElementRange::begin() const noexcept {
	return {storage_, start_, count_, 0};
}

ArrayElementRange::Sentinel ArrayElementRange::end() const noexcept {
	return {};
}

bool is_same_node(
	NodeRef a,
	NodeRef b) noexcept {
	return a.storage_ == b.storage_ && a.idx_ == b.idx_;
}

// NOLINTNEXTLINE(misc-no-recursion)
bool is_value_equal(
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
			return std::ranges::equal(av.elements(), bv.elements(), [](NodeRef lhs, NodeRef rhs) {
				return is_value_equal(lhs, rhs);
			});
		}
	case NodeKind::object:
		{
			ObjectView const ao{a.storage_, a.rec().off, a.rec().len, a.idx_};
			ObjectView const bo{b.storage_, b.rec().off, b.rec().len, b.idx_};
			if (ao.size() != bo.size()) {
				return false;
			}
			return std::ranges::all_of(ao.members(), [&bo](ObjectMember const member) {
				auto found = bo.find_member(member.name);
				return found && is_value_equal(member.value, *found);
			});
		}
	}
	return false;
}

// NOLINTNEXTLINE(misc-no-recursion)
bool is_value_equal_exact(
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
			return std::ranges::equal(av.elements(), bv.elements(), [](NodeRef lhs, NodeRef rhs) {
				return is_value_equal_exact(lhs, rhs);
			});
		}
	case NodeKind::object:
		{
			ObjectView const ao{a.storage_, a.rec().off, a.rec().len, a.idx_};
			ObjectView const bo{b.storage_, b.rec().off, b.rec().len, b.idx_};
			if (ao.size() != bo.size()) {
				return false;
			}
			return std::ranges::all_of(ao.members(), [&bo](ObjectMember const member) {
				auto found = bo.find_member(member.name);
				return found && is_value_equal_exact(member.value, *found);
			});
		}
	}
	return false;
}

std::size_t NodeIdentityHash::operator ()(
	NodeRef n) const noexcept {
	return std::hash<void const *>{}(n.storage_) ^ (std::hash<std::size_t>{}(n.idx_) << 1U);
}

bool NodeIdentityEqual::operator ()(
	NodeRef a,
	NodeRef b) const noexcept {
	return is_same_node(a, b);
}

// ---------------------------------------------------------------------------
// Document / arena cold lookup helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<void, JsonError> warm_member_index_impl(
	DocumentStorage *storage,
	NodeRef node) {
	auto obj_or = node.as_object();
	if (!obj_or) {
		return std::unexpected(std::move(obj_or).error());
	}
	auto const &ov = *obj_or;
	if (ov.mem_count_ < kHashThreshold) {
		return {};
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto &slot = storage->nodes[ov.node_idx_].hash_idx_raw;
	auto ref = std::atomic_ref<ObjHashTable *>{slot};
	auto *prior = ref.load(std::memory_order_acquire);
	if (prior != nullptr && prior != kHashBuildFailedSentinel) {
		return {}; // already built
	}
	if (prior == kHashBuildFailedSentinel) {
		// Cached prior failure — surface the same error.
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::resource_exhausted,
				.message = "object std::hash index unavailable (cached failure)"});
	}
	ObjHashTable *owned = nullptr;
	auto stash_failure_sentinel = [&] {
		ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
		auto _ = ref.compare_exchange_strong(
			expected_null,
			kHashBuildFailedSentinel,
			std::memory_order_release,
			std::memory_order_acquire);
	};
	try {
		std::uint32_t const cap = detail::clamped_capacity(static_cast<std::uint32_t>(ov.mem_count_));
		if (cap == 0) {
			stash_failure_sentinel();
			return std::unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "object exceeds std::hash-index std::byte budget"});
		}
		owned = ObjHashTable::create(
			cap,
			static_cast<std::uint32_t>(ov.mem_count_),
			detail::make_hash_seed(),
			storage->hash_mr_);
		if (owned == nullptr) {
			stash_failure_sentinel();
			return std::unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "OOM building object std::hash index"});
		}
		if (!detail::build_table(*owned, storage, ov.mem_start_, ov.mem_count_)) {
			ObjHashTable::destroy(owned);
			stash_failure_sentinel();
			return std::unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "object std::hash build exceeded probe-chain cap"});
		}
		ObjHashTable *expected_null = nullptr; // NOLINT(misc-const-correctness)
		if (!ref.compare_exchange_strong(expected_null, owned, std::memory_order_release, std::memory_order_acquire)) {
			ObjHashTable::destroy(owned); // lost race — other std::thread published first
		}
		return {};
	} catch (std::bad_alloc const &) {
		ObjHashTable::destroy(owned);
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::resource_exhausted,
				.message = "OOM building object std::hash index"});
	} catch (...) {
		ObjHashTable::destroy(owned);
		assert(false && "warm_member_index: std::unexpected std::exception from no-user-code build path");
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::constraint_violation,
				.message = "std::unexpected std::exception building object std::hash index"});
	}
}

std::expected<void, JsonError> Document::warm_member_index(
	NodeRef node) const {
	return warm_member_index_impl(storage_.get(), node);
}

std::expected<void, JsonError> Document::warm_member_indices(
	WarmIndexOptions const &opts) const {
	std::size_t objects_warmed = 0;
	std::size_t bytes_allocated = 0;
	for (std::size_t i = 0; i < storage_->nodes.size(); ++i) {
		auto &n = storage_->nodes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		if (n.kind != NodeKind::object) {
			continue;
		}
		auto const mem_count = n.len;
		if (mem_count < kHashThreshold) {
			continue;
		}
		if (std::atomic_ref<ObjHashTable *>{n.hash_idx_raw}.load(std::memory_order_acquire) != nullptr) {
			continue; // already indexed or failed
		}
		std::uint32_t const cap = detail::clamped_capacity(static_cast<std::uint32_t>(mem_count));
		std::size_t const est_bytes =
			cap > 0 ? sizeof(ObjHashTable) + static_cast<std::size_t>(cap) * sizeof(ObjHashSlot) : 0;
		if (objects_warmed >= opts.max_objects) {
			break;
		}
		if (est_bytes > 0
			&& opts.max_extra_bytes != std::numeric_limits<std::size_t>::max()
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

Document make_document(
	std::unique_ptr<DocumentStorage> s) noexcept {
	return Document{std::move(s)};
}

std::expected<void, JsonError> ArenaDocument::warm_member_index(
	NodeRef node) const {
	check_live();
	// Arena documents own their storage; this forwards to the same std::hash-index
	// builder used by Document without transferring ownership.
	return warm_member_index_impl(const_cast<DocumentStorage *>(storage_), node);
}

} // namespace conflux::json
