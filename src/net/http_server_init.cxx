module;
#include <cstddef>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:init;

import std;
import std.compat;
import conflux.net.detail.direct_slot_pool;
import conflux.net.http_server_config;
import conflux.uring;
import conflux.uring.completion;
import conflux.file_io;
import conflux.socket_io;
import :state;

using namespace conflux::socket_io;
using conflux::uring::CompletionTable;
using conflux::uring::DirectFd;
using conflux::uring::UserDataFn;

void Ring::init_uring_queue(
	unsigned entries,
	std::uint32_t uring_flags,
	std::uint32_t wq_fd,
	bool no_mmap) {
	io_uring_params params{};
	params.flags = uring_flags;
	if (wq_fd != 0) {
		params.flags = (conflux::uring::SetupFlags{params.flags} | conflux::uring::setup_flags::attach_wq).raw();
		params.wq_fd = wq_fd;
	}
	requested_setup_flags_ = params.flags;
	active_setup_flags_ = 0;
	stripped_setup_flags_ = 0;
	if (no_mmap) {
		ssize_t const sz = io_uring_mlock_size(entries, params.flags);
		if (sz <= 0) {
			throw std::runtime_error{"io_uring_mlock_size failed"};
		}
		auto *raw = static_cast<std::byte *>(
			std::aligned_alloc(static_cast<std::size_t>(sysconf(_SC_PAGESIZE)), static_cast<std::size_t>(sz)));
		if (raw == nullptr) {
			throw std::bad_alloc{};
		}
		ring_mem = std::unique_ptr<std::byte[], void (*)(void *)>{raw, std::free};
		if (io_uring_queue_init_mem(entries, &ring, &params, raw, static_cast<std::size_t>(sz)) < 0) {
			throw std::runtime_error{"io_uring_queue_init_mem failed"};
		}
		active_setup_flags_ = params.flags;
	} else {
		for (;;) {
			int const rc = ::io_uring_queue_init_params(entries, &ring, &params);
			if (rc == 0) {
				active_setup_flags_ = params.flags;
				break;
			}
			if (rc != -EINVAL) {
				throw std::runtime_error{std::format("io_uring_queue_init_params: {}", strerror(-rc))};
			}
			auto const stripped = conflux::http::detail::next_uring_setup_flag_to_strip(params.flags);
			if (!stripped) {
				throw std::runtime_error{"io_uring_queue_init_params: no supported flag combination"};
			}
			params.flags = (conflux::uring::SetupFlags{params.flags} & ~*stripped).raw();
			stripped_setup_flags_ = (conflux::uring::SetupFlags{stripped_setup_flags_} | *stripped).raw();
		}
	}
}

void Ring::detect_ring_capabilities() {
	caps = detect_caps(conflux::uring::RingRef{ring});
	use_recv_bundle =
		use_recv_bundle && !use_recv_incremental_buf && caps.recvsend_bundle && CONFLUX_ENABLE_RECV_BUNDLE;
}

void Ring::init_client_task_ring() {
	client_task_ring_.emplace(
		SocketRawRing{ring},
		client_ct_,
		UserDataFn{[](std::uint32_t slot, std::uint32_t gen) noexcept -> std::uint64_t {
			return pack(Op::ClientRing, gen, static_cast<int>(slot));
		}});
}

void Ring::open_listen_socket(
	std::uint16_t port) {
	listen_fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		throw std::system_error{errno, std::system_category(), "socket"};
	}

	int opt = 1;
	int v6only = 0;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	::setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	::setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons(port);

	if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		throw std::system_error{errno, std::system_category(), "bind"};
	}
	if (::listen(listen_fd, SOMAXCONN) < 0) {
		throw std::system_error{errno, std::system_category(), "listen"};
	}
}

void Ring::publish_bound_port() {
	sockaddr_in6 local{};
	socklen_t llen = sizeof(local);
	if (getsockname(listen_fd, reinterpret_cast<sockaddr *>(&local), &llen) == 0) {
		bound_port = ntohs(local.sin6_port);
	}
	if (port_signal != nullptr) {
		port_signal->store(bound_port, std::memory_order_release);
		port_signal->notify_all();
	}
}

