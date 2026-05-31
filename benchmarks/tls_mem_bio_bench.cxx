// tls_mem_bio_bench — steady-state TLS encode/decode floor without sockets.
//
// tls_tcp_increment_coro exercises OpenSSL plus loopback TCP, blocking server
// thread, io_uring client wakeups, and request/response parsing. This benchmark
// keeps a warm client/server TLS session connected by OpenSSL memory BIO pairs
// and measures record encode/decode for payload sizes used by static/HTTP rows.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bench_common;

using namespace std::string_view_literals;

namespace {

inline std::atomic<std::uint64_t> sink{};

struct Config {
	std::size_t iterations = 2000;
	std::size_t warmup = 100;
	std::size_t samples = 0;
	std::size_t batch = 0;
	std::size_t bytes = 64 * 1024;
	bool json_out = false;
	std::string config_name;
};

[[nodiscard]] Config parse_args(
	std::span<char *> args) {
	Config cfg;
	auto base = bench_parse_args(args);
	cfg.iterations = base.iterations;
	cfg.warmup = base.warmup;
	cfg.samples = base.samples;
	cfg.batch = base.batch;
	cfg.json_out = base.json_out;
	cfg.config_name = std::move(base.config_name);
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--bytes" && i + 1 < args.size()) {
			cfg.bytes = bench_parse_sz(args[++i]);
		}
	}
	cfg.bytes = std::max<std::size_t>(1, cfg.bytes);
	if (cfg.config_name.empty()) {
		cfg.config_name = std::format("bytes_{}", cfg.bytes);
	}
	return cfg;
}

struct UniqueCtx {
	SSL_CTX *ptr{};
	UniqueCtx() = default;
	explicit UniqueCtx(
		SSL_CTX *p)
		: ptr{p} {}
	~UniqueCtx() {
		if (ptr != nullptr) {
			SSL_CTX_free(ptr);
		}
	}
	UniqueCtx(UniqueCtx const &) = delete;
	UniqueCtx &operator =(UniqueCtx const &) = delete;
	UniqueCtx(
		UniqueCtx &&other) noexcept
		: ptr{std::exchange(other.ptr, nullptr)} {}
	UniqueCtx &operator =(
		UniqueCtx &&other) noexcept {
		if (this != &other) {
			if (ptr != nullptr) {
				SSL_CTX_free(ptr);
			}
			ptr = std::exchange(other.ptr, nullptr);
		}
		return *this;
	}
	[[nodiscard]] SSL_CTX *get() const noexcept { return ptr; }
};

struct UniqueSsl {
	SSL *ptr{};
	UniqueSsl() = default;
	explicit UniqueSsl(
		SSL *p)
		: ptr{p} {}
	~UniqueSsl() {
		if (ptr != nullptr) {
			SSL_free(ptr);
		}
	}
	UniqueSsl(UniqueSsl const &) = delete;
	UniqueSsl &operator =(UniqueSsl const &) = delete;
	UniqueSsl(
		UniqueSsl &&other) noexcept
		: ptr{std::exchange(other.ptr, nullptr)} {}
	UniqueSsl &operator =(
		UniqueSsl &&other) noexcept {
		if (this != &other) {
			if (ptr != nullptr) {
				SSL_free(ptr);
			}
			ptr = std::exchange(other.ptr, nullptr);
		}
		return *this;
	}
	[[nodiscard]] SSL *get() const noexcept { return ptr; }
};

struct KeyCert {
	EVP_PKEY *pkey{};
	X509 *cert{};
	KeyCert() = default;
	~KeyCert() {
		if (pkey != nullptr) {
			EVP_PKEY_free(pkey);
		}
		if (cert != nullptr) {
			X509_free(cert);
		}
	}
	KeyCert(KeyCert const &) = delete;
	KeyCert &operator =(KeyCert const &) = delete;
	KeyCert(
		KeyCert &&other) noexcept
		: pkey{std::exchange(other.pkey, nullptr)}
		, cert{std::exchange(other.cert, nullptr)} {}
	KeyCert &operator =(
		KeyCert &&other) noexcept {
		if (this != &other) {
			if (pkey != nullptr) {
				EVP_PKEY_free(pkey);
			}
			if (cert != nullptr) {
				X509_free(cert);
			}
			pkey = std::exchange(other.pkey, nullptr);
			cert = std::exchange(other.cert, nullptr);
		}
		return *this;
	}
};

