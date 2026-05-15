#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef CONFLUX_BENCH_HAS_TLS
#define CONFLUX_BENCH_HAS_TLS 0
#endif

#if CONFLUX_BENCH_HAS_TLS
#include <openssl/ssl.h>
#endif

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::literals;
namespace {

struct BenchClient {
	int fd = -1;
	explicit BenchClient(
		u16 port) {
		connect_to(port);
	}
	~BenchClient() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	BenchClient(BenchClient const &) = delete;
	BenchClient &operator =(
		BenchClient const &) = delete;
	BenchClient(
		BenchClient &&o) noexcept
		: fd(exchange(o.fd, -1)) {}
	BenchClient &operator =(
		BenchClient &&o) noexcept {
		if (this != &o) {
			if (fd >= 0) {
				::close(fd);
			}
			fd = exchange(o.fd, -1);
		}
		return *this;
	}
	void connect_to(
		u16 port) {
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw RE{"socket failed"};
		}
		static constexpr int one = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(fd);
			fd = -1;
			throw RE{"connect failed"};
		}
	}
	void send_all(
		SV data) const {
		auto const *p = data.data();
		auto remaining = data.size();
		while (remaining > 0) {
			auto n = ::send(fd, p, remaining, MSG_NOSIGNAL);
			if (n <= 0) {
				throw RE{"send failed"};
			}
			p += n;
			remaining -= static_cast<SZ>(n);
		}
	}
	SZ recv_response(
		span<char> buf) const {
		SZ total = 0;
		SZ hdr_end_pos = SV::npos;
		SZ body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto n = ::recv(fd, buf.data() + total, buf.size() - total, 0);
			if (n <= 0) {
				break;
			}
			total += static_cast<SZ>(n);
			if (hdr_end_pos == SV::npos) {
				SV sofar{buf.data(), total};
				hdr_end_pos = sofar.find("\r\n\r\n");
				if (hdr_end_pos == SV::npos) {
					continue;
				}
				hdr_end_pos += 4;
				SV hdrs{buf.data(), hdr_end_pos};
				auto cl = hdrs.find("Content-Length: ");
				if (cl != SV::npos) {
					cl += 16;
					auto end = hdrs.find("\r\n", cl);
					from_chars(buf.data() + cl, buf.data() + end, body_len);
					have_cl = true;
				}
				if (hdrs.starts_with("HTTP/1.1 304") || hdrs.starts_with("HTTP/1.1 204")) {
					return total;
				}
			}
			if (have_cl && total >= hdr_end_pos + body_len) {
				return total;
			}
			if (!have_cl && hdr_end_pos != SV::npos) {
				return total;
			}
		}
		return total;
	}
};

