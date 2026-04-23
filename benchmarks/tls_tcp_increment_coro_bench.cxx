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
import conflux.work;
import conflux.file_io;
import conflux.net.tls;

using namespace std;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct Config {
	size_t iterations = 50'000;
	size_t warmup = 2'000;
	bool csv = false;
};

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = stoull(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = stoull(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_tls_tcp_increment_coro_bench [--iterations N] [--warmup N] [--csv]");
			exit(0);
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
		throw runtime_error{"EVP_PKEY_CTX_new"};
	}
	if (EVP_PKEY_keygen_init(pctx) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw runtime_error{"keygen_init"};
	}
	if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw runtime_error{"rsa_bits"};
	}
	if (EVP_PKEY_keygen(pctx, &kc.pkey) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw runtime_error{"keygen"};
	}
	EVP_PKEY_CTX_free(pctx);

	kc.cert = X509_new();
	if (kc.cert == nullptr) {
		throw runtime_error{"X509_new"};
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
		throw runtime_error{"X509_sign"};
	}
	return kc;
}

// Blocking TLS server on one accepted client.
void run_server(
	int listen_fd,
	SSL_CTX *sctx,
	atomic_flag &stop) {
	int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
	if (cfd < 0) {
		return;
	}
	int one = 1;
	::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	SSL *ssl = SSL_new(sctx);
	if (ssl == nullptr) {
		::close(cfd);
		return;
	}
	SSL_set_fd(ssl, cfd);
	if (SSL_accept(ssl) != 1) {
		SSL_free(ssl);
		::close(cfd);
		return;
	}

	array<char, 128> buf{};
	size_t held = 0;
	while (!stop.test(memory_order_acquire)) {
		int got = SSL_read(ssl, buf.data() + held, static_cast<int>(buf.size() - held));
		if (got <= 0) {
			break;
		}
		held += static_cast<size_t>(got);
		size_t scan = 0;
		while (scan < held) {
			auto it =
				find(buf.begin() + static_cast<ptrdiff_t>(scan), buf.begin() + static_cast<ptrdiff_t>(held), '\n');
			if (it == buf.begin() + static_cast<ptrdiff_t>(held)) {
				break;
			}
			size_t const msg_end = static_cast<size_t>(it - buf.begin());
			uint64_t n = 0;
			auto const parsed = from_chars(buf.data() + scan, buf.data() + msg_end, n);
			if (parsed.ec != errc{}) {
				goto done;
			}
			++n;
			array<char, 24> out{};
			auto const conv = to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != errc{}) {
				goto done;
			}
			*conv.ptr = '\n';
			size_t const out_len = static_cast<size_t>(conv.ptr - out.data()) + 1;
			size_t sent = 0;
			while (sent < out_len) {
				int const w = SSL_write(ssl, out.data() + sent, static_cast<int>(out_len - sent));
				if (w <= 0) {
					goto done;
				}
				sent += static_cast<size_t>(w);
			}
			scan = msg_end + 1;
		}
		if (scan > 0) {
			size_t const remain = held - scan;
			memmove(buf.data(), buf.data() + scan, remain);
			held = remain;
		}
		if (held == buf.size()) {
			break;
		}
	}
done:
	SSL_shutdown(ssl);
	SSL_free(ssl);
	::close(cfd);
}

int start_listener(
	uint16_t &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw runtime_error{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw runtime_error{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw runtime_error{"listen"};
	}
	return fd;
}

int connect_to(
	uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = ::htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw runtime_error{"connect"};
	}
	return fd;
}

size_t encode_line(
	span<char> out,
	uint64_t n) {
	auto const r = to_chars(out.data(), out.data() + out.size() - 1, n);
	if (r.ec != errc{}) {
		throw runtime_error{"to_chars"};
	}
	*r.ptr = '\n';
	return static_cast<size_t>(r.ptr - out.data()) + 1;
}

uint64_t decode_line(
	string_view line) {
	uint64_t n = 0;
	auto const r = from_chars(line.data(), line.data() + line.size(), n);
	if (r.ec != errc{}) {
		throw runtime_error{"from_chars"};
	}
	return n;
}

struct AsyncTlsLineReader {
	TlsAsyncStream &tls;
	array<byte, 256> buf{};
	size_t held = 0;

