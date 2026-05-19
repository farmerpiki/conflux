#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.socket_io.blocking;
import conflux.json;
import bench_common;

using namespace conflux::json;
namespace {

template<typename F>
BenchStats measure(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1,
	std::size_t bytes = 0) {
	for (std::size_t i = 0; i < warmup * batch; ++i) {
		fn();
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(iters);
	std::uint64_t total = 0;
	for (std::size_t i = 0; i < iters; ++i) {
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		total += elapsed;
		samples.push_back(elapsed);
	}
	sort(samples.begin(), samples.end());
	double const med = static_cast<double>(samples[iters / 2]) / static_cast<double>(batch);
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	return {
		.iterations = iters * batch,
		.total_ns = total,
		.ns_per_iter = med,
		.throughput = mbs,
	};
}
bool g_csv = false;
bool g_first_row = true;
void print_row(
	std::string_view name,
	BenchStats s) {
	s.variant = name;
	if (g_csv) {
		bench_print(s, true, g_first_row);
		g_first_row = false;
	} else if (s.throughput > 0.0) {
		print("[json-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s\n", name, s.ns_per_iter, s.throughput);
	} else {
		print("[json-bench] {:<40} {:>10.1f} ns\n", name, s.ns_per_iter);
	}
}
// ---------------------------------------------------------------------------
// Corpus builders
// ---------------------------------------------------------------------------

// Typical config corpus: ~4 KB flat object with S/number/bool values.
std::string make_config_corpus() {
	std::string out;
	out.reserve(4096);
	out += '{';
	for (int i = 0; i < 64; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(
			R"("key_{}":{{"value":{},"label":"item_{}","active":{}}})",
			i,
			i * 17,
			i,
			(i % 2 == 0) ? "true" : "false");
	}
	out += '}';
	return out;
}
// Struct-decode corpus: A of objects with SV-compatible fields.
std::string make_decode_corpus() {
	std::string out;
	out.reserve(8192);
	out += '[';
	for (int i = 0; i < 200; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"({{"id":{},"name":"user_{}","score":{}}})", i, i, i * 3.14);
	}
	out += ']';
	return out;
}
// Lookup corpus: object with 1024 members.
std::string make_lookup_corpus() {
	std::string out;
	out.reserve(32768);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("member_{}":{},)", i, i); // extra comma intentional — remove after
		out.pop_back();
	}
	out += '}';
	return out;
}
// Array traversal corpus: A of 10000 numbers.
std::string make_array_corpus() {
	std::string out;
	out.reserve(65536);
	out += '[';
	for (int i = 0; i < 10000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::to_string(i);
	}
	out += ']';
	return out;
}
// Large corpus for parse throughput gate: ~1 MB nested structure.
std::string make_large_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ);
	out += '[';
	for (int i = 0; i < 2000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(
			R"({{"id":{},"name":"entry_{}","tags":["alpha","beta","gamma"],"meta":{{"score":{},"active":{}}}}})",
			i,
			i,
			i * 1.5,
			(i % 2 == 0) ? "true" : "false");
	}
	out += ']';
	return out;
}
// R0 — long-S-heavy corpus: 32 elements of 32 KiB ASCII payload, no
// escapes. Exercises memcpy-free zero-copy std::string slice + the SIMD scan_str
// fast path on long unescaped runs.
std::string make_long_strings_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	constexpr int kElems = 32;
	constexpr int kLen = 32 * 1024;
	for (int i = 0; i < kElems; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += '"';
		for (int k = 0; k < kLen; ++k) {
			out += static_cast<char>('a' + (k % 26));
		}
		out += '"';
	}
	out += ']';
	return out;
}
// R0 — pretty-printed corpus: ~1 MB flat object, 2-space indent + newlines.
// Exposes skip_ws cost; today's compact corpora hide it.
std::string make_pretty_ws_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += "{\n";
	constexpr int kMembers = 16000;
	for (int i = 0; i < kMembers; ++i) {
		out += "  \"key_";
		out += std::to_string(i);
		out += "\" : ";
		out += std::to_string(i * 17);
		if (i + 1 < kMembers) {
			out += ',';
		}
		out += '\n';
	}
	out += "}\n";
	return out;
}
// R0 — escape-heavy corpus: a single 256 KiB std::string with backslash escapes
// at high density. Stresses the parse-side slow path (parse_str_decode_tail)
// and the dump-side escape scan.
std::string make_escape_heavy_corpus() {
	std::string out;
	constexpr std::size_t kTarget = 256UZ * 1024UZ;
	out.reserve(kTarget + 16);
	out += '"';
	while (out.size() + 8 < kTarget) {
		out += R"(\n\t\")"; // 6 source bytes → 3 JSON escapes per cycle
	}
	out += '"';
	return out;
}
// R0 — deeply-nested A: 256 levels of [[…]] with a single 0 at center.
// Tests recursion / iterative parse depth handling without tripping the
// 512-frame default max_depth.
std::string make_deep_nest_corpus() {
	std::string out;
	constexpr int kDepth = 256;
	out.reserve(kDepth * 2 + 4);
	out.append(kDepth, '[');
	out += '0';
	out.append(kDepth, ']');
	return out;
}
// R0 — mixed-number corpus: ~1 MB A of integers, scientific,
// long fractions, signed values. Stresses number-lexeme parse paths.
std::string make_mixed_numbers_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	bool first = true;
	int i = 0;
	constexpr std::size_t kTarget = 1024UZ * 1024UZ - 16;
	while (out.size() < kTarget) {
		if (!first) {
			out += ',';
		}
		first = false;
		switch (i % 4) {
		case 0 : out += std::to_string(i); break;
		case 1 : out += std::format("{}.{}e{}", i, i * 3, (i % 7) - 3); break;
		case 2 : out += std::format("0.{}", i); break;
		case 3 : out += std::format("-{}.{}", i, i * 9); break;
		default: break;
		}
		++i;
	}
	out += ']';
	return out;
}
// ---------------------------------------------------------------------------
// Benchmark drivers
// ---------------------------------------------------------------------------

