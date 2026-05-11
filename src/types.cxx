module;
#include<iostream>
#include<memory>

export module conflux.types;

import std;

export using i8=std::int8_t;
export using i16=std::int16_t;
export using i32=std::int32_t;
export using i64=std::int64_t;

export using u8=std::uint8_t;
export using u16=std::uint16_t;
export using u32=std::uint32_t;
export using u64=std::uint64_t;

export using S=std::string;
export using SV=std::string_view;
export using SZ=std::size_t;

export template<typename T>
using V=std::vector<T>;

export template<typename T,std::size_t N>
using A=std::array<T,N>;

export template<typename K,typename T>
using M=std::map<K,T>;

export template<typename K,typename T>
using UM=std::unordered_map<K,T>;

export template<typename T>
using SP=std::shared_ptr<T>;

export template<typename T,typename D>
using UPD=std::unique_ptr<T,D>;

export template<typename T>
using UP=std::unique_ptr<T>;

export template<typename T1,typename T2>
using P=std::pair<T1,T2>;

export template<typename T>
using Opt=std::optional<T>;

export template<typename T>
using Fn=std::function<T>;

export template<SZ i,typename Tp>
using TEt=std::tuple_element_t<i,Tp>;

export template<typename...Ts>
using Tup=std::tuple<Ts...>;

export using EP=std::exception_ptr;
export using RE=std::runtime_error;
export using SE=std::system_error;
export using EC=std::error_code;
export using LE=std::logic_error;

export template<typename T>
using Atom=std::atomic<T>;

export template<typename...Ms>
using SL=std::scoped_lock<Ms...>;

export template<typename T>
using NL=std::numeric_limits<T>;

export namespace chrono=std::chrono;

export using std::span;
export using std::mutex;
export using std::lock_guard;
export using std::variant;
export using std::expected;
export using std::move;
export using std::forward;
export using std::exchange;
export using std::make_shared;
export using std::make_unique;
export using std::nullopt;
export using std::to_string;
export using std::println;
export using std::cerr;
export using std::exception;
export using std::same_as;
export using std::byte;
export using std::max;
export using std::min;
export using std::thread;
export using std::jthread;
export using std::atomic_flag;
export using std::deque;
export using std::barrier;
export using std::memory_order_release;
export using std::memory_order_acquire;
export using std::memory_order_relaxed;
export using std::memory_order_seq_cst;
export using std::memory_order_acq_rel;
export using std::unexpected;
export using std::format;
export using std::from_chars;
export using std::to_chars;
export using std::errc;
export using std::isfinite;
export using std::isinf;
export namespace ranges=std::ranges;
export namespace views=std::views;
export namespace fs=std::filesystem;
export using std::current_exception;
export using std::make_exception_ptr;
export using std::make_pair;
export using std::rethrow_exception;
export using std::invoke;
export using std::hash;
export using std::system_category;
export using std::generic_category;
export using std::weak_ptr;
export void eprintln(
SV message){
static mutex mu;
SL const lk{mu};
std::println(std::cerr,"{}",message);
}
export template<typename T>
using US=std::unordered_set<T>;
export struct IoError final:SE{
IoError(
int err,
S const&what)
:SE{err,generic_category(),what}{}
};
