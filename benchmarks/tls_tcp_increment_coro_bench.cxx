// TLS variant of tcp_increment_coro_bench. Client uses TlsAsyncStream (memory
// BIOs shuttled through io_uring). Server is blocking OpenSSL on a thread
// with SSL_set_fd. Self-signed cert generated in memory at startup.
#include <arpa/inet.h>
#include <charconv>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.net.tls;

import bench_common;

using conflux::work::root::Task;
namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}
struct Config {
	SZ iterations = 50000;
	SZ warmup = 2000;
	bool json_out = false;
};
namespace {

u64 parse_u64(
	char const *s) noexcept {
	SV sv{s};
	u64 v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace
Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = parse_u64(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = parse_u64(args[++i]);
		} else if (a == "--json") {
			cfg.json_out = true;
		} else if (a == "--help" || a == "-h") {
			std::println("Usage: conflux_tls_tcp_increment_coro_bench [--iterations N] [--warmup N] [--json]");
			std::exit(0);
		}
	}
	return cfg;
}
struct KeyCert {
	EVP_PKEY *pkey{nullptr};
	X509 *cert{nullptr};
	~KeyCert() {
		if (pkey != nullptr) {
			EVP_PKEY_free(pkey);
		}
		if (cert != nullptr) {
			X509_free(cert);
		}
	}
};
KeyCert make_self_signed() {
	KeyCert kc;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
	if (pctx == nullptr) {
		throw RE{"EVP_PKEY_CTX_new"};
	}
	if (EVP_PKEY_keygen_init(pctx) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw RE{"keygen_init"};
	}
	if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw RE{"rsa_bits"};
	}
	if (EVP_PKEY_keygen(pctx, &kc.pkey) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw RE{"keygen"};
	}
	EVP_PKEY_CTX_free(pctx);

	kc.cert = X509_new();
	if (kc.cert == nullptr) {
		throw RE{"X509_new"};
	}
	X509_set_version(kc.cert, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(kc.cert), 1);
	X509_gmtime_adj(X509_getm_notBefore(kc.cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(kc.cert), 60L * 60L * 24L * 365L);
	X509_set_pubkey(kc.cert, kc.pkey);
	X509_NAME *name = X509_get_subject_name(kc.cert);
	X509_NAME_add_entry_by_txt(
		name,
		"CN",
		MBSTRING_ASC,
		reinterpret_cast<unsigned char const *>("localhost"),
		-1,
		-1,
		0);
	X509_set_issuer_name(kc.cert, name);
	if (X509_sign(kc.cert, kc.pkey, EVP_sha256()) == 0) {
		throw RE{"X509_sign"};
	}
	return kc;
}
// Blocking TLS server on one accepted client.
void run_server(
	int listen_fd,
	SSL_CTX *sctx,
	atomic_flag &stop) {
	int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
	::close(listen_fd);
	if (cfd < 0) {
		return;
	}
	int one = 1;
	::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	UniqueSsl const ssl{SSL_new(sctx)};
	if (!ssl) {
		::close(cfd);
		return;
	}
	SSL_set_fd(ssl.get(), cfd);
	if (SSL_accept(ssl.get()) != 1) {
		::close(cfd);
		return;
	}

	A<char, 128> buf{};
	SZ held = 0;
	while (!stop.test(memory_order_acquire)) {
		int got = SSL_read(ssl.get(), buf.data() + held, static_cast<int>(buf.size() - held));
		if (got <= 0) {
			break;
		}
		held += static_cast<SZ>(got);
		SZ scan = 0;
		while (scan < held) {
			auto view = span{buf}.subspan(scan, held - scan);
			auto it = ranges::find(view, '\n');
			if (it == view.end()) {
				break;
			}
			SZ const msg_end = scan + static_cast<SZ>(it - view.begin());
			u64 n = 0;
			auto const parsed = from_chars(buf.data() + scan, buf.data() + msg_end, n);
			if (parsed.ec != errc{}) {
				goto done;
			}
			++n;
			A<char, 24> out{};
			auto const conv = to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != errc{}) {
				goto done;
			}
			*conv.ptr = '\n';
			SZ const out_len = static_cast<SZ>(conv.ptr - out.data()) + 1;
			SZ sent = 0;
			while (sent < out_len) {
				int const w = SSL_write(ssl.get(), out.data() + sent, static_cast<int>(out_len - sent));
				if (w <= 0) {
					goto done;
				}
				sent += static_cast<SZ>(w);
			}
			scan = msg_end + 1;
		}
		if (scan > 0) {
			SZ const remain = held - scan;
			memmove(buf.data(), buf.data() + scan, remain);
			held = remain;
		}
		if (held == buf.size()) {
			break;
		}
	}
