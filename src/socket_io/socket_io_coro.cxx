module;
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <exception>
#include <format>
#include <functional>
#include <liburing.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

export module conflux.socket_io.coro;

// Keep this module off `import std` for GCC 16: the exported coroutine/socket
// surface is large enough to trip CMI deserialization with direct std-module
// imports.  Textual std headers in the global module fragment keep the public
// API unchanged while shrinking the CMI dependency surface for importers.
import conflux.types;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.socket_io;
import conflux.work;

namespace wroot = conflux::work::root;
using std::atomic;
using std::atomic_bool;
using std::current_exception;
using std::make_exception_ptr;
using std::make_shared;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_release;
using std::move;
using std::span;
using std::weak_ptr;
template<class T>
using WP = weak_ptr<T>;
// ─── helpers ──────────────────────────────────────────────────────────────────

// Buffer lifetime contract for Task methods:
//   *_borrowed — caller storage is passed to the kernel. It must remain valid
//                until the operation reaches its CQE-backed terminal completion.
//                In normal awaited use, this means until co_await returns.
//                Do not destroy/drop/cancel a borrowed task unless the borrowed
//                storage outlives the underlying io_uring operation.
//   *_copy     — implementation copies input before submission; caller may drop
//                or mutate the source buffer after the call returns.
//   *_owned    — implementation takes ownership by move; no source lifetime
//                obligation remains after the call returns.

export class TcpStream {
	SP<void> state_{};

	[[nodiscard]] wroot::Task<SZ> do_send(u8 const *data, SZ len, SP<void> keeper);

public:
	TcpStream() noexcept;
	explicit TcpStream(SP<void> state) noexcept;
	~TcpStream();
	TcpStream(TcpStream const &) = delete;
	TcpStream &operator =(TcpStream const &) = delete;
	TcpStream(TcpStream &&) noexcept;
	TcpStream &operator =(TcpStream &&) noexcept;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] int raw_fd() const noexcept;

	[[nodiscard]] wroot::Task<SZ> async_recv_borrowed(span<u8> dst);
	[[nodiscard]] wroot::Task<SZ> async_recv_borrowed(span<u8> dst, chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<SZ> recv_borrowed(span<u8> dst);
	[[nodiscard]] wroot::Task<SZ> recv_borrowed(span<u8> dst, chrono::milliseconds timeout);
	[[deprecated("use recv_borrowed")]] [[nodiscard]] wroot::Task<SZ> read_borrowed(span<u8> dst);
	[[nodiscard]] wroot::Task<V<u8>> async_recv_owned(SZ max_bytes);
	[[nodiscard]] wroot::Task<V<u8>> recv_owned(SZ max_bytes);

	[[nodiscard]] wroot::Task<SZ> async_write_borrowed(span<u8 const> src);
	[[nodiscard]] wroot::Task<SZ> async_write_borrowed(span<u8 const> src, chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<void> async_write_all_borrowed(span<u8 const> src, chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<SZ> async_write_copy(span<u8 const> src);
	[[nodiscard]] wroot::Task<SZ> async_write_owned(V<u8> data);
	[[nodiscard]] wroot::Task<SZ> async_write_owned(S data);
	[[nodiscard]] wroot::Task<void> async_write_all_borrowed(span<u8 const> src);
	[[nodiscard]] wroot::Task<void> async_write_all_copy(span<u8 const> src);
	[[nodiscard]] wroot::Task<void> async_write_all_owned(V<u8> data);
	[[nodiscard]] wroot::Task<void> async_write_all_owned(S data);
	[[nodiscard]] wroot::Task<void> async_shutdown(int how = SHUT_WR);
	[[nodiscard]] wroot::Task<void> async_close();

	[[nodiscard]] wroot::Task<SZ> write_borrowed(span<u8 const> src);
	[[nodiscard]] wroot::Task<SZ> write_borrowed(span<u8 const> src, chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<void> write_all_borrowed(span<u8 const> src, chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<SZ> write_copy(span<u8 const> src);
	[[nodiscard]] wroot::Task<SZ> write_owned(V<u8> data);
	[[nodiscard]] wroot::Task<SZ> write_owned(S data);
	[[nodiscard]] wroot::Task<void> write_all_borrowed(span<u8 const> src);
	[[nodiscard]] wroot::Task<void> write_all_copy(span<u8 const> src);
	[[nodiscard]] wroot::Task<void> write_all_owned(V<u8> data);
	[[nodiscard]] wroot::Task<void> write_all_owned(S data);
	[[nodiscard]] wroot::Task<void> shutdown(int how = SHUT_WR);
	[[nodiscard]] wroot::Task<void> close();
};

export [[nodiscard]] wroot::Task<TcpStream> async_tcp_connect(
	SocketTaskRing &ring,
	int family,
	sockaddr_storage addr,
	socklen_t len,
	ConnectOptions opts = {});

export [[nodiscard]] wroot::Task<TcpStream> tcp_connect(
	SocketTaskRing &ring,
	int family,
	sockaddr_storage addr,
	socklen_t len,
	ConnectOptions opts = {});

export [[nodiscard]] wroot::Task<TcpStream> async_tcp_accept(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts = {});

export [[nodiscard]] wroot::Task<TcpStream> tcp_accept(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts = {});

export [[nodiscard]] wroot::Task<void> async_tcp_accept_multishot(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts,
	Fn<wroot::Task<void>(TcpStream)> handler);

export [[nodiscard]] wroot::Task<void> tcp_accept_multishot(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts,
	Fn<wroot::Task<void>(TcpStream)> handler);

export struct UdpRecvResult {
	SZ bytes{0};
	sockaddr_storage from{};
	socklen_t from_len{0};
};

export class UdpSocket {
	SocketTaskRing *ring_{};
	OwnedSocketHandle handle_{};

public:
	UdpSocket() noexcept;
	explicit UdpSocket(SocketTaskRing &ring, OwnedSocketHandle fh) noexcept;
	~UdpSocket();
	UdpSocket(UdpSocket const &) = delete;
	UdpSocket &operator =(UdpSocket const &) = delete;
	UdpSocket(UdpSocket &&) noexcept;
	UdpSocket &operator =(UdpSocket &&) noexcept;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] int raw_fd() const noexcept;
	[[nodiscard]] static UdpSocket ephemeral(SocketTaskRing &ring, int family);

	// payload (span<u8 const>) is NOT copied — caller must keep it valid until co_await returns;
	// if abandoned/detached/cancelled, storage must outlive the underlying io_uring op. Use async_send_to_copy otherwise.
	[[nodiscard]] wroot::Task<SZ> async_send_to_borrowed(span<u8 const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<SZ> async_send_to_copy(span<u8 const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<UdpRecvResult> async_recv_from(span<u8> buf);
	[[nodiscard]] wroot::Task<UdpRecvResult> async_recv_from(span<u8> buf, chrono::milliseconds timeout);

	[[nodiscard]] wroot::Task<SZ> send_to_borrowed(span<u8 const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<SZ> send_to_copy(span<u8 const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<UdpRecvResult> recv_from(span<u8> buf);
	[[nodiscard]] wroot::Task<UdpRecvResult> recv_from(span<u8> buf, chrono::milliseconds timeout);
};

export [[nodiscard]] wroot::Task<void> async_sleep_for(SocketTaskRing &ring, chrono::milliseconds dur);
export [[nodiscard]] wroot::Task<void> sleep_for(SocketTaskRing &ring, chrono::milliseconds dur);
