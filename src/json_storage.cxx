module conflux.json;

import std;
import std.compat;
import conflux.types;

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