done:
	SSL_shutdown(ssl.get());
	::close(cfd);
}
int start_listener(
	u16 &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw RE{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw RE{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw RE{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw RE{"listen"};
	}
	return fd;
}
int connect_to(
	u16 port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw RE{"socket"};
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = ::htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw RE{"connect"};
	}
	return fd;
}
SZ encode_line(
	span<char> out,
	u64 n) {
	auto const r = to_chars(out.data(), out.data() + out.size() - 1, n);
	if (r.ec != errc{}) {
		throw RE{"to_chars"};
	}
	*r.ptr = '\n';
	return static_cast<SZ>(r.ptr - out.data()) + 1;
}
u64 decode_line(
	SV line) {
	u64 n = 0;
	auto const r = from_chars(line.data(), line.data() + line.size(), n);
	if (r.ec != errc{}) {
		throw RE{"from_chars"};
	}
	return n;
}
struct AsyncTlsLineReader {
	TlsAsyncStream &tls;
	A<byte, 256> buf{};
	SZ held = 0;
	Task<SV> read_line() {
		for (;;) {
			auto view = span{buf}.first(held);
			auto it = ranges::find(view, static_cast<byte>('\n'));
			if (it != view.end()) {
				auto const end = static_cast<SZ>(it - view.begin());
				co_return SV{reinterpret_cast<char const *>(buf.data()), end};
			}
			auto got = co_await tls.read_some(span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				throw RE{"tls eof"};
			}
			held += got;
		}
	}
	void consume_line(
		SZ line_len) {
		SZ const drop = line_len + 1;
		if (drop >= held) {
			held = 0;
		} else {
			memmove(buf.data(), buf.data() + drop, held - drop);
			held -= drop;
		}
	}
};
u64 run_callback(
	FileReader &files,
	TlsAsyncStream &tls,
	SZ iters,
	u64 start) {
	AsyncTlsLineReader reader{.tls = tls};
	A<char, 24> out{};
	u64 n = start;
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		SZ const len = encode_line(out, n);
		block_on(files, tls.write_all(as_bytes(span{out.data(), len})));
		auto line = block_on(files, reader.read_line());
		u64 const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw RE{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}
Task<u64> coro_loop(
	TlsAsyncStream &tls,
	SZ iters,
	u64 start) {
	AsyncTlsLineReader reader{.tls = tls};
	A<char, 24> out{};
	u64 n = start;
	for (SZ i = 0; i < iters; ++i) {
		SZ const len = encode_line(out, n);
		co_await tls.write_all(as_bytes(span{out.data(), len}));
		auto line = co_await reader.read_line();
		u64 const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw RE{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	co_return n;
}
u64 run_coroutine(
	FileReader &files,
	TlsAsyncStream &tls,
	SZ iters,
	u64 start) {
	auto const t0 = chrono::steady_clock::now();
	(void)block_on(files, coro_loop(tls, iters, start));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

} // namespace
int main(
	int argc,
	char **argv) try {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"tls_tcp_increment_coro","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","10000","--warmup","500"]}]})");
	auto cfg = parse_args(span{argv, static_cast<SZ>(argc)});

	auto kc = make_self_signed();

	UniqueSslCtx const sctx{SSL_CTX_new(TLS_server_method())};
	if (!sctx) {
		std::println(std::cerr, "SSL_CTX_new server failed");
		return 1;
	}
	SSL_CTX_use_certificate(sctx.get(), kc.cert);
	SSL_CTX_use_PrivateKey(sctx.get(), kc.pkey);

	for (int which = 0; which < 2; ++which) {
		u16 port = 0;
		int const lfd = start_listener(port);
		atomic_flag server_stop{};
		thread server{[lfd, sctx_raw = sctx.get(), &server_stop] { run_server(lfd, sctx_raw, server_stop); }};

		int const csock = connect_to(port);

		::io_uring ring{};
		if (::io_uring_queue_init(64, &ring, 0) < 0) {
			::close(csock);
			server_stop.test_and_set(memory_order_release);
			server.join();
			std::println(std::cerr, "io_uring_queue_init failed");
			return 1;
		}
		CompletionTable ct;
		FileReader files{&ring, &ct, pack_ud};

		TlsContext cctx;
		cctx.set_verify_peer(false);
		FileHandle sock = FileHandle::from_fd(csock);
		TlsAsyncStream tls{cctx, files, move(sock)};
		(void)tls.set_server_name("localhost");

		try {
			block_on(files, tls.handshake_connect());

			(void)run_callback(files, tls, cfg.warmup, 0);
			u64 const ns = (which == 0) ? run_callback(files, tls, cfg.iterations, cfg.warmup) :
										  run_coroutine(files, tls, cfg.iterations, cfg.warmup);
			double const per = static_cast<double>(ns) / static_cast<double>(cfg.iterations);
			SV const label = (which == 0) ? "callback" : "coroutine";
			if (cfg.json_out) {
				std::println(
					"{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:."
					"2f}}}",
					label,
					cfg.iterations,
					ns,
					per);
			} else {
				if (which == 0) {
					std::println("iterations: {}, warmup: {}", cfg.iterations, cfg.warmup);
				}
				std::println("  {:<10} {:>8.1f} ns/iter ({} ns total)", label, per, ns);
			}
		} catch (exception const &e) { std::println(std::cerr, "error: {}", e.what()); }

		::io_uring_queue_exit(&ring);
		server_stop.test_and_set(memory_order_release);
		int const raw = tls.handle().raw_fd();
		::shutdown(raw, SHUT_RDWR);
		server.join();
	}
} catch (exception const &e) {
	std::println(std::cerr, "fatal: {}", e.what());
	return 1;
}
