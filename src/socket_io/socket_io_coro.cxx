module;
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

export module conflux.socket_io.coro;

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.socket_io;
import conflux.work;

namespace wroot = conflux::work::root;
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
	std::shared_ptr<void> state_{};

	[[nodiscard]] wroot::Task<std::size_t>
	do_send(std::uint8_t const *data, std::size_t len, std::shared_ptr<void> keeper);

public:
	TcpStream() noexcept;
	explicit TcpStream(std::shared_ptr<void> state) noexcept;
	~TcpStream();
	TcpStream(TcpStream const &) = delete;
	TcpStream &operator =(TcpStream const &) = delete;
	TcpStream(TcpStream &&) noexcept;
	TcpStream &operator =(TcpStream &&) noexcept;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] int raw_fd() const noexcept;

	[[nodiscard]] wroot::Task<std::size_t> async_recv_borrowed(std::span<std::uint8_t> dst);
	[[nodiscard]] wroot::Task<std::size_t>
	async_recv_borrowed(std::span<std::uint8_t> dst, std::chrono::milliseconds timeout);
	[[deprecated("use async_recv_borrowed")]] [[nodiscard]] wroot::Task<std::size_t>
	read_borrowed(std::span<std::uint8_t> dst);
	[[nodiscard]] wroot::Task<std::vector<std::uint8_t>> async_recv_owned(std::size_t max_bytes);

	[[nodiscard]] wroot::Task<std::size_t> async_write_borrowed(std::span<std::uint8_t const> src);
	[[nodiscard]] wroot::Task<std::size_t>
	async_write_borrowed(std::span<std::uint8_t const> src, std::chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<void>
	async_write_all_borrowed(std::span<std::uint8_t const> src, std::chrono::milliseconds timeout);
	[[nodiscard]] wroot::Task<std::size_t> async_write_copy(std::span<std::uint8_t const> src);
	[[nodiscard]] wroot::Task<std::size_t> async_write_owned(std::vector<std::uint8_t> data);
	[[nodiscard]] wroot::Task<std::size_t> async_write_owned(std::string data);
	[[nodiscard]] wroot::Task<void> async_write_all_borrowed(std::span<std::uint8_t const> src);
	[[nodiscard]] wroot::Task<void> async_write_all_copy(std::span<std::uint8_t const> src);
	[[nodiscard]] wroot::Task<void> async_write_all_owned(std::vector<std::uint8_t> data);
	[[nodiscard]] wroot::Task<void> async_write_all_owned(std::string data);
	[[nodiscard]] wroot::Task<void> async_shutdown(int how = SHUT_WR);
	[[nodiscard]] wroot::Task<void> async_close();
};

export [[nodiscard]] wroot::Task<TcpStream>
async_tcp_connect(SocketTaskRing &ring, int family, sockaddr_storage addr, socklen_t len, ConnectOptions opts = {});

export [[nodiscard]] wroot::Task<TcpStream>
async_tcp_accept(TcpListener &listener, SocketTaskRing &ring, AcceptOptions opts = {});

export [[nodiscard]] wroot::Task<void> async_tcp_accept_multishot(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts,
	std::function<wroot::Task<void>(TcpStream)> handler);

export struct UdpRecvResult {
	std::size_t bytes{0};
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

	// payload (std::span<std::uint8_t const>) is NOT copied — caller must keep it valid until co_await returns;
	// if abandoned/detached/cancelled, storage must outlive the underlying io_uring op. Use async_send_to_copy
	// otherwise.
	[[nodiscard]] wroot::Task<std::size_t>
	async_send_to_borrowed(std::span<std::uint8_t const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<std::size_t>
	async_send_to_copy(std::span<std::uint8_t const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<UdpRecvResult> async_recv_from(std::span<std::uint8_t> buf);
	[[nodiscard]] wroot::Task<UdpRecvResult>
	async_recv_from(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout);

	[[nodiscard]] wroot::Task<std::size_t>
	send_to_borrowed(std::span<std::uint8_t const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<std::size_t>
	send_to_copy(std::span<std::uint8_t const> data, sockaddr_storage addr, socklen_t addr_len);
	[[nodiscard]] wroot::Task<UdpRecvResult> recv_from(std::span<std::uint8_t> buf);
	[[nodiscard]] wroot::Task<UdpRecvResult> recv_from(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout);
};

export [[nodiscard]] wroot::Task<void> async_sleep_for(SocketTaskRing &ring, std::chrono::milliseconds dur);
