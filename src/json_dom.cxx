module;
#include <cassert>

module conflux.json;

import std;
import std.compat;
import conflux.types;

// ---------------------------------------------------------------------------
// JsonPath::from_pointer (after JsonError definition)
// ---------------------------------------------------------------------------

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
		std::string name;
		name.reserve(slash - pos);
		for (std::size_t i = pos; i < slash; ++i) {
			if (sv[i] == '~') {
				if (i + 1 >= slash) {
					return std::unexpected(
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
					return std::unexpected(
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
// Implement NodeRef methods that need ObjectView/ArrayView
// ---------------------------------------------------------------------------

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
ObjectMemberRange ObjectView::members() const noexcept {
	return {storage_, mem_start_, mem_count_};
}
ArrayElementRange ArrayView::elements() const noexcept {
	return {storage_, child_start_, child_count_};
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
		owned = ObjHashTable::create(cap, static_cast<std::uint32_t>(ov.mem_count_), storage->hash_mr_);
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
		std::size_t const est_bytes = cap > 0 ? sizeof(ObjHashTable) + static_cast<std::size_t>(cap) * sizeof(ObjHashSlot) : 0;
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
