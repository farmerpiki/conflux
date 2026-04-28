module;
#include <cstdint>

export module conflux.types;

import std;

export using i8 = std::int8_t;
export using i16 = std::int16_t;
export using i32 = std::int32_t;
export using i64 = std::int64_t;

export using u8 = std::uint8_t;
export using u16 = std::uint16_t;
export using u32 = std::uint32_t;
export using u64 = std::uint64_t;

export using S  = std::string;
export using SV = std::string_view;
export using SZ = std::size_t;

export template <typename T>
using V = std::vector<T>;

export template <typename T, std::size_t N>
using A = std::array<T, N>;

export template <typename K, typename T>
using M = std::map<K, T>;

export template <typename K, typename T>
using UM = std::unordered_map<K, T>;

export template <typename T>
using SP = std::shared_ptr<T>;

export template <typename T>
using UP = std::unique_ptr<T>;

export template <typename T1, typename T2>
using P = std::pair<T1, T2>;

export template <typename T>
using Opt = std::optional<T>;

export template <typename T>
using Fn = std::function<T>;

export template <typename... Ts>
using Tup = std::tuple<Ts...>;
