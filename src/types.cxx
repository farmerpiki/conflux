module;
#include <memory>

export module conflux.types;

import std;

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
export namespace ranges = std::ranges;
export namespace views = std::views;
export namespace fs = std::filesystem;
export using std::current_exception;
export using std::make_exception_ptr;
export using std::make_pair;
export using std::rethrow_exception;
export using std::invoke;
export using std::hash;
export using std::system_category;
export using std::generic_category;
export using std::weak_ptr;
export struct IoError final : std::system_error {
	IoError(
		int err,
		std::string const &what)
		: std::system_error{err, generic_category(), what} {}
};
