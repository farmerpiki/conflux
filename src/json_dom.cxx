module;
#include <cassert>

module conflux.json;

import std;
import std.compat;
import conflux.types;

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
expected<NodeRef, JsonError> NodeRef::at_pointer(
	SV pointer) const {
	auto path = JsonPath::from_pointer(pointer);
	if (!path) {
		return unexpected(move(path).error());
	}
	return at(*path);
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
ObjectMemberRange ObjectView::members() const noexcept {
	return {storage_, mem_start_, mem_count_};
}
ArrayElementRange ArrayView::elements() const noexcept {
	return {storage_, child_start_, child_count_};
}

// ---------------------------------------------------------------------------
// Document / arena cold lookup helpers
// ---------------------------------------------------------------------------

[[nodiscard]] expected<void, JsonError> warm_member_index_impl(
	DocumentStorage *storage,
	NodeRef node) {
	auto obj_or = node.as_object();
	if (!obj_or) {
		return unexpected(move(obj_or).error());
	}
	auto const &ov = *obj_or;
	if (ov.mem_count_ < kHashThreshold) {
		return {};
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto &slot = storage->nodes[ov.node_idx_].hash_idx_raw;
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
		owned = ObjHashTable::create(cap, static_cast<u32>(ov.mem_count_), storage->hash_mr_);
		if (owned == nullptr) {
			stash_failure_sentinel();
			return unexpected(
				JsonError{
					.stage = JsonStage::lookup,
					.code = JsonIssueCode::resource_exhausted,
					.message = "OOM building object hash index"});
		}
		if (!detail::build_table(*owned, storage, ov.mem_start_, ov.mem_count_)) {
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

expected<void, JsonError> Document::warm_member_index(
	NodeRef node) const {
	return warm_member_index_impl(storage_.get(), node);
}

expected<void, JsonError> Document::warm_member_indices(
	WarmIndexOptions const &opts) const {
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
			&& opts.max_extra_bytes != std::numeric_limits<SZ>::max()
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
	UP<DocumentStorage> s) noexcept {
	return Document{move(s)};
}

expected<void, JsonError> ArenaDocument::warm_member_index(
	NodeRef node) const {
	check_live();
	// Arena documents own their storage; this forwards to the same hash-index
	// builder used by Document without transferring ownership.
	return warm_member_index_impl(const_cast<DocumentStorage *>(storage_), node);
}
