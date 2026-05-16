module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:ws;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.router;
import conflux.file_map;
import conflux.net.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
import conflux.net.http_server_helpers;
import conflux.net.http_server_config;
import conflux.uring;
import conflux.uring.completion;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.utils;
#if CONFLUX_HAS_HTTP2
import conflux.net.http2;
#endif
#if CONFLUX_HAS_HTTP3
import conflux.net.http3;
#endif
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;

#if CONFLUX_HTTP_TRACE
#define HTTP_TRACE(MSG) eprintln(format("http_trace {}", (MSG)))
#else
#define HTTP_TRACE(MSG) ((void)0)
#endif

[[nodiscard]] bool Ring::make_blocking_fd(
	int fd) {
		// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
		int const flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0) {
			return false;
		}
		return fcntl(fd, F_SETFL, static_cast<int>(static_cast<unsigned>(flags) & ~static_cast<unsigned>(O_NONBLOCK)))
			== 0;
		// NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
	}


[[nodiscard]] WsHandoffState Ring::begin_ws_handoff(
	Conn &conn) {
		auto state = WsHandoffState{move(conn.ws_upgrade), move(conn.ws_work_pool), move(conn.saved_req)};
		++conn.gen;
		conn.fd = -1;
		conn.is_ws = false;
		conn.recv_armed = false;
		conn.send_queued = false;
		return state;
	}


void Ring::launch_plain_ws_handler(
	WorkPool &pool,
	WsHandoffState state,
	int fd,
	S initial_buf) {
		if (!pool.enqueue([state = move(state), fd, ibuf = move(initial_buf)]() mutable {
				WsConn ws{fd, move(ibuf)};
				state.upgrade->handler(state.request, ws);
				::close(fd);
			})) {
			::close(fd);
		}
	}


void Ring::finish_plain_ws_handoff(
	int fd,
	WsInstallEntry entry) {
		if (accepted_sockets_direct) {
			queue_ws_fixed_install(fd, move(entry.state), move(entry.initial_buf));
			return;
		}
		if (!make_blocking_fd(fd)) {
			::close(fd);
			return;
		}
		auto &pool = *entry.state.pool;
		launch_plain_ws_handler(pool, move(entry.state), fd, move(entry.initial_buf));
	}


void Ring::handoff_plain_ws(
	Conn &conn,
	int fd) {
		conn.partial.consume(conn.request_bytes);
		conn.request_bytes = 0;
		S initial_buf = conn.partial.take();
		bool const cancel_recv = conn.recv_armed;
		retire_incremental_partial(fd, conn.gen, conn);
		auto state = begin_ws_handoff(conn);
		if (!state.pool) {
			if (accepted_sockets_direct) {
				if (direct_slots_ && !direct_slots_->mark_closing(static_cast<u32>(fd))) {
					eprintln(format("handoff_plain_ws: mark_closing failed slot={}", fd));
				}
				auto const ud = pack(Op::DirectSlotClose, 0, fd);
				if (!submit_close(raw_, SocketHandle::from_direct(static_cast<u32>(fd)), ud)) {
					defer_op(
						[this, fd, ud] { submit_close(raw_, SocketHandle::from_direct(static_cast<u32>(fd)), ud); });
				}
			} else {
				::close(fd);
			}
			return;
		}
		auto entry = WsInstallEntry{
			move(state),
			move(initial_buf)
#if CONFLUX_HAS_TLS
				,
			nullptr
#endif
		};
		if (cancel_recv) {
			queue_ws_cancel(fd, move(entry));
			return;
		}
		finish_plain_ws_handoff(fd, move(entry));
	}


#if CONFLUX_HAS_TLS
void Ring::launch_tls_ws_handler(
	WorkPool &pool,
	WsHandoffState state,
	int fd,
	SSL *ssl,
	S initial_buf) {
		UniqueSsl owned{ssl};
		if (!pool.enqueue([state = move(state), fd, ssl_owned = move(owned), ibuf = move(initial_buf)]() mutable {
				WsConn ws{fd, ssl_owned.release(), move(ibuf)};
				state.upgrade->handler(state.request, ws);
				::close(fd);
			})) {
			::close(fd);
		}
	}


void Ring::handoff_tls_ws(
	Conn &conn,
	int fd) {
		// Strip the HTTP request bytes — only post-header data belongs in the
		// WS initial buffer (pipelined WS data, if any).
		conn.partial.consume(conn.request_bytes);
		conn.request_bytes = 0;

		S initial_buf = conn.partial.take();
		auto orig_ssl = move(conn.ssl); // transfer ownership to the thread
		bool const cancel_recv = conn.recv_armed;
		retire_incremental_partial(fd, conn.gen, conn);
		auto state = begin_ws_handoff(conn);
		if (!state.pool) {
			orig_ssl.reset();
			if (accepted_sockets_direct) {
				if (direct_slots_ && !direct_slots_->mark_closing(static_cast<u32>(fd))) {
					eprintln(format("handoff_tls_ws: mark_closing failed slot={}", fd));
				}
				auto const ud = pack(Op::DirectSlotClose, 0, fd);
				if (!submit_close(raw_, SocketHandle::from_direct(static_cast<u32>(fd)), ud)) {
					defer_op(
						[this, fd, ud] { submit_close(raw_, SocketHandle::from_direct(static_cast<u32>(fd)), ud); });
				}
			} else {
				::close(fd);
			}
			return;
		}
		auto entry = WsInstallEntry{move(state), move(initial_buf), move(orig_ssl)};
		if (cancel_recv) {
			queue_ws_cancel(fd, move(entry));
			return;
		}
		finish_tls_ws_handoff(fd, move(entry));
	}