void Ring::install_listen_direct_fd() {
	direct_fds_ = std::make_unique<DirectFdTable>(conflux::uring::RingRef{ring}, MAX_FILES);
	if (direct_fds_->registered() && direct_fds_->install(static_cast<std::uint32_t>(listen_fd), listen_fd)) {
		listen_fixed = true;
		caps.socket_direct_alloc = caps.op_socket && direct_fds_->registered();
		direct_slots_ = std::make_unique<conflux::net::detail::DirectSlotPool>(direct_fds_->capacity());
		if (!direct_slots_->install_os_fd(static_cast<std::uint32_t>(listen_fd), listen_fd)) {
			direct_slots_.reset();
			listen_fixed = false;
			caps.socket_direct_alloc = false;
		}
	}
	accepted_sockets_direct = direct_accept_enabled_ && listen_fixed && caps.accept_direct_supported;
}

void Ring::init_file_io_pools() {
	if (file_io_slabs > 0 && file_io_pipe_pairs > 0) {
		auto const total_buf_slots =
			static_cast<unsigned>(file_io_slabs + (send_fixed_buffers_enabled ? send_buffer_slabs : std::size_t{0}));
		auto table = std::make_unique<conflux::file_io::RegisteredBufferTable>(&ring, total_buf_slots);
		if (table->ok()) {
			auto file_pool =
				std::make_unique<conflux::file_io::FixedBufferPool>(table.get(), 0, file_io_slabs, file_io_slab_bytes);
			auto pipes = std::make_unique<conflux::file_io::PipePool>(file_io_pipe_pairs);
			if (file_pool->ok() && file_pool->capacity() > 0 && pipes->capacity() > 0) {
				file_completions = std::make_unique<CompletionTable>();
				buf_table = std::move(table);
				fixed_buffers = std::move(file_pool);
				splice_pipes = std::move(pipes);
				files = std::make_unique<conflux::file_io::FileReader>(
					&ring,
					file_completions.get(),
					[](std::uint32_t slot, std::uint32_t gen) noexcept {
						return pack(Op::FileIo, gen, static_cast<int>(slot));
					});
				if (send_fixed_buffers_enabled && send_buffer_slabs > 0) {
					auto sp = std::make_unique<conflux::file_io::FixedBufferPool>(
						buf_table.get(),
						static_cast<unsigned>(file_io_slabs),
						send_buffer_slabs,
						send_buffer_bytes);
					if (sp->ok() && sp->capacity() > 0) {
						send_buffers = std::move(sp);
						send_fixed_buffers_supported = true;
					}
				}
			}
		}
	}
}

void Ring::init_recv_buffer_ring(
	unsigned entries) {
	buf_ring_ = std::make_unique<BufferRing>(
		conflux::uring::RingRef{ring},
		BufferRingOptions{
			.count = entries * 4,
			.buf_size = BUF_SIZE,
			.group_id = 0,
			.huge_pages = true,
			.mode = use_recv_incremental_buf ? BufferRingMode::incremental :
					use_recv_bundle          ? BufferRingMode::recv_bundle :
											   BufferRingMode::classic_one_cqe_per_buffer,
		},
		caps);
}

void Ring::reserve_ring_tables(
	unsigned entries) {
	fd_table.reserve(FD_TABLE_RESERVE);
	recvs.reserve(entries);
}

void Ring::init(
	std::uint16_t port,
	unsigned entries,
	std::uint32_t uring_flags,
	std::uint32_t wq_fd,
	bool no_mmap) {
	init_uring_queue(entries, uring_flags, wq_fd, no_mmap);
	detect_ring_capabilities();
	init_client_task_ring();
	open_listen_socket(port);
	publish_bound_port();
	install_listen_direct_fd();
	init_file_io_pools();
	init_recv_buffer_ring(entries);
	reserve_ring_tables(entries);
}
