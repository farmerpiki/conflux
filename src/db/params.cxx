module;
#include <libpq-fe.h>

export module conflux.db.params;

import std;
import conflux.types;

using namespace std;

namespace conflux::db {

export class Params {
	vector<optional<string>> owned_{};
	vector<int> lengths_{};
	vector<int> formats_{};
	vector<Oid> types_{};
	mutable vector<char const *> values_cache_{};
	mutable bool cache_dirty_{true};

	void rebuild_cache_() const {
		if (!cache_dirty_) {
			return;
		}
		values_cache_.clear();
		values_cache_.reserve(owned_.size());
		for (auto const &o: owned_) {
			values_cache_.push_back(o ? o->c_str() : nullptr);
		}
		cache_dirty_ = false;
	}

public:
	Params() {
		constexpr size_t kCommonSlots = 8;
		owned_.reserve(kCommonSlots);
		lengths_.reserve(kCommonSlots);
		formats_.reserve(kCommonSlots);
		types_.reserve(kCommonSlots);
	}

	Params &add_null() {
		owned_.emplace_back();
		lengths_.push_back(0);
		formats_.push_back(0);
		types_.push_back(0);
		cache_dirty_ = true;
		return *this;
	}

	Params &add(
		string_view v) {
		owned_.emplace_back(string{v});
		lengths_.push_back(static_cast<int>(v.size()));
		formats_.push_back(0);
		types_.push_back(0);
		cache_dirty_ = true;
		return *this;
	}

	Params &add(
		char const *v) {
		return add(string_view{v != nullptr ? v : ""});
	}

	Params &add(
		i64 v) {
		array<char, 24> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		i32 v) {
		array<char, 16> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		double v) {
		array<char, 32> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		bool v) {
		return add(v ? string_view{"t"} : string_view{"f"});
	}
	Params &add_json(
		string_view j) {
		return add(j);
	}

	[[nodiscard]] int count() const noexcept { return static_cast<int>(owned_.size()); }
	[[nodiscard]] Oid const *types() const noexcept { return types_.empty() ? nullptr : types_.data(); }
	// NOLINTNEXTLINE(bugprone-exception-escape) — vector growth in cache rebuild;
	// libpq accessors are documented as noexcept-equivalent.
	[[nodiscard]] char const *const *values() const noexcept {
		rebuild_cache_();
		return values_cache_.empty() ? nullptr : values_cache_.data();
	}
	[[nodiscard]] int const *lengths() const noexcept { return lengths_.empty() ? nullptr : lengths_.data(); }
	[[nodiscard]] int const *formats() const noexcept { return formats_.empty() ? nullptr : formats_.data(); }
	[[nodiscard]] int result_format() const noexcept { return 0; }
};

} // namespace conflux::db