#endif
void Ring::queue_ws_cancel(
	int fd,
	WsInstallEntry entry) {
		auto *sqe = get_sqe();
		if (sqe == nullptr) {
			defer_op([this, fd, e = move(entry)]() mutable { queue_ws_cancel(fd, move(e)); });
			return;
		}
		ws_cancel_handoffs.emplace(fd, move(entry));
		auto handle =
			accepted_sockets_direct ? SocketHandle::from_direct(static_cast<u32>(fd)) : SocketHandle::from_os(fd);
		io_uring_prep_cancel_fd(sqe, handle.as_fd(), handle.fixed ? IORING_ASYNC_CANCEL_FD_FIXED : 0);
		io_uring_sqe_set_data64(sqe, pack(Op::WsCancel, 0, fd));
	}


#if CONFLUX_HAS_TLS
void Ring::finish_tls_ws_handoff(
	int fd,
	WsInstallEntry entry) {
		if (accepted_sockets_direct) {
			queue_ws_fixed_install(fd, move(entry.state), move(entry.initial_buf), entry.ssl.release());
			return;
		}
		// Replace memory BIOs with a socket BIO and make fd blocking.
		// TRICKS.md #2 says "DO NOT call SSL_set_fd" for the io_uring path.
		// Here we're exiting that path — blocking I/O is correct for the WS thread.
		SSL_set_fd(entry.ssl.get(), fd); // replaces memory BIOs with socket BIOs
		if (!make_blocking_fd(fd)) {
			entry.ssl.reset();
			::close(fd);
			return;
		}
		auto &pool = *entry.state.pool;
		launch_tls_ws_handler(pool, move(entry.state), fd, entry.ssl.release(), move(entry.initial_buf));
	}


#endif
void Ring::handle_ws_cancel(
	int fd) {
		auto it = ws_cancel_handoffs.find(fd);
		if (it == ws_cancel_handoffs.end()) {
			return;
		}
		ws_cancel_ready.push_back(fd);
	}


void Ring::queue_ws_fixed_install(
	int slot_fd,
	WsHandoffState state,
	S initial_buf
#if CONFLUX_HAS_TLS
	,
	SSL *ssl
#endif
) {
		ws_installs.emplace(
			slot_fd,
			WsInstallEntry{
				move(state),
				move(initial_buf)
#if CONFLUX_HAS_TLS
					,
				UniqueSsl{ssl}
#endif
			});
		if (!submit_fixed_fd_install(raw_, static_cast<u32>(slot_fd), pack(Op::FixedFdInstall, 0, slot_fd))) {
			auto entry = move(ws_installs.at(slot_fd));
			ws_installs.erase(slot_fd);
			defer_op([this,
					  slot_fd,
					  s = move(entry.state),
					  ib = move(entry.initial_buf)
#if CONFLUX_HAS_TLS
						  ,
					  ssl_raw = entry.ssl.release()
#endif
			]() mutable {
				queue_ws_fixed_install(
					slot_fd,
					move(s),
					move(ib)
#if CONFLUX_HAS_TLS
						,
					ssl_raw
#endif
				);
			});
		}
	}


void Ring::handle_fixed_fd_install(
	int slot_fd,
	int real_fd) {
		auto it = ws_installs.find(slot_fd);
		if (it == ws_installs.end()) {
			if (real_fd >= 0) {
				::close(real_fd);
			}
			return;
		}
		auto entry = move(it->second);
		ws_installs.erase(it);

		auto free_slot = [this, slot_fd] {
			if (direct_slots_ && !direct_slots_->mark_closing(static_cast<u32>(slot_fd))) {
				eprintln(format("free_slot: mark_closing failed slot={}", slot_fd));
			}
			if (!submit_close(
					raw_,
					SocketHandle::from_direct(static_cast<u32>(slot_fd)),
					pack(Op::DirectSlotClose, 0, slot_fd))) {
				defer_op([this, slot_fd] {
					submit_close(
						raw_,
						SocketHandle::from_direct(static_cast<u32>(slot_fd)),
						pack(Op::DirectSlotClose, 0, slot_fd));
				});
			}
		};
		free_slot();

		if (real_fd < 0 || !entry.state.pool) {
			if (real_fd >= 0) {
				::close(real_fd);
			}
			return;
		}

#if CONFLUX_HAS_TLS
		if (entry.ssl) {
			SSL_set_fd(entry.ssl.get(), real_fd);
			if (!make_blocking_fd(real_fd)) {
				entry.ssl.reset();
				::close(real_fd);
				return;
			}
			auto &pool = *entry.state.pool;
			launch_tls_ws_handler(pool, move(entry.state), real_fd, entry.ssl.release(), move(entry.initial_buf));
			return;
		}
#endif

		if (!make_blocking_fd(real_fd)) {
			::close(real_fd);
			return;
		}
		auto &pool = *entry.state.pool;
		launch_plain_ws_handler(pool, move(entry.state), real_fd, move(entry.initial_buf));
	}