	Task<string_view> read_line() {
		for (;;) {
			auto it = find(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(held), static_cast<byte>('\n'));
			if (it != buf.begin() + static_cast<ptrdiff_t>(held)) {
				auto const end = static_cast<size_t>(it - buf.begin());
				co_return string_view{reinterpret_cast<char const *>(buf.data()), end};
			}
			auto got = co_await tls.read_some(span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				throw runtime_error{"tls eof"};
			}
			held += got;
		}
	}

	void consume_line(
		size_t line_len) {
		size_t const drop = line_len + 1;
		if (drop >= held) {
			held = 0;
		} else {
			memmove(buf.data(), buf.data() + drop, held - drop);
			held -= drop;
		}
	}
};

uint64_t run_callback(
	FileReader &files,
	TlsAsyncStream &tls,
	size_t iters,
	uint64_t start) {
	AsyncTlsLineReader reader{.tls = tls};
	array<char, 24> out{};
	uint64_t n = start;
	auto const t0 = chrono::steady_clock::now();
	for (size_t i = 0; i < iters; ++i) {
		size_t const len = encode_line(out, n);
		block_on(files, tls.write_all(as_bytes(span{out.data(), len})));
		auto line = block_on(files, reader.read_line());
		uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw runtime_error{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<uint64_t> coro_loop(
	TlsAsyncStream &tls,
	size_t iters,
	uint64_t start) {
	AsyncTlsLineReader reader{.tls = tls};
	array<char, 24> out{};
	uint64_t n = start;
	for (size_t i = 0; i < iters; ++i) {
		size_t const len = encode_line(out, n);
		co_await tls.write_all(as_bytes(span{out.data(), len}));
		auto line = co_await reader.read_line();
		uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw runtime_error{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	co_return n;
}

uint64_t run_coroutine(
	FileReader &files,
	TlsAsyncStream &tls,
	size_t iters,
	uint64_t start) {
	auto const t0 = chrono::steady_clock::now();
	(void)block_on(files, coro_loop(tls, iters, start));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

} // namespace

int main(
	int argc,
	char **argv) try {
	auto cfg = parse_args(span{argv, static_cast<size_t>(argc)});

	auto kc = make_self_signed();

	SSL_CTX *sctx = SSL_CTX_new(TLS_server_method());
	if (sctx == nullptr) {
		println(cerr, "SSL_CTX_new server failed");
		return 1;
	}
	SSL_CTX_use_certificate(sctx, kc.cert);
	SSL_CTX_use_PrivateKey(sctx, kc.pkey);

	for (int which = 0; which < 2; ++which) {
		uint16_t port = 0;
		int const lfd = start_listener(port);
		atomic_flag server_stop{};
		thread server{[lfd, sctx, &server_stop] { run_server(lfd, sctx, server_stop); }};

		int const csock = connect_to(port);
		::close(lfd);

		::io_uring ring{};
		if (::io_uring_queue_init(64, &ring, 0) < 0) {
			::close(csock);
			server_stop.test_and_set(memory_order_release);
			server.join();
			println(cerr, "io_uring_queue_init failed");
			SSL_CTX_free(sctx);
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
			uint64_t const ns = (which == 0) ? run_callback(files, tls, cfg.iterations, cfg.warmup) :
											   run_coroutine(files, tls, cfg.iterations, cfg.warmup);
			double const per = static_cast<double>(ns) / static_cast<double>(cfg.iterations);
			string_view const label = (which == 0) ? "callback" : "coroutine";
			if (cfg.csv) {
				if (which == 0) {
					println("style,iterations,total_ns,ns_per_iter");
				}
				println("{},{},{},{:.1f}", label, cfg.iterations, ns, per);
			} else {
				if (which == 0) {
					println("iterations: {}, warmup: {}", cfg.iterations, cfg.warmup);
				}
				println("  {:<10} {:>8.1f} ns/iter ({} ns total)", label, per, ns);
			}
		} catch (exception const &e) { println(cerr, "error: {}", e.what()); }

		::io_uring_queue_exit(&ring);
		server_stop.test_and_set(memory_order_release);
		int const raw = tls.handle().raw_fd();
		::shutdown(raw, SHUT_RDWR);
		server.join();
	}

	SSL_CTX_free(sctx);
} catch (exception const &e) {
	println(cerr, "fatal: {}", e.what());
	return 1;
}