void bench_parse_small(
	std::string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 100, 500, 1, corpus.size());
	print_row("parse/small (~4KB config)", s);
}
void bench_parse_large(
	std::string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 4, 20, 1, corpus.size());
	print_row("parse/large (~1MB nested)", s);
}
void bench_decode(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	// Measure: parse + extract name field as std::string_view from each object
	auto s = measure(
		[&] {
			auto res = parse(corpus);
			if (!res) {
				return;
			}
			auto arr = res->root().as_array();
			if (!arr) {
				return;
			}
			for (NodeRef const elem: arr->elements()) {
				auto obj = elem.as_object();
				if (!obj) {
					continue;
				}
				auto name = obj->find_member("name");
				if (name) {
					(void)name->as_string();
				}
			}
		},
		20,
		100,
		1,
		corpus.size());
	print_row("decode/struct-like (sv fields)", s);
}
void bench_find_member(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root()); // pre-build hash index
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	// batch=1000: amortise clock overhead for sub-microsecond lookup
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member object (per lookup)", s);
}
void bench_array_traversal(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto arr = doc->root().as_array();
	if (!arr) {
		return;
	}
	auto s = measure(
		[&] {
			std::int64_t sum = 0;
			for (NodeRef const elem: arr->elements()) {
				auto n = elem.as_number();
				if (n) {
					auto v = n->to_i64();
					if (v) {
						sum += *v;
					}
				}
			}
			(void)sum;
		},
		100,
		500);
	print_row("A/traverse 10k numbers", s);
}
void bench_builder() {
	auto s = measure(
		[&] {
			auto b = value_builder();
			auto obj = b.begin_object();
			if (!obj) {
				return;
			}
			for (int i = 0; i < 64; ++i) {
				(void)obj->insert_string(std::format("key_{}", i), std::format("value_{}", i));
			}
			std::move(*obj).commit();
			(void)std::move(b).finish();
		},
		100,
		500);
	print_row("builder/64-member object", s);
}
void bench_dump_plain(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, 100, 500, 1, json_str->size());
	print_row("dump/plain (no sort / no ascii_only)", s);
}
void bench_dump_sorted(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	JsonDumpOptions opts;
	opts.sort_object_keys = true;
	auto json_str = doc->dump(opts);
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(opts); }, 40, 200, 1, json_str->size());
	print_row("dump/sort_object_keys", s);
}
void bench_accumulate_chunked(
	std::string_view name,
	std::string const &corpus,
	std::size_t chunk_size) {
	auto s = measure(
		[&] {
			JsonAccumulator acc;
			auto const *ptr = corpus.data();
			std::size_t remaining = corpus.size();
			while (remaining > 0) {
				std::size_t const n = std::min(chunk_size, remaining);
				auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(ptr), n});
				if (!feed) {
					throw std::runtime_error{"json accumulator feed failed"};
				}
				ptr += n;
				remaining -= n;
			}
			auto doc = acc.finish();
			if (!doc) {
				throw std::runtime_error{"json accumulator finish failed"};
			}
			(void)doc->root();
		},
		20,
		100,
		1,
		corpus.size());
	print_row(name, s);
}
[[nodiscard]] int start_listener(
	std::uint16_t &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw std::runtime_error{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw std::runtime_error{"listen"};
	}
	return fd;
}
[[nodiscard]] sockaddr_storage loopback_addr(
	std::uint16_t port) noexcept {
	sockaddr_storage ss{};
	auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	sin->sin_port = ::htons(port);
	return ss;
}
void send_all(
	int fd,
	std::string_view data) {
	auto const *ptr = data.data();
	std::size_t remaining = data.size();
	while (remaining > 0) {
		ssize_t const n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error{"send"};
		}
		ptr += static_cast<std::size_t>(n);
		remaining -= static_cast<std::size_t>(n);
	}
}
void serve_json_corpus(
	int listener_fd,
	std::atomic_flag &stop,
	std::string_view corpus) {
	timeval tv{.tv_sec = 0, .tv_usec = 100000};
	(void)::setsockopt(listener_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (!stop.test(std::memory_order_acquire)) {
		int const cfd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		try {
			send_all(cfd, corpus);
			::shutdown(cfd, SHUT_WR);
		} catch (...) {
			::close(cfd);
			break;
		}
		::close(cfd);
	}
	::close(listener_fd);
}
struct TempCorpusFile {
	std::filesystem::path path;
	explicit TempCorpusFile(
		std::string_view corpus) {
		path = std::filesystem::temp_directory_path() / "conflux_json_bench_e2e.json";
		std::ofstream out{path, std::ios::binary | std::ios::trunc};
		if (!out) {
			throw std::runtime_error{std::format("cannot open {}", path.string())};
		}
		out.write(corpus.data(), static_cast<std::streamsize>(corpus.size()));
		if (!out) {
			throw std::runtime_error{std::format("cannot write {}", path.string())};
		}
	}
	~TempCorpusFile() {
		std::error_code ec;
		(void)std::filesystem::remove(path, ec);
	}
};
conflux::work::root::Task<void> decode_file_once(
	FileReader &files,
	std::string path) {
	auto handle = co_await files.async_open(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
	JsonAccumulator acc;
	std::array<std::uint8_t, 8192> buf{};
	std::uint64_t off = 0;
	for (;;) {
		auto got = co_await files.read_into(handle, off, std::as_writable_bytes(std::span{buf}));
		if (got == 0) {
			break;
		}
		off += got;
		auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(buf.data()), got});
		if (!feed) {
			throw std::runtime_error{"json accumulator feed failed"};
		}
	}
	auto doc = acc.finish();
	if (!doc) {
		throw std::runtime_error{"json accumulator finish failed"};
	}
	(void)doc->root();
}
conflux::work::root::Task<void> decode_socket_once(
	SocketTaskRing &ring,
	std::uint16_t port) {
	auto ss = loopback_addr(port);
	auto stream = co_await async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in));
	JsonAccumulator acc;
	std::array<std::uint8_t, 8192> buf{};
	for (;;) {
		auto got = co_await stream.async_recv_borrowed(std::span<std::uint8_t>{buf.data(), buf.size()});
		if (got == 0) {
			break;
		}
		auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(buf.data()), got});
		if (!feed) {
			throw std::runtime_error{"json accumulator feed failed"};
		}
	}
	auto doc = acc.finish();
	if (!doc) {
		throw std::runtime_error{"json accumulator finish failed"};
	}
	(void)doc->root();
}
void bench_e2e_decode(
	std::string_view name,
	std::string const &corpus) {
	TempCorpusFile const temp{corpus};
	std::string const file_path = temp.path.string();
	::io_uring raw{};
	if (::io_uring_queue_init(64, &raw, 0) < 0) {
		throw std::runtime_error{"io_uring_queue_init"};
	}
	CompletionTable ct;
	auto const pack_ud = [](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t {
		return (static_cast<std::uint64_t>(g) << 32U) | s;
	};
	FileReader files{&raw, &ct, pack_ud};
	SocketTaskRing ring{SocketRawRing{&raw}, ct, pack_ud};
	try {
		auto file_stats =
			measure([&] { block_on(files, decode_file_once(files, file_path)); }, 4, 20, 1, corpus.size());
		print_row(std::format("{}/file_reader", name), file_stats);
		{
			std::uint16_t port = 0;
			int listener_fd = start_listener(port);
			std::atomic_flag stop{};
			std::thread server{[&] { serve_json_corpus(listener_fd, stop, corpus); }};
			try {
				auto socket_stats = measure(
					[&] { sync_wait_socket_task(ring, decode_socket_once(ring, port)); },
					4,
					20,
					1,
					corpus.size());
				print_row(std::format("{}/socket_task_ring", name), socket_stats);
			} catch (...) {
				stop.test_and_set(std::memory_order_release);
				(void)::shutdown(listener_fd, SHUT_RDWR);
				if (server.joinable()) {
					server.join();
				}
				throw;
			}
			stop.test_and_set(std::memory_order_release);
			(void)::shutdown(listener_fd, SHUT_RDWR);
			if (server.joinable()) {
				server.join();
			}
		}
	} catch (...) {
		::io_uring_queue_exit(&raw);
		throw;
	}
	::io_uring_queue_exit(&raw);
}
// Item C — 1024-member object where every key has a \u escape → arena storage.
// Decoded names are identical to make_lookup_corpus() ("member_N"), so the
// same lookup keys can be used for apples-to-apples comparison.
std::string make_lookup_escaped_corpus() {
	// Keys: "member_N" (JSON) → decoded "member_N".
	// All MemberEntry flags = 0 (arena); kStorageInputView never set.
	std::string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format("\"\\u006Dember_{}\":{}", i, i);
	}
	out += '}';
	return out;
}
// Item C — 1024-member object with alternating plain/escaped keys.
// Even indices: plain ("member_N", kStorageInputView).
// Odd indices:  "member_N" decoded to "member_N" (arena storage).
// Half-half pattern is worst-case for branch prediction in member_name() dispatch.
std::string make_lookup_mixed_corpus() {
	std::string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		if (i % 2 == 0) {
			out += std::format("\"member_{}\":{}", i, i);
		} else {
			out += std::format("\"\\u006Dember_{}\":{}", i, i);
		}
	}
	out += '}';
	return out;
}
// FI-1 — small object (below kHashThreshold=32): find_member always does linear
// scan. Proxy for per-lookup cost after the sentinel caches a build failure.
std::string make_below_threshold_corpus() {
	std::string out = "{";
	for (int i = 0; i < 7; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
// 5.5-B gate: 31-member object — always linear (just below kHashThreshold=32).
// Isolates cache-line packing benefit of 16-std::byte vs 24-std::byte MemberEntry.
std::string make_linear31_corpus() {
	std::string out = "{";
	for (int i = 0; i < 31; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("member_{}":{},)", i, i);
		out.pop_back(); // remove trailing comma left by format string
	}
	out += '}';
	return out;
}
void bench_find_member_linear31(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_15");
			(void)obj->find_member("member_30");
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/31-member linear (per lookup)", s);
}
// Item C — probe throughput on arena-storage names (baseline: bench_find_member
// uses kStorageInputView names). Delta isolates member_name() dispatch overhead.
void bench_find_member_escaped(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member escaped names (per lookup)", s);
}
// Item C — worst-case dispatch: alternating kStorageInputView/arena per probe.
void bench_find_member_mixed(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0"); // plain (even)
			(void)obj->find_member("member_511"); // escaped (odd)
			(void)obj->find_member("member_1023"); // escaped (odd)
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member mixed names (per lookup)", s);
}
// Item E — builder name-copy cost: same value ("v"), varying key length.
// If per-insert ns scales with key length, arena copy is the hot path.
// If flat, overhead is in tree structure — name-copy optimisation is not justified.
void bench_builder_name_length() {
	constexpr int kMembers = 256;
	auto gen_keys = [](std::size_t n, std::size_t total_len) {
		std::vector<std::string> keys;
		keys.reserve(n);
		for (std::size_t i = 0; i < n; ++i) {
			std::string const suffix = std::to_string(i);
			std::size_t const pad = total_len > suffix.size() ? total_len - suffix.size() : 0;
			std::string k(pad, 'k');
			k += suffix;
			keys.push_back(std::move(k));
		}
		return keys;
	};
	std::vector<std::string> const k5 = gen_keys(static_cast<std::size_t>(kMembers), 5);
	std::vector<std::string> const k32 = gen_keys(static_cast<std::size_t>(kMembers), 32);
	std::vector<std::string> const k128 = gen_keys(static_cast<std::size_t>(kMembers), 128);

	auto run = [&](std::vector<std::string> const &keys, std::string_view label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (int i = 0; i < kMembers; ++i) {
					(void)obj->insert_string(keys[static_cast<std::size_t>(i)], "v");
				}
				std::move(*obj).commit();
				(void)std::move(b).finish();
			},
			100,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run(k5, "builder/insert_string   5-char keys (per insert)");
	run(k32, "builder/insert_string  32-char keys (per insert)");
	run(k128, "builder/insert_string 128-char keys (per insert)");

	auto run_view = [&](std::vector<std::string> const &keys, std::string_view label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (std::size_t i = 0; i < static_cast<std::size_t>(kMembers); ++i) {
					(void)obj->insert_string_borrowed_name(keys[i], "v");
				}
				std::move(*obj).commit();
				(void)std::move(b).finish();
			},
			100,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run_view(k5, "builder/insert_string_borrowed_name   5-char keys (per insert)");
	run_view(k32, "builder/insert_string_borrowed_name  32-char keys (per insert)");
	run_view(k128, "builder/insert_string_borrowed_name 128-char keys (per insert)");
}
// R0 — generic parse/dump drivers used for the new corpora.
void bench_parse_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50) {
	auto s = measure([&] { (void)parse(corpus); }, warmup, iters, 1, corpus.size());
	print_row(name, s);
}
void bench_dump_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, warmup, iters, 1, json_str->size());
	print_row(name, s);
}

