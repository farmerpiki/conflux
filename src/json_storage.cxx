module conflux.json;

import std;
import std.compat;
import conflux.types;

DocumentStorage::~DocumentStorage() noexcept {
	for (auto &n: nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
		}
	}
}

std::string_view DocumentStorage::str_at(
	std::uint32_t off,
	std::uint32_t len) const noexcept {
	return {string_arena.data() + off, len};
}

std::string_view DocumentStorage::bytes_at(
	std::uint32_t off,
	std::uint32_t len,
	std::uint8_t flags) const noexcept {
	if ((flags & kValueExternalView) != 0) {
		return {external_ptrs_[off], len};
	}
	if ((flags & kStorageInputView) != 0) {
		return input_view.substr(off, len);
	}
	return str_at(off, len);
}

std::string_view DocumentStorage::member_name(
	MemberEntry const &m) const noexcept {
	if ((m.name_flags & kMemberExternalView) != 0) {
		return {external_ptrs_[m.name_off], m.name_len};
	}
	return bytes_at(m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags));
}

ObjHashTable *ObjHashTable::create(
	std::uint32_t capacity,
	std::uint32_t member_count,
	std::pmr::memory_resource *mr) noexcept {
	std::size_t const bytes =
		sizeof(ObjHashTable) + sizeof(ObjHashSlot) * capacity + sizeof(char const *) * member_count;
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

void ObjHashTable::destroy(
	ObjHashTable *t) noexcept {
	if (t == nullptr) {
		return;
	}
	auto *mr = t->mr;
	std::size_t const bytes =
		sizeof(ObjHashTable) + sizeof(ObjHashSlot) * t->capacity + sizeof(char const *) * t->member_count;
	t->~ObjHashTable();
	mr->deallocate(t, bytes, alignof(ObjHashTable));
}