void wait_for_server(
	u16 port) {
	for (int i = 0; i < 200; ++i) {
		int const s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (s < 0) {
			continue;
		}
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		bool const up = ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		::close(s);
		if (up) {
			return;
		}
		std::this_thread::sleep_for(chrono::milliseconds(10));
	}
	throw RE{"server did not start in time"};
}
struct ServerHandle {
	SP<HttpServer> server;
	thread thr;
	u16 port{};
};
ServerHandle start_server(
	Config cfg,
	Router router) {
	(void)::signal(SIGPIPE, SIG_IGN);
	cfg.startup_banner = false;
	auto srv = make_shared<HttpServer>(cfg, move(router));
	thread t{[srv] {
		try {
			auto _ = srv->run();
		} catch (exception const &e) { std::println(std::cerr, "bench server: {}", e.what()); }
	}};
	auto p = srv->port();
	wait_for_server(p);
	return {.server = srv, .thr = move(t), .port = p};
}
HttpServerMetrics stop_server(
	ServerHandle &h) {
	if (!h.server) {
		return {};
	}
	h.server->shutdown();
	if (h.thr.joinable()) {
		h.thr.join();
	}
	return h.server->metrics();
}
Config bench_config_zc(
	SV mode) {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_zc = S{mode};
	cfg.send_zc_threshold = 16384;
	cfg.send_zc_report_usage = true;
	return cfg;
}
using RunFn = Fn<void()>;
struct Variant {
	S name;
	RunFn setup;
	RunFn run;
	RunFn teardown;
	Fn<HttpServerMetrics()> metrics;
	SZ ops_per_iter = 1;
	SZ iters_override = 0;
};
struct BenchStats {
	S config;
	S variant;
	SZ iterations{};
	u64 total_ns{};
	double ns_per_iter{};
	HttpServerMetrics metrics{};
};
BenchStats run_variant(
	Variant const &v,
	SZ iterations,
	SZ warmup,
	SV config_name) {
	if (v.iters_override) {
		iterations = v.iters_override;
		warmup = max(SZ{2}, v.iters_override / 10);
	}
	if (v.setup && warmup > 0) {
		v.setup();
		for (SZ i = 0; i < warmup; ++i) {
			v.run();
		}
		if (v.teardown) {
			v.teardown();
		}
	} else {
		for (SZ i = 0; i < warmup; ++i) {
			v.run();
		}
	}
	if (v.setup) {
		v.setup();
	}
	auto const t0 = bench_now_ns();
	for (SZ i = 0; i < iterations; ++i) {
		v.run();
	}
	auto const t1 = bench_now_ns();
	if (v.teardown) {
		v.teardown();
	}
	auto const total = t1 - t0;
	auto const ns_pi = static_cast<double>(total) / static_cast<double>(iterations);
	return BenchStats{
		.config = S{config_name},
		.variant = v.name,
		.iterations = iterations,
		.total_ns = total,
		.ns_per_iter = ns_pi,
		.metrics = v.metrics ? v.metrics() : HttpServerMetrics{},
	};
}
void print_variant(
	BenchStats const &s,
	bool json,
	SZ ops_per_iter) {
	auto const &zc = s.metrics.send_zc;
	if (json) {
		auto const total_ops = s.iterations * ops_per_iter;
		auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"ops_per_iter\":{},\"total_ops\":{},\"ns_per_op\":{:.2f},"
			"\"zc_attempts\":{},\"zc_plain_attempts\":{},\"zc_mapped_attempts\":{},"
			"\"zc_bytes_requested\":{},\"zc_bytes_sent\":{},\"zc_notifications\":{},"
			"\"zc_copied_notifications\":{},\"zc_sends_without_notification\":{},"
			"\"zc_errors_enomem\":{},\"zc_errors_other\":{},\"zc_fallback_regular_send\":{},"
			"\"zc_tls_bypass\":{},\"zc_tls_bypass_bytes\":{},\"zc_adaptive_disable_count\":{},"
			"\"zc_notifications_pending\":{}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			ops_per_iter,
			total_ops,
			ns_per_op,
			zc.attempts,
			zc.plain_attempts,
			zc.mapped_attempts,
			zc.bytes_requested,
			zc.bytes_sent,
			zc.notifications,
			zc.copied_notifications,
			zc.sends_without_notification,
			zc.errors_enomem,
			zc.errors_other,
			zc.fallback_regular_send,
			zc.tls_bypass,
			zc.tls_bypass_bytes,
			zc.adaptive_disable_count,
			s.metrics.zc_notifications_pending);
	} else if (ops_per_iter > 1) {
		auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
		std::println(
			"{:<40} {:>8} iters  {:>10.2f} ns/iter  {:>10.2f} ns/op (x{})  zc={}/{} map={} tls_bypass={}",
			s.variant,
			s.iterations,
			s.ns_per_iter,
			ns_per_op,
			ops_per_iter,
			zc.attempts,
			zc.copied_notifications,
			zc.mapped_attempts,
			zc.tls_bypass);
	} else {
		std::println(
			"{:<40} {:>8} iters  {:>10.2f} ns/iter  zc={}/{} map={} tls_bypass={}",
			s.variant,
			s.iterations,
			s.ns_per_iter,
			zc.attempts,
			zc.copied_notifications,
			zc.mapped_attempts,
			zc.tls_bypass);
	}
}

