module;
#include <libpq-fe.h>

export module conflux.db.params;

import std;
import conflux.types;

using std::array;
using std::bit_cast;
using std::copy_n;
using std::int32_t;
using std::int64_t;
using std::ptrdiff_t;
using std::size_t;
using std::string_view;
using std::to_chars;
using std::uint32_t;
using std::uint64_t;
using std::vector;
export namespace conflux::db::oids {

inline constexpr Oid bool_ = 16;
inline constexpr Oid bytea = 17;
inline constexpr Oid int8 = 20;
inline constexpr Oid int4 = 23;
inline constexpr Oid text = 25;
inline constexpr Oid float8 = 701;

} // namespace conflux::db::oids

export namespace conflux::db {

class Params {
	static constexpr size_t kInline = 8;

	// SoA inline metadata for ≤ kInline params
	array<ptrdiff_t, kInline> ioff_{};
	array<int, kInline> ilen_{};
	array<int, kInline> ifmt_{};
	array<Oid, kInline> ioid_{};

	// Overflow metadata (populated from 0..N-1 when count_ > kInline)
	vector<ptrdiff_t> voff_{};
	vector<int> vlen_{};
	vector<int> vfmt_{};
	vector<Oid> void_{};

	// Contiguous data arena: text bytes (with NUL) + binary values
	vector<char> arena_{};

	// Pointer table (rebuilt from arena offsets at values() time)
	mutable array<char const *, kInline> pvi_{};
	mutable vector<char const *> pvv_{};
	mutable bool dirty_{true};

	size_t count_{0};
	int result_fmt_{0};
	// ---- internal helpers ----------------------------------------

	ptrdiff_t push_text_(
		string_view v) {
		auto const off = static_cast<ptrdiff_t>(arena_.size());
		arena_.resize(arena_.size() + v.size() + 1);
		std::copy_n(v.data(), v.size(), arena_.data() + off);
		arena_[static_cast<size_t>(off) + v.size()] = '\0';
		dirty_ = true;
		return off;
	}
	ptrdiff_t push_bytes_(
		void const *data,
		size_t n) {
		auto const off = static_cast<ptrdiff_t>(arena_.size());
		arena_.resize(arena_.size() + n);
		std::copy_n(static_cast<char const *>(data), n, arena_.data() + off);
		dirty_ = true;
		return off;
	}
	void spill_() {
		voff_.reserve(kInline + 1);
		vlen_.reserve(kInline + 1);
		vfmt_.reserve(kInline + 1);
		void_.reserve(kInline + 1);
		for (size_t i = 0; i < kInline; ++i) {
			voff_.push_back(ioff_[i]);
			vlen_.push_back(ilen_[i]);
			vfmt_.push_back(ifmt_[i]);
			void_.push_back(ioid_[i]);
		}
	}
	void push_(
		ptrdiff_t off,
		int len,
		int fmt,
		Oid oid) {
		if (count_ < kInline) {
			ioff_[count_] = off;
			ilen_[count_] = len;
			ifmt_[count_] = fmt;
			ioid_[count_] = oid;
		} else {
			if (voff_.empty()) {
				spill_();
			}
			voff_.push_back(off);
			vlen_.push_back(len);
			vfmt_.push_back(fmt);
			void_.push_back(oid);
		}
		++count_;
		dirty_ = true;
	}
	template<class T>
	static T to_net_(
		T v) noexcept {
		if constexpr (endian::native == endian::little) {
			return byteswap(v);
		}
		return v;
	}
	// NOLINTNEXTLINE(bugprone-exception-escape) — vector ops; libpq accessors documented as noexcept-equivalent.
	void rebuild_() const noexcept {
		if (!dirty_) {
			return;
		}
		auto const *base = arena_.data();
		auto resolve = [base](ptrdiff_t off) noexcept -> char const * { return off < 0 ? nullptr : base + off; };
		if (count_ <= kInline) {
			for (size_t i = 0; i < count_; ++i) {
				pvi_[i] = resolve(ioff_[i]);
			}
		} else {
			pvv_.resize(count_);
			for (size_t i = 0; i < count_; ++i) {
				pvv_[i] = resolve(voff_[i]);
			}
		}
		dirty_ = false;
	}

public:
	Params() = default;
	Params(
		Params const &o)
		: ioff_{o.ioff_}
		, ilen_{o.ilen_}
		, ifmt_{o.ifmt_}
		, ioid_{o.ioid_}
		, voff_{o.voff_}
		, vlen_{o.vlen_}
		, vfmt_{o.vfmt_}
		, void_{o.void_}
		, arena_{o.arena_}
		, count_{o.count_}
		, result_fmt_{o.result_fmt_} {}
	Params &operator =(
		Params const &o) {
		if (this != &o) {
			ioff_ = o.ioff_;
			ilen_ = o.ilen_;
			ifmt_ = o.ifmt_;
			ioid_ = o.ioid_;
			voff_ = o.voff_;
			vlen_ = o.vlen_;
			vfmt_ = o.vfmt_;
			void_ = o.void_;
			arena_ = o.arena_;
			count_ = o.count_;
			result_fmt_ = o.result_fmt_;
			dirty_ = true;
		}
		return *this;
	}
	Params(Params &&) noexcept;
	Params &operator =(Params &&) noexcept;
	// ---- text bind -----------------------------------------------