struct CorpusFileSpec {
	std::string_view label;
	std::string_view file;
	std::size_t warmup{10};
	std::size_t iters{50};
	bool dump{true};
};

[[nodiscard]] std::filesystem::path corpus_root() {
	return std::filesystem::path{__FILE__}.parent_path() / "corpus";
}
[[nodiscard]] std::optional<std::string> load_corpus_file(
	std::string_view filename) {
	std::filesystem::path const p = corpus_root() / std::filesystem::path{std::string{filename}};
	std::ifstream f{p, std::ios::binary};
	if (!f) {
		return std::nullopt;
	}
	return std::string{std::istreambuf_iterator<char>{f}, {}};
}
void bench_parse_required_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50,
	JsonParseOptions const &opts = {}) {
	auto s = measure(
		[&] {
			auto doc = parse(corpus, opts);
			if (!doc) {
				throw std::runtime_error{"json benchmark fixture parse failed"};
			}
		},
		warmup,
		iters,
		1,
		corpus.size());
	print_row(name, s);
}
void bench_parse_reject_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 20,
	std::size_t iters = 100) {
	auto s = measure(
		[&] {
			auto doc = parse(corpus);
			if (doc) {
				throw std::runtime_error{"malformed JSON benchmark fixture parsed successfully"};
			}
		},
		warmup,
		iters,
		1,
		corpus.size());
	print_row(name, s);
}
void bench_file_corpora(
	std::string_view title,
	std::span<CorpusFileSpec const> specs) {
	bool printed_header = false;
	for (CorpusFileSpec const &spec: specs) {
		auto corpus = load_corpus_file(spec.file);
		if (!corpus) {
			continue;
		}
		if (!printed_header && !g_csv) {
			std::println("[json-bench]");
			std::println("[json-bench] -- {} --", title);
			printed_header = true;
		}
		bench_parse_required_named(std::format("parse/{}", spec.label), *corpus, spec.warmup, spec.iters);
		if (spec.dump) {
			bench_dump_named(std::format("dump/{}", spec.label), *corpus, spec.warmup, spec.iters);
		}
	}
}
void bench_reject_file_corpora(
	std::string_view title,
	std::span<CorpusFileSpec const> specs) {
	bool printed_header = false;
	for (CorpusFileSpec const &spec: specs) {
		auto corpus = load_corpus_file(spec.file);
		if (!corpus) {
			continue;
		}
		if (!printed_header && !g_csv) {
			std::println("[json-bench]");
			std::println("[json-bench] -- {} --", title);
			printed_header = true;
		}
		bench_parse_reject_named(std::format("reject/{}", spec.label), *corpus, spec.warmup, spec.iters);
	}
}
void bench_duplicate_policy_fixture(
	std::string const &corpus) {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	bench_parse_required_named("parse/edge/duplicate_keys last_wins", corpus, 40, 200, opts);
	bench_parse_reject_named("reject/edge/duplicate_keys default", corpus, 40, 200);
}
// FI-1 — measures two components that together show the value of the sentinel:
//
//   (A) linear-only lookup (7-member, below kHashThreshold=48) — this is the
//       per-lookup cost WITH the sentinel cached (find_member short-circuits
//       straight to linear scan on every subsequent call after the first failure).
//
//   (B) hash-build overhead = (parse + first find_member) − (parse only), measured
//       on the 1024-member corpus. In the adversarial repeat-lookup scenario
//       WITHOUT the sentinel, (B) would be paid on every single call because
//       hash_idx_raw stays nullptr and each call retries alloc + build + free.
//       With the sentinel (FI-1), (B) is paid exactly once.
void bench_fi1_sentinel(
	std::string const &small_corpus,
	std::string const &lookup_corpus) {
	{
		auto doc = parse(small_corpus);
		if (!doc) {
			return;
		}
		auto obj = doc->root().as_object();
		if (!obj) {
			return;
		}
		auto s = measure(
			[&] {
				(void)obj->find_member("field_0");
				(void)obj->find_member("field_3");
				(void)obj->find_member("field_6");
			},
			200,
			1000,
			1000);
		s.ns_per_iter /= 3.0;
		print_row("FI-1/sentinel: (A) linear-only 7-member (failure path proxy)", s);
	}
	{
		auto parse_only = measure([&] { (void)parse(lookup_corpus); }, 20, 100);
		auto parse_find = measure(
			[&] {
				auto d = parse(lookup_corpus);
				if (!d) {
					return;
				}
				auto o = d->root().as_object();
				if (!o) {
					return;
				}
				(void)o->find_member("member_512");
			},
			20,
			100);
		double const build_ns = parse_find.ns_per_iter - parse_only.ns_per_iter;
		BenchStats diff{};
		diff.ns_per_iter = std::max(0.0, build_ns);
		print_row("FI-1/sentinel: (B) build+lookup overhead (parse+find − parse-only)", diff);
	}
}

} // namespace
// ---------------------------------------------------------------------------
// UnknownMemberPolicy::reject cost on wide objects
// ---------------------------------------------------------------------------