[[nodiscard]] KeyCert make_self_signed() {
	KeyCert kc;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
	if (pctx == nullptr) {
		throw std::runtime_error{"EVP_PKEY_CTX_new_from_name"};
	}
	if (EVP_PKEY_keygen_init(pctx) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw std::runtime_error{"EVP_PKEY_keygen_init"};
	}
	if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw std::runtime_error{"EVP_PKEY_CTX_set_rsa_keygen_bits"};
	}
	if (EVP_PKEY_keygen(pctx, &kc.pkey) <= 0) {
		EVP_PKEY_CTX_free(pctx);
		throw std::runtime_error{"EVP_PKEY_keygen"};
	}
	EVP_PKEY_CTX_free(pctx);

	kc.cert = X509_new();
	if (kc.cert == nullptr) {
		throw std::runtime_error{"X509_new"};
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
		throw std::runtime_error{"X509_sign"};
	}
	return kc;
}

[[nodiscard]] UniqueCtx make_server_ctx(
	KeyCert const &kc) {
	UniqueCtx ctx{SSL_CTX_new(TLS_server_method())};
	if (ctx.get() == nullptr) {
		throw std::runtime_error{"SSL_CTX_new server"};
	}
	if (SSL_CTX_use_certificate(ctx.get(), kc.cert) != 1) {
		throw std::runtime_error{"SSL_CTX_use_certificate"};
	}
	if (SSL_CTX_use_PrivateKey(ctx.get(), kc.pkey) != 1) {
		throw std::runtime_error{"SSL_CTX_use_PrivateKey"};
	}
	SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
	return ctx;
}

[[nodiscard]] UniqueCtx make_client_ctx() {
	UniqueCtx ctx{SSL_CTX_new(TLS_client_method())};
	if (ctx.get() == nullptr) {
		throw std::runtime_error{"SSL_CTX_new client"};
	}
	SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
	return ctx;
}

struct TlsPair {
	UniqueSsl client;
	UniqueSsl server;
};

void throw_ssl_error(
	SSL *ssl,
	int rc,
	std::string_view what) {
	int const err = SSL_get_error(ssl, rc);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		return;
	}
	unsigned long const detail = ERR_get_error();
	throw std::runtime_error{std::format(
		"{} failed: ssl_error={} state={} detail={}",
		what,
		err,
		SSL_state_string_long(ssl),
		ERR_error_string(detail, nullptr))};
}

[[nodiscard]] TlsPair make_tls_pair(
	SSL_CTX *client_ctx,
	SSL_CTX *server_ctx) {
	TlsPair pair{UniqueSsl{SSL_new(client_ctx)}, UniqueSsl{SSL_new(server_ctx)}};
	if (pair.client.get() == nullptr || pair.server.get() == nullptr) {
		throw std::runtime_error{"SSL_new"};
	}
	BIO *client_rbio{};
	BIO *server_wbio{};
	BIO *server_rbio{};
	BIO *client_wbio{};
	if (BIO_new_bio_pair(&client_rbio, 0, &server_wbio, 0) != 1
		|| BIO_new_bio_pair(&server_rbio, 0, &client_wbio, 0) != 1) {
		throw std::runtime_error{"BIO_new_bio_pair"};
	}
	SSL_set_bio(pair.client.get(), client_rbio, client_wbio);
	SSL_set_bio(pair.server.get(), server_rbio, server_wbio);
	SSL_set_connect_state(pair.client.get());
	SSL_set_accept_state(pair.server.get());

	for (int spins = 0;
		 spins < 1000 && (SSL_is_init_finished(pair.client.get()) == 0 || SSL_is_init_finished(pair.server.get()) == 0);
		 ++spins) {
		if (SSL_is_init_finished(pair.client.get()) == 0) {
			int const rc = SSL_do_handshake(pair.client.get());
			if (rc != 1) {
				throw_ssl_error(pair.client.get(), rc, "client handshake"sv);
			}
		}
		if (SSL_is_init_finished(pair.server.get()) == 0) {
			int const rc = SSL_do_handshake(pair.server.get());
			if (rc != 1) {
				throw_ssl_error(pair.server.get(), rc, "server handshake"sv);
			}
		}
	}
	if (SSL_is_init_finished(pair.client.get()) == 0 || SSL_is_init_finished(pair.server.get()) == 0) {
		throw std::runtime_error{"TLS memory BIO handshake did not converge"};
	}
	return pair;
}

void ssl_write_all(
	SSL *ssl,
	std::span<char const> data) {
	std::size_t off = 0;
	while (off < data.size()) {
		int const n =
			SSL_write(ssl, data.data() + off, static_cast<int>(std::min<std::size_t>(data.size() - off, 16 * 1024)));
		if (n <= 0) {
			throw_ssl_error(ssl, n, "SSL_write"sv);
			continue;
		}
		off += static_cast<std::size_t>(n);
	}
}

