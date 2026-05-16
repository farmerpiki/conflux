module;
#include <cstdint>

module conflux.net.http_server:state;

import std;
import conflux.types;
import std.compat;

enum class Op : u8 {
	Accept,
	Recv,
	Send,
	Close,
	SsePoll,
	DeferredPoll,
	Shutdown,
	FdShutdown,
	Timer,
	FileIo,
	WsCancel,
	FixedFdInstall,
	DirectSlotClose,
	ClientRing,
	Nop,
	SendZc,

};

struct SendZcCounters : SendZcMetrics {
	[[nodiscard]] SendZcMetrics snapshot() const noexcept {
		return static_cast<SendZcMetrics const &>(*this);
	}
};

enum class ServerFatalReason : u8 {
	none,
	cq_overflow,
	cq_overflow_no_nodrop,
	submit_wait_ebadr,
	internal_exception,

};

inline constexpr u32 OP_SHIFT = 56U;
inline constexpr u32 GEN_SHIFT = 24U;
inline constexpr u64 GEN_MASK = 0xFFFFFFFFULL;
inline constexpr u64 FD_MASK = 0x00FFFFFFULL;
constexpr u64 pack(
	Op op,
	u32 gen,
	int fd) noexcept {
	return (static_cast<u64>(static_cast<u8>(op)) << OP_SHIFT)
		 | ((static_cast<u64>(gen) & GEN_MASK) << GEN_SHIFT)
		 | (static_cast<u64>(static_cast<u32>(fd)) & FD_MASK);
}
constexpr Tup<Op, u32, int> unpack(
	u64 ud) noexcept {
	return {
		static_cast<Op>(ud >> OP_SHIFT),
		static_cast<u32>((ud >> GEN_SHIFT) & GEN_MASK),
		static_cast<int>(ud & FD_MASK)};
}

struct PartialBuf {
	S buf{};
	SZ pos{0};
	[[nodiscard]] inline bool empty() const noexcept { return pos >= buf.size(); }
	[[nodiscard]] inline SZ size() const noexcept { return buf.size() - pos; }
	[[nodiscard]] inline char const *data() const noexcept { return buf.data() + pos; }
	[[nodiscard]] inline char front() const noexcept { return buf[pos]; }
	[[nodiscard]] inline SV view() const noexcept { return {buf.data() + pos, buf.size() - pos}; }
	inline void append(
		char const *p,
		SZ n) {
		buf.append(p, n);
	}
	inline void consume(
		SZ n) noexcept {
		pos += n;
		if (pos >= buf.size()) {
			clear();
		}
	}
	inline void clear() noexcept {
		buf.clear();
		pos = 0;
	}
	[[nodiscard]] inline S take() {
		if (pos > 0) {
			buf.erase(0, pos);
		}
		pos = 0;
		return move(buf);
	}
};

struct RecvComp {
	int fd;
	int res;
	u32 gen;
	u32 flags;
};
inline constexpr SZ FD_TABLE_RESERVE = 4096;
inline constexpr unsigned DEFAULT_RING_ENTRIES = 1024U;