	Params &add_null() {
		push_(-1, 0, 0, 0);
		return *this;
	}
	Params &add(
		string_view v) {
		auto const off = push_text_(v);
		push_(off, static_cast<int>(v.size()), 0, 0);
		return *this;
	}
	Params &add(
		char const *v) {
		return add(string_view{v != nullptr ? v : ""});
	}
	Params &add(
		std::int64_t v) {
		array<char, 24> buf{};
		auto [p, _] = to_chars(buf.data(), buf.data() + buf.size(), v);
		return add(string_view{buf.data(), static_cast<size_t>(p - buf.data())});
	}
	Params &add(
		std::int32_t v) {
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
	// ---- binary bind ---------------------------------------------

	Params &add_binary(
		std::int64_t v,
		Oid oid = conflux::db::oids::int8) {
		auto const nv = to_net_(static_cast<uint64_t>(v));
		auto const off = push_bytes_(&nv, sizeof(nv));
		push_(off, static_cast<int>(sizeof(nv)), 1, oid);
		return *this;
	}
	Params &add_binary(
		std::int32_t v,
		Oid oid = conflux::db::oids::int4) {
		auto const nv = to_net_(static_cast<uint32_t>(v));
		auto const off = push_bytes_(&nv, sizeof(nv));
		push_(off, static_cast<int>(sizeof(nv)), 1, oid);
		return *this;
	}
	Params &add_binary(
		double v,
		Oid oid = conflux::db::oids::float8) {
		auto const nv = to_net_(bit_cast<uint64_t>(v));
		auto const off = push_bytes_(&nv, sizeof(nv));
		push_(off, static_cast<int>(sizeof(nv)), 1, oid);
		return *this;
	}
	Params &add_binary(
		span<byte const> bytes,
		Oid oid = conflux::db::oids::bytea) {
		auto const off = push_bytes_(bytes.data(), bytes.size());
		push_(off, static_cast<int>(bytes.size()), 1, oid);
		return *this;
	}
	// ---- result format -------------------------------------------

	Params &result_format(
		int fmt) noexcept {
		result_fmt_ = fmt;
		return *this;
	}
	// ---- libpq accessors -----------------------------------------

	[[nodiscard]] int count() const noexcept { return static_cast<int>(count_); }
	// NOLINTNEXTLINE(bugprone-exception-escape)
	[[nodiscard]] char const *const *values() const noexcept {
		rebuild_();
		if (count_ == 0) {
			return nullptr;
		}
		return count_ <= kInline ? pvi_.data() : pvv_.data();
	}
	[[nodiscard]] int const *lengths() const noexcept {
		if (count_ == 0) {
			return nullptr;
		}
		return count_ <= kInline ? ilen_.data() : vlen_.data();
	}
	[[nodiscard]] int const *formats() const noexcept {
		if (count_ == 0) {
			return nullptr;
		}
		return count_ <= kInline ? ifmt_.data() : vfmt_.data();
	}
	[[nodiscard]] Oid const *types() const noexcept {
		if (count_ == 0) {
			return nullptr;
		}
		return count_ <= kInline ? ioid_.data() : void_.data();
	}
	[[nodiscard]] int result_format() const noexcept { return result_fmt_; }
};
Params::Params(Params &&) noexcept = default;
Params &Params::operator =(Params &&) noexcept = default;

} // namespace conflux::db