std::uint64_t ssl_read_exact(
	SSL *ssl,
	std::span<char> buf,
	std::size_t expected) {
	std::size_t got = 0;
	std::uint64_t acc = 0;
	while (got < expected) {
		int const n = SSL_read(ssl, buf.data(), static_cast<int>(std::min<std::size_t>(buf.size(), expected - got)));
		if (n <= 0) {
			throw_ssl_error(ssl, n, "SSL_read"sv);
			continue;
		}
		got += static_cast<std::size_t>(n);
		acc += static_cast<unsigned char>(buf[0]);
		acc += static_cast<std::uint64_t>(n);
	}
	return acc;
}

struct RunStats {
	std::uint64_t client_to_server_ns{};
	std::uint64_t round_trip_ns{};
};

template<class F>
[[nodiscard]] BenchStats measure_loop(
	Config const &cfg,
	F &&fn) {
	BenchSamplePlan const plan = bench_sample_plan(cfg.iterations, cfg.warmup, cfg.samples, cfg.batch);
	return bench_measure_batched(std::forward<F>(fn), plan, cfg.bytes);
}

void print_row(
	Config const &cfg,
	std::string_view variant,
	BenchStats stats,
	bool &first) {
	stats.config = cfg.config_name;
	stats.variant = variant;
	if (cfg.json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"label\":"
			"\"micro/user-space\",\"payload_bytes\":{},\"mib_per_s\":{:.1f},\"sink\":{},\"sample_count\":{},"
			"\"batch\":{},\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f}}}",
			stats.config,
			stats.variant,
			stats.iterations,
			stats.total_ns,
			stats.ns_per_iter,
			cfg.bytes,
			stats.throughput,
			sink.load(std::memory_order_relaxed),
			stats.sample_count,
			stats.batch,
			stats.timer_sample_ns,
			stats.timer_overhead_pct);
	} else {
		if (first) {
			std::println("tls_mem_bio [{}]", cfg.config_name);
			first = false;
		}
		std::println(
			"{:<28} {:>10} iters {:>10.2f} ns/iter {:>10.1f} MiB/s label=micro/user-space",
			stats.variant,
			stats.iterations,
			stats.ns_per_iter,
			stats.throughput);
	}
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"tls_mem_bio","parser":"standard","configs":[{"name":"payload_4k","extra":{"payload_bytes":4096,"label":"micro/user-space"},"target_ms":500,"max_iterations":50000,"calibration_iterations":4,"args":["--bytes","4096","--config-name","payload_4k","--iterations","0","--warmup","0"]},{"name":"payload_64k","extra":{"payload_bytes":65536,"label":"micro/user-space"},"target_ms":500,"max_iterations":10000,"calibration_iterations":4,"args":["--bytes","65536","--config-name","payload_64k","--iterations","0","--warmup","0"]},{"name":"payload_1m","extra":{"payload_bytes":1048576,"label":"micro/user-space"},"target_ms":500,"max_iterations":1000,"calibration_iterations":2,"args":["--bytes","1048576","--config-name","payload_1m","--iterations","0","--warmup","0"]}]})");

	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	OPENSSL_init_ssl(0, nullptr);
	auto kc = make_self_signed();
	auto server_ctx = make_server_ctx(kc);
	auto client_ctx = make_client_ctx();
	std::string payload(cfg.bytes, 'T');
	std::vector<char> read_buf(std::min<std::size_t>(cfg.bytes, 16 * 1024));

	auto pair = make_tls_pair(client_ctx.get(), server_ctx.get());
	bool first = true;
	auto const one_way_stats = measure_loop(cfg, [&] {
		ssl_write_all(pair.client.get(), payload);
		sink.fetch_add(ssl_read_exact(pair.server.get(), read_buf, payload.size()), std::memory_order_relaxed);
	});
	print_row(cfg, "steady_client_to_server"sv, one_way_stats, first);

	pair = make_tls_pair(client_ctx.get(), server_ctx.get());
	auto const round_trip_stats = measure_loop(cfg, [&] {
		ssl_write_all(pair.client.get(), payload);
		sink.fetch_add(ssl_read_exact(pair.server.get(), read_buf, payload.size()), std::memory_order_relaxed);
		ssl_write_all(pair.server.get(), payload);
		sink.fetch_add(ssl_read_exact(pair.client.get(), read_buf, payload.size()), std::memory_order_relaxed);
	});
	print_row(cfg, "steady_round_trip_echo"sv, round_trip_stats, first);
	if (!cfg.json_out) {
		std::println("sink={}", sink.load(std::memory_order_relaxed));
	}
	return 0;
}