struct BenchModel5 {
	std::int64_t id{};
	std::string name{};
	double score{};
	bool active{};
	std::string tag{};
};
template<>
struct JsonMembers<BenchModel5> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchModel5::id),
			json_member("name", &BenchModel5::name),
			json_member("score", &BenchModel5::score),
			json_member("active", &BenchModel5::active),
			json_member("tag", &BenchModel5::tag),
		};
	}
	static constexpr std::string_view type_name() { return "BenchModel5"; }
};
namespace {

std::string make_reject_corpus(
	std::size_t extra_members) {
	std::string out;
	out.reserve(extra_members * 30 + 128);
	out += R"({"id":42,"name":"bench","score":3.14,"active":true,"tag":"x")";
	for (std::size_t i = 0; i < extra_members; ++i) {
		out += std::format(R"(,"extra_field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
void bench_reject_policy() {
	for (std::size_t extra: std::array<std::size_t, 5>{0, 10, 50, 100, 200}) {
		std::string const corpus = make_reject_corpus(extra);
		auto doc_res = parse(corpus);
		if (!doc_res) {
			return;
		}

		// reject policy (default): O(N·M) scan after DOM decode
		auto s_reject = measure(
			[&] {
				auto d = parse(corpus);
				if (!d) {
					return;
				}
				JsonDecodeOptions opts;
				opts.unknown_members = UnknownMemberPolicy::reject;
				auto r = decode<BenchModel5>(d->root(), opts);
				(void)r;
			},
			100,
			500);
		print_row(std::format("decode/reject 5+{} members", extra), s_reject);

		// ignore policy: no extra scan
		auto s_ignore = measure(
			[&] {
				auto d = parse(corpus);
				if (!d) {
					return;
				}
				JsonDecodeOptions opts;
				opts.unknown_members = UnknownMemberPolicy::ignore;
				auto r = decode<BenchModel5>(d->root(), opts);
				(void)r;
			},
			100,
			500);
		print_row(std::format("decode/ignore 5+{} members", extra), s_ignore);
	}
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_csv = cfg.json_out;
	if (!g_csv) {
		std::println("[json-bench] building corpora…");
	}
	std::string const config_corpus = make_config_corpus();
	std::string const decode_corpus = make_decode_corpus();
	std::string const lookup_corpus = make_lookup_corpus();
	std::string const array_corpus = make_array_corpus();
	std::string const large_corpus = make_large_corpus();
	std::string const long_strings_corpus = make_long_strings_corpus();
	std::string const pretty_ws_corpus = make_pretty_ws_corpus();
	std::string const escape_heavy_corpus = make_escape_heavy_corpus();
	std::string const deep_nest_corpus = make_deep_nest_corpus();
	std::string const mixed_numbers_corpus = make_mixed_numbers_corpus();
	std::string const lookup_escaped_corpus = make_lookup_escaped_corpus();
	std::string const lookup_mixed_corpus = make_lookup_mixed_corpus();
	std::string const below_threshold_corpus = make_below_threshold_corpus();
	std::string const linear31_corpus = make_linear31_corpus();

	if (!g_csv) {
		std::println(
			"[json-bench] corpus sizes: config={}B decode={}B lookup={}B A={}B large={}B",
			config_corpus.size(),
			decode_corpus.size(),
			lookup_corpus.size(),
			array_corpus.size(),
			large_corpus.size());
		std::println(
			"[json-bench]                long_strings={}B pretty_ws={}B escape_heavy={}B deep_nest={}B "
			"mixed_numbers={}B",
			long_strings_corpus.size(),
			pretty_ws_corpus.size(),
			escape_heavy_corpus.size(),
			deep_nest_corpus.size(),
			mixed_numbers_corpus.size());
		std::println("[json-bench]");
		std::println("[json-bench] {:<40} {:>10}     {:>10}", "benchmark", "median", "throughput");
		std::println("[json-bench] {}", std::string(60, '-'));
	}

	bench_parse_small(config_corpus);
	bench_parse_large(large_corpus);
	bench_decode(decode_corpus);
	bench_find_member(lookup_corpus);
	bench_array_traversal(array_corpus);
	bench_builder();
	bench_dump_plain(config_corpus);
	bench_dump_sorted(config_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (4KB config)", config_corpus, 4096);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- R0 corpora (added v16) --");
	}
	bench_parse_named("parse/long_strings (1MB / 32x32KiB)", long_strings_corpus);
	bench_dump_named("dump/long_strings", long_strings_corpus);
	bench_parse_named("parse/pretty_ws (1MB indented)", pretty_ws_corpus);
	bench_dump_named("dump/pretty_ws", pretty_ws_corpus);
	bench_parse_named("parse/escape_heavy (256KiB)", escape_heavy_corpus, 20, 100);
	bench_dump_named("dump/escape_heavy", escape_heavy_corpus, 20, 100);
	bench_parse_named("parse/deep_nest (256 levels)", deep_nest_corpus, 100, 500);
	bench_parse_named("parse/mixed_numbers (1MB)", mixed_numbers_corpus);
	bench_dump_named("dump/mixed_numbers", mixed_numbers_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (1MB large)", large_corpus, 4096);
	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- e2e JSON decode: FileReader vs SocketTaskRing --");
	}
	bench_e2e_decode("e2e/large_json_decode", large_corpus);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- v16 Item C/E: member_name dispatch + builder name-copy --");
		std::println("[json-bench]    Baseline (plain names, kStorageInputView) already shown above.");
	}
	bench_find_member_escaped(lookup_escaped_corpus);
	bench_find_member_mixed(lookup_mixed_corpus);
	bench_find_member_linear31(linear31_corpus);
	bench_builder_name_length();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- FI-1: sentinel prevents repeated hash-build on failure --");
		std::println("[json-bench]    (A) per-lookup cost after sentinel cached; (B) overhead saved per repeat call");
		std::println(
			"[json-bench]    adversarial cost WITHOUT sentinel: (A)+(B) per lookup; WITH: (A) after first call");
	}
	bench_fi1_sentinel(below_threshold_corpus, lookup_corpus);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- UnknownMemberPolicy::reject O(N·M) cost --");
	}
	bench_reject_policy();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] Acceptance thresholds:");
		std::println("[json-bench]   parse >=500 MB/s on typical-config corpus");
		std::println("[json-bench]   find_member <=1000 ns median on 1024-member object");
		std::println("[json-bench]   dump >=1000 MB/s on plain path");
	}

	{
		std::array<CorpusFileSpec, 5> const real_world{
			{
             {"file/canada geo", "canada.json"},
             {"file/citm_catalog catalog", "citm_catalog.json"},
             {"file/twitter social", "twitter.json"},
             {"file/apache_builds CI", "apache_builds.json"},
             {"file/github_events events", "github_events.json"},
			 }
        };
		bench_file_corpora("real-world corpora", real_world);
	}
	{
		std::array<CorpusFileSpec, 4> const route_payloads{
			{
             {"route/persona_create_request", "route_payloads/persona_create_request.json", 20, 200},
             {"route/content_generation_response", "route_payloads/content_generation_response.json", 20, 200},
             {"route/scheduled_publish_batch", "route_payloads/scheduled_publish_batch.json", 20, 200},
             {"route/analytics_timeseries", "route_payloads/analytics_timeseries.json", 20, 200},
			 }
        };
		bench_file_corpora("route payload fixtures", route_payloads);
	}
	{
		std::array<CorpusFileSpec, 3> const edge_cases{
			{
             {"edge/large_numbers", "edge/large_numbers.json", 20, 200},
             {"edge/escaped_unicode", "edge/escaped_unicode.json", 20, 200},
             {"edge/out_of_order_keys", "edge/out_of_order_keys.json", 20, 200},
			 }
        };
		bench_file_corpora("edge-case valid fixtures", edge_cases);
		if (auto duplicate_keys = load_corpus_file("edge/duplicate_keys.json")) {
			if (!g_csv) {
				std::println("[json-bench]");
				std::println("[json-bench] -- duplicate-key policy fixture --");
			}
			bench_duplicate_policy_fixture(*duplicate_keys);
		}
	}
	{
		std::array<CorpusFileSpec, 5> const malformed{
			{
             {"malformed/trailing_comma", "malformed/trailing_comma.json", 20, 200, false},
             {"malformed/bad_string_escape", "malformed/bad_string_escape.json", 20, 200, false},
             {"malformed/leading_zero", "malformed/leading_zero.json", 20, 200, false},
             {"malformed/unclosed_array", "malformed/unclosed_array.json", 20, 200, false},
             {"malformed/garbage_suffix", "malformed/garbage_suffix.json", 20, 200, false},
			 }
        };
		bench_reject_file_corpora("malformed rejection fixtures", malformed);
	}
}