#if CONFLUX_BENCH_HAS_TLS
struct SslCtxDeleter {
	void operator ()(
		SSL_CTX *p) const noexcept {
		if (p) {
			SSL_CTX_free(p);
		}
	}
};
struct SslDeleter {
	void operator ()(
		SSL *p) const noexcept {
		if (p) {
			SSL_shutdown(p);
			SSL_free(p);
		}
	}
};
P<S, S> make_self_signed_cert() {
	char cert_tmp[] = "/tmp/conflux_send_zc_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_send_zc_key_XXXXXX.pem";
	{
		int f = ::mkstemps(cert_tmp, 4);
		if (f < 0) {
			throw RE{"mkstemps cert failed"};
		}
		::close(f);
	}
	{
		int f = ::mkstemps(key_tmp, 4);
		if (f < 0) {
			throw RE{"mkstemps key failed"};
		}
		::close(f);
	}
	auto cmd = format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	if (::system(cmd.c_str()) != 0) {
		throw RE{"openssl req failed — TLS send_zc bench requires openssl CLI"};
	}
	return {S{cert_tmp}, S{key_tmp}};
}
void ssl_write_all(
	SSL *ssl,
	SV data) {
	auto const *p = data.data();
	auto remaining = data.size();
	while (remaining > 0) {
		auto const n = SSL_write(ssl, p, static_cast<int>(min<SZ>(remaining, 16 * 1024)));
		if (n <= 0) {
			throw RE{"SSL_write failed"};
		}
		p += n;
		remaining -= static_cast<SZ>(n);
	}
}
SZ ssl_recv_response(
	SSL *ssl,
	span<char> buf) {
	SZ total = 0;
	SZ hdr_end_pos = SV::npos;
	SZ body_len = 0;
	bool have_cl = false;
	for (;;) {
		auto n = SSL_read(ssl, buf.data() + total, static_cast<int>(buf.size() - total));
		if (n <= 0) {
			break;
		}
		total += static_cast<SZ>(n);
		if (hdr_end_pos == SV::npos) {
			SV sofar{buf.data(), total};
			hdr_end_pos = sofar.find("\r\n\r\n");
			if (hdr_end_pos == SV::npos) {
				continue;
			}
			hdr_end_pos += 4;
			SV hdrs{buf.data(), hdr_end_pos};
			auto cl = hdrs.find("Content-Length: ");
			if (cl != SV::npos) {
				cl += 16;
				auto end = hdrs.find("\r\n", cl);
				from_chars(buf.data() + cl, buf.data() + end, body_len);
				have_cl = true;
			}
		}
		if (have_cl && total >= hdr_end_pos + body_len) {
			return total;
		}
		if (!have_cl && hdr_end_pos != SV::npos) {
			return total;
		}
	}
	return total;
}
#endif

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"send_zc","parser":"standard","configs":[{"name":"default","extra":{"captures_send_zc_counters":true},"args":["--iterations","2000","--warmup","200"]}]})");

	auto const args = bench_parse_args(span{argv, static_cast<SZ>(argc)});
	auto const iters = args.iterations;
	auto const warmup = args.warmup;
	auto const json_out = args.json_out;
	auto const config_name = args.config_name.empty() ? "default"sv : SV{args.config_name};
	struct BodySpec {
		SV label;
		SZ size;
	};
	static constexpr A<BodySpec, 7> kBodies{
		{{"512B", 512}, {"1K", 1024}, {"4K", 4096}, {"16K", 16384}, {"64K", 65536}, {"256K", 262144}, {"1M", 1048576}}
	};

	M<S, S> body_map;
	for (auto const &[label, size]: kBodies) {
		body_map.emplace(S{label}, S(size, 'X'));
	}

	auto make_body_router = [&] {
		Router r;
		for (auto const &[label, body]: body_map) {
			auto const *body_ptr = &body;
			r.get(format("/body/{}", label), [body_ptr](HttpRequest const &) { return HttpResponse::text(*body_ptr); });
		}
		return r;
	};

	auto const static_dir = fs::temp_directory_path() / format("conflux_send_zc_bench_static_{}", ::getpid());
	fs::create_directories(static_dir);
	for (auto const &[label, size]: kBodies) {
		auto path = static_dir / format("{}.bin", label);
		std::ofstream out{path, std::ios::binary};
		S data(size, 'Y');
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
	}
	auto make_static_router = [&] {
		Router r;
		r.serve_static("/", S{static_dir.string()});
		return r;
	};

	V<char> recv_buf(1200000);
	auto rb = span<char>{recv_buf};
	V<Variant> variants;

	auto make_http_variant = [&](S name, SV mode, Fn<Router()> router_factory, S request, SZ iters_override = 0) {
		struct State {
			UP<ServerHandle> server;
			UP<BenchClient> client;
			HttpServerMetrics metrics{};
		};
		auto st = make_shared<State>();
		return Variant{
			.name = move(name),
			.setup = [st, mode = S{mode}, router_factory = move(router_factory)] {
				st->metrics = {};
				st->server = make_unique<ServerHandle>(start_server(bench_config_zc(SV{mode}), router_factory()));
				st->client = make_unique<BenchClient>(st->server->port);
			},
			.run = [st, request = move(request), rb] {
				st->client->send_all(request);
				(void)st->client->recv_response(rb);
			},
			.teardown = [st] {
				st->client.reset();
				if (st->server) {
					st->metrics = stop_server(*st->server);
					st->server.reset();
				}
			},
			.metrics = [st] { return st->metrics; },
			.ops_per_iter = 1,
			.iters_override = iters_override,
		};
	};

	for (auto const &[label, size]: kBodies) {
		auto const req = S{format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto label_s = S{label};
		variants.push_back(make_http_variant(format("plain/{}/off", label_s), "off", make_body_router, req));
		variants.push_back(make_http_variant(format("plain/{}/zc_auto", label_s), "auto", make_body_router, req));
	}

	for (auto const &[label, size]: kBodies) {
		auto const req = S{format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto label_s = S{label};
		variants.push_back(make_http_variant(format("mapped/{}/off", label_s), "off", make_static_router, req));
		variants.push_back(make_http_variant(format("mapped/{}/zc_auto", label_s), "auto", make_static_router, req));
	}

#if CONFLUX_BENCH_HAS_TLS
	auto [tls_cert_path, tls_key_path] = make_self_signed_cert();
	std::unique_ptr<SSL_CTX, SslCtxDeleter> ssl_ctx{[] {
		SSL_CTX *c = SSL_CTX_new(TLS_client_method());
		if (c == nullptr) {
			throw RE{"SSL_CTX_new failed"};
		}
		SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
		SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
		return c;
	}()};

	auto make_tls_variant = [&](S name, SV mode, Fn<Router()> router_factory, S request, SZ iters_override = 200) {
		struct State {
			UP<ServerHandle> server;
			UP<BenchClient> client;
			std::unique_ptr<SSL, SslDeleter> ssl;
			HttpServerMetrics metrics{};
		};
		auto st = make_shared<State>();
		return Variant{
			.name = move(name),
			.setup = [&, st, mode = S{mode}, router_factory = move(router_factory)] {
				st->metrics = {};
				auto cfg = bench_config_zc(SV{mode});
				cfg.cert_file = tls_cert_path;
				cfg.key_file = tls_key_path;
				st->server = make_unique<ServerHandle>(start_server(cfg, router_factory()));
				st->client = make_unique<BenchClient>(st->server->port);
				SSL *ssl = SSL_new(ssl_ctx.get());
				if (ssl == nullptr) {
					throw RE{"SSL_new failed"};
				}
				SSL_set_fd(ssl, st->client->fd);
				if (SSL_connect(ssl) != 1) {
					SSL_free(ssl);
					throw RE{"SSL_connect failed"};
				}
				st->ssl.reset(ssl);
			},
			.run = [st, request = move(request), rb] {
				ssl_write_all(st->ssl.get(), request);
				(void)ssl_recv_response(st->ssl.get(), rb);
			},
			.teardown = [st] {
				st->ssl.reset();
				st->client.reset();
				if (st->server) {
					st->metrics = stop_server(*st->server);
					st->server.reset();
				}
			},
			.metrics = [st] { return st->metrics; },
			.ops_per_iter = 1,
			.iters_override = iters_override,
		};
	};

	for (auto const &[label, size]: kBodies) {
		if (size < 65536) {
			continue;
		}
		auto label_s = S{label};
		auto const body_req = S{format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto const mapped_req = S{format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		SZ const tls_iters = size >= 1048576 ? 50 : 200;
		variants.push_back(
			make_tls_variant(format("tls/plain/{}/off", label_s), "off", make_body_router, body_req, tls_iters));
		variants.push_back(
			make_tls_variant(format("tls/plain/{}/zc_auto", label_s), "auto", make_body_router, body_req, tls_iters));
		variants.push_back(
			make_tls_variant(format("tls/mapped/{}/off", label_s), "off", make_static_router, mapped_req, tls_iters));
		variants.push_back(
			make_tls_variant(format("tls/mapped/{}/zc_auto", label_s), "auto", make_static_router, mapped_req, tls_iters));
	}
#endif

	if (!json_out) {
		std::println("send_zc_bench: {} iterations, {} warmup\n", iters, warmup);
	}

	for (auto const &v: variants) {
		auto s = run_variant(v, iters, warmup, config_name);
		print_variant(s, json_out, v.ops_per_iter);
	}

	fs::remove_all(static_dir);
#if CONFLUX_BENCH_HAS_TLS
	::unlink(tls_cert_path.c_str());
	::unlink(tls_key_path.c_str());
#endif
}
