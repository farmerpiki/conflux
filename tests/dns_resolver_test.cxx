// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.net.dns;

using namespace conflux::net::dns;
using namespace conflux::net::dns::codec;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
class TempTextFile {
public:
	TempTextFile(
		std::string_view stem,
		std::string_view contents)
		: path_{
			  std::filesystem::temp_directory_path()
			  / std::format(
				  "conflux-dns-{}-{}-{}",
				  stem,
				  ::getpid(),
				  std::chrono::steady_clock::now().time_since_epoch().count())} {
		std::ofstream out{path_};
		out << contents;
	}
	~TempTextFile() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	TempTextFile(TempTextFile const &) = delete;
	TempTextFile &operator =(TempTextFile const &) = delete;
	[[nodiscard]] std::filesystem::path const &path() const noexcept { return path_; }
	void write(
		std::string_view contents) const {
		std::ofstream out{path_};
		out << contents;
	}

private:
	std::filesystem::path path_;
};
// ---------------------------------------------------------------------------
// DnsMockServer — thread-based UDP listener on 127.0.0.1:0
// ---------------------------------------------------------------------------

class DnsMockServer {
public:
	enum class RespKind : std::uint8_t {
		noerror,
		nxdomain,
		servfail,
		refused,
		formerr,
		no_response,
	};
	struct MockRR {
		std::vector<std::uint8_t> rdata; // 4 bytes = A, 16 bytes = AAAA
		std::uint32_t ttl{60};
	};
	struct Response {
		RespKind kind{RespKind::nxdomain}; // default: NXDOMAIN
		std::vector<MockRR> records;
		std::uint16_t id_delta{0};
		bool wrong_question{false};
		bool truncated{false};
		std::chrono::milliseconds response_delay{0};
		std::vector<std::uint8_t> raw_response;
	};
	struct ReceivedQuery {
		std::string name;
		std::uint16_t qtype{};
		std::chrono::steady_clock::time_point at{};
	};
	DnsMockServer() {
		fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
		if (fd_ < 0) {
			throw std::runtime_error{"DnsMockServer: socket failed"};
		}
		::sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_port = 0;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(fd_, reinterpret_cast<::sockaddr const *>(&sa), sizeof(sa)) < 0) {
			::close(fd_);
			throw std::runtime_error{"DnsMockServer: bind failed"};
		}
		::sockaddr_in bound{};
		socklen_t len = sizeof(bound);
		::getsockname(fd_, reinterpret_cast<::sockaddr *>(&bound), &len);
		port_ = ntohs(bound.sin_port);

		::timeval tv{};
		tv.tv_usec = 50000; // 50 ms receive timeout for clean shutdown
		::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		thread_ = std::thread{[this] { run(); }};
	}
	~DnsMockServer() {
		running_.store(false, std::memory_order_relaxed);
		thread_.join();
		::close(fd_);
	}
	DnsMockServer(DnsMockServer const &) = delete;
	DnsMockServer &operator =(DnsMockServer const &) = delete;
	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] NameserverEndpoint endpoint() const noexcept {
		NameserverEndpoint ns{};
		auto *sin = reinterpret_cast<::sockaddr_in *>(&ns.addr);
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port_);
		sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ns.addr_len = sizeof(::sockaddr_in);
		ns.port = port_;
		return ns;
	}
	void set_response(
		std::string const &name,
		std::uint16_t qtype,
		Response resp) {
		std::scoped_lock const lk{mtx_};
		responses_[name + ':' + std::to_string(qtype)] = std::move(resp);
	}
	[[nodiscard]] std::vector<ReceivedQuery> queries() const {
		std::scoped_lock const lk{mtx_};
		return received_;
	}
	void clear_queries() {
		std::scoped_lock const lk{mtx_};
		received_.clear();
	}
	[[nodiscard]] std::size_t query_count(
		std::string_view name,
		std::uint16_t qtype) const {
		std::scoped_lock const lk{mtx_};
		std::size_t n = 0;
		for (auto const &q: received_) {
			if (q.name == name && q.qtype == qtype) {
				++n;
			}
		}
		return n;
	}

private:
	void run() {
		std::array<std::uint8_t, 512> buf{};
		::sockaddr_storage src{};
		socklen_t src_len{};

		while (running_.load(std::memory_order_relaxed)) {
			src_len = sizeof(src);
			ssize_t const n =
				::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<::sockaddr *>(&src), &src_len);
			if (n <= 12) {
				continue; // timeout or malformed
			}
			auto const wire = std::span<std::uint8_t const>{buf.data(), static_cast<std::size_t>(n)};
			auto const name = get_qname(wire);
			auto const qtype = get_qtype(wire);

			{
				std::scoped_lock const lk{mtx_};
				received_.push_back({name, qtype, std::chrono::steady_clock::now()});
			}

			Response resp;
			{
				std::scoped_lock const lk{mtx_};
				auto const key = name + ':' + std::to_string(qtype);
				if (auto it = responses_.find(key); it != responses_.end()) {
					resp = it->second;
				}
			}
			if (resp.kind == RespKind::no_response) {
				continue;
			}
			if (resp.response_delay.count() > 0) {
				std::this_thread::sleep_for(resp.response_delay);
			}
			if (!resp.raw_response.empty()) {
				::sendto(
					fd_,
					resp.raw_response.data(),
					resp.raw_response.size(),
					0,
					reinterpret_cast<::sockaddr const *>(&src),
					src_len);
				continue;
			}

			auto const reply = build_response(wire, resp);
			::sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<::sockaddr const *>(&src), src_len);
		}
	}
	static std::string get_qname(
		std::span<std::uint8_t const> wire) {
		std::string name;
		std::size_t i = 12;
		while (i < wire.size() && wire[i] != 0) {
			std::uint8_t const len = wire[i++];
			if (!name.empty()) {
				name += '.';
			}
			for (std::uint8_t j = 0; j < len && i < wire.size(); ++j) {
				char c = static_cast<char>(wire[i++]);
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c + ('a' - 'A'));
				}
				name += c;
			}
		}
		return name;
	}
	static std::uint16_t get_qtype(
		std::span<std::uint8_t const> wire) {
		std::size_t i = 12;
		while (i < wire.size() && wire[i] != 0) {
			i += 1U + wire[i];
		}
		++i;
		if (i + 2 > wire.size()) {
			return 0;
		}
		return static_cast<std::uint16_t>((static_cast<std::uint16_t>(wire[i]) << 8U) | wire[i + 1]);
	}
	static std::size_t find_question_end(
		std::span<std::uint8_t const> wire) {
		std::size_t i = 12;
		while (i < wire.size() && wire[i] != 0) {
			i += 1U + wire[i];
		}
		return i + 1 + 4; // null + QTYPE(2) + QCLASS(2)
	}
	static std::uint8_t rcode_for(
		RespKind k) noexcept {
		switch (k) {
		case RespKind::noerror : return 0;
		case RespKind::nxdomain: return 3;
		case RespKind::servfail: return 2;
		case RespKind::refused : return 5;
		case RespKind::formerr : return 1;
		default                : return 0;
		}
	}
	static std::vector<std::uint8_t> build_response(
		std::span<std::uint8_t const> query,
		Response const &resp) {
		std::vector<std::uint8_t> out;
		out.reserve(64);

		for (int k = 0; k < 12; ++k) {
			out.push_back(query[static_cast<std::size_t>(k)]);
		}
		if (resp.id_delta != 0) {
			std::uint16_t const id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(out[0]) << 8U) | out[1]);
			std::uint16_t const mutated = static_cast<std::uint16_t>(id + resp.id_delta);
			out[0] = static_cast<std::uint8_t>(mutated >> 8U);
			out[1] = static_cast<std::uint8_t>(mutated & 0xFFU);
		}
		out[2] = static_cast<std::uint8_t>((out[2] & 0x01U) | 0x80U); // QR=1, keep RD
		out[3] = static_cast<std::uint8_t>(0x80U | rcode_for(resp.kind)); // RA=1, RCODE
		if (resp.truncated) {
			out[2] = static_cast<std::uint8_t>(out[2] | 0x02U); // TC=1
		}
		std::uint16_t const ancount =
			(resp.kind == RespKind::noerror) ? static_cast<std::uint16_t>(resp.records.size()) : std::uint16_t{0};
		out[6] = static_cast<std::uint8_t>(ancount >> 8U);
		out[7] = static_cast<std::uint8_t>(ancount & 0xFFU);
		out[8] = out[9] = out[10] = out[11] = 0;

		std::size_t const qend = find_question_end(query);
		for (std::size_t k = 12; k < qend && k < query.size(); ++k) {
			out.push_back(query[k]);
		}
		if (resp.wrong_question && out.size() >= qend) {
			// Corrupt the echoed QTYPE while leaving the packet otherwise parseable.
			out[qend - 4] = 0x00;
			out[qend - 3] = 0x0F; // MX
		}

		if (resp.kind == RespKind::noerror) {
			for (auto const &rr: resp.records) {
				out.push_back(0xC0);
				out.push_back(0x0C); // pointer to question QNAME
				std::uint16_t const rtype = (rr.rdata.size() == 4) ? std::uint16_t{1} : std::uint16_t{28};
				out.push_back(static_cast<std::uint8_t>(rtype >> 8U));
				out.push_back(static_cast<std::uint8_t>(rtype & 0xFFU));
				out.push_back(0x00);
				out.push_back(0x01); // CLASS IN
				out.push_back(static_cast<std::uint8_t>((rr.ttl >> 24U) & 0xFFU));
				out.push_back(static_cast<std::uint8_t>((rr.ttl >> 16U) & 0xFFU));
				out.push_back(static_cast<std::uint8_t>((rr.ttl >> 8U) & 0xFFU));
				out.push_back(static_cast<std::uint8_t>(rr.ttl & 0xFFU));
				auto const rdlen = static_cast<std::uint16_t>(rr.rdata.size());
				out.push_back(static_cast<std::uint8_t>(rdlen >> 8U));
				out.push_back(static_cast<std::uint8_t>(rdlen & 0xFFU));
				for (auto b: rr.rdata) {
					out.push_back(b);
				}
			}
		}
		return out;
	}
	int fd_{-1};
	std::uint16_t port_{};
	std::thread thread_;
	std::atomic<bool> running_{true};
	mutable std::mutex mtx_;
	std::unordered_map<std::string, Response> responses_;
	std::vector<ReceivedQuery> received_;
};
ResolveOptions mock_opts(
	DnsMockServer const &mock) {
	ResolveOptions opts;
	opts.override_nameservers = {mock.endpoint()};
	return opts;
}
struct RingGuard {
	::io_uring ring{};
	CompletionTable ct;
	bool ok{false};

	RingGuard() = default;
	~RingGuard() {
		if (ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingGuard(RingGuard const &) = delete;
	RingGuard &operator =(RingGuard const &) = delete;
	RingGuard(RingGuard &&) = delete;
	RingGuard &operator =(RingGuard &&) = delete;
	static std::unique_ptr<RingGuard> make(
		unsigned entries = 32) {
		auto g = std::make_unique<RingGuard>();
		g->ok = (::io_uring_queue_init(entries, &g->ring, 0) == 0);
		return g;
	}
};
// SocketTaskRing-owning ring guard for caller-ring tests.
struct StrRingGuard {
	::io_uring ring{};
	CompletionTable ct;
	SocketTaskRing str;
	bool ring_ok{false};
	StrRingGuard()
		: str{SocketRawRing{&ring}, ct, [](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t {
				  return pack_ud(s, g);
			  }} {}
	~StrRingGuard() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	StrRingGuard(StrRingGuard const &) = delete;
	StrRingGuard &operator =(StrRingGuard const &) = delete;
	static std::unique_ptr<StrRingGuard> make(
		unsigned entries = 64) {
		auto g = std::make_unique<StrRingGuard>();
		if (::io_uring_queue_init(entries, &g->ring, 0) < 0) {
			return {};
		}
		g->ring_ok = true;
		return g;
	}
};
template<typename T>
T block_on_str(
	StrRingGuard &g,
	conflux::work::root::Task<T> task,
	std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) {
	using namespace conflux::work::root;
	struct Slot {
		std::atomic_flag done{};
		std::exception_ptr err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
	};
	auto slot = std::make_shared<Slot>();
	auto jh = std::make_shared<TaskJoinHandle<T>>(into_join_handle(std::move(task)));
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = blocking_join(std::move(*jh));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = make_exception_ptr(std::runtime_error{"task cancelled"});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done.test_and_set(std::memory_order_release);
	});
	auto *raw = &g.ring;
	auto *ct = &g.ct;
	auto const deadline = std::chrono::steady_clock::now() + budget;
	while (!slot->done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		__kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
		int const rc = ::io_uring_submit_and_wait_timeout(raw, &cqe, 1, &ts, nullptr);
		if (rc == -ETIME) {
			if (std::chrono::steady_clock::now() > deadline) {
				throw std::runtime_error{"block_on_str: budget exhausted"};
			}
			continue;
		}
		if (rc == -EINTR) {
			continue;
		}
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(raw, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto const ud = c->user_data;
				ct->dispatch(
					static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
					static_cast<std::uint32_t>(ud >> 32U),
					c->res,
					conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(raw, n);
			if (slot->done.test(std::memory_order_acquire)) {
				break;
			}
		}
	}
	if (slot->err) {
		rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(*slot->value);
	}
}
void pump_ring_once(
	RingGuard &g,
	std::chrono::milliseconds wait = std::chrono::milliseconds{20}) {
	::io_uring_cqe *cqe = nullptr;
	__kernel_timespec ts{};
	auto const sec = std::chrono::duration_cast<std::chrono::seconds>(wait);
	ts.tv_sec = sec.count();
	ts.tv_nsec = (wait - sec).count() * 1000000LL;
	int const rc = ::io_uring_submit_and_wait_timeout(&g.ring, &cqe, 1, &ts, nullptr);
	if (rc == -ETIME || rc == -EINTR || (rc >= 0 && cqe == nullptr)) {
		return;
	}
	if (rc < 0) {
		throw std::runtime_error{std::format("io_uring_submit_and_wait_timeout failed: {}", rc)};
	}
	std::array<::io_uring_cqe *, 32> batch{};
	for (;;) {
		unsigned const n = ::io_uring_peek_batch_cqe(&g.ring, batch.data(), static_cast<unsigned>(batch.size()));
		if (n == 0) {
			break;
		}
		for (unsigned i = 0; i < n; ++i) {
			auto const *c = batch[static_cast<std::size_t>(i)];
			auto const ud = c->user_data;
			g.ct.dispatch(
				static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
				static_cast<std::uint32_t>(ud >> 32U),
				c->res,
				conflux::uring::CqeFlags{c->flags});
		}
		::io_uring_cq_advance(&g.ring, n);
	}
}
[[nodiscard]] bool pump_until_query(
	RingGuard &g,
	DnsMockServer const &mock,
	std::string_view name,
	std::uint16_t qtype,
	std::chrono::milliseconds budget = std::chrono::milliseconds{500}) {
	auto const deadline = std::chrono::steady_clock::now() + budget;
	while (std::chrono::steady_clock::now() < deadline) {
		if (mock.query_count(name, qtype) > 0) {
			return true;
		}
		pump_ring_once(g);
	}
	return mock.query_count(name, qtype) > 0;
}

} // namespace
// ---------------------------------------------------------------------------
// Tests: native_udp backend via resolve_blocking (spins its own temp ring)
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: resolve A record via native_udp backend",
	"[dns][resolver][native]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"a.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {192, 168, 1, 1}, .ttl = 300}},
		});

	auto result = r.resolve_blocking("a.test", 80, mock_opts(mock));
	REQUIRE(result.has_value());
	CHECK_FALSE(result->endpoints.empty());
	CHECK(result->endpoints[0].family == AddressFamily::v4);
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&result->endpoints[0].addr);
	CHECK(sin.sin_addr.s_addr == htonl(0xC0A80101U)); // 192.168.1.1
	CHECK(ntohs(sin.sin_port) == 80);
}
TEST_CASE(
	"dns: hosts file shortcut returns endpoint without querying nameserver",
	"[dns][resolver][hosts]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	TempTextFile const hosts{
		"hosts",
		"203.0.113.9 HostAlias\n"
		"2001:db8::9 HostAlias\n"};
	ResolverOptions resolver_opts;
	resolver_opts.hosts_file = hosts.path();
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	DnsMockServer const mock;
	auto opts = mock_opts(mock);
	auto result = r.resolve_blocking("HostAlias", 2525, opts);

	REQUIRE(result.has_value());
	CHECK(result->from_hosts_file);
	REQUIRE(result->endpoints.size() == 2);
	CHECK(mock.queries().empty());
	for (auto const &ep: result->endpoints) {
		if (ep.family == AddressFamily::v4) {
			auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&ep.addr);
			CHECK(ntohs(sin.sin_port) == 2525);
		} else {
			auto const &sin6 = *reinterpret_cast<::sockaddr_in6 const *>(&ep.addr);
			CHECK(ntohs(sin6.sin6_port) == 2525);
		}
	}
}
TEST_CASE(
	"dns: resolv.conf nameserver is used when no override is supplied",
	"[dns][resolver][resolv-conf]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);

	DnsMockServer mock;
	mock.set_response(
		"resolv-conf.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 7, 0, 1}, .ttl = 60}},
		});

	TempTextFile const resolv{"resolv", std::format("nameserver 127.0.0.1:{}\n", mock.port())};
	ResolverOptions resolver_opts;
	resolver_opts.resolv_conf = resolv.path();
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	ResolveOptions opts;
	opts.allow_v6 = false;
	auto result = r.resolve_blocking("resolv-conf.test", 80, opts);

	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(mock.query_count("resolv-conf.test", 1) == 1);
}
TEST_CASE(
	"dns: resolv.conf search domain is applied for short blocking lookup",
	"[dns][resolver][resolv-conf]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);

	DnsMockServer mock;
	mock.set_response(
		"www.example.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 7, 0, 2}, .ttl = 60}},
		});

	TempTextFile const resolv{
		"resolv-search",
		std::format(
			"nameserver 127.0.0.1:{}\n"
			"search example.test\n"
			"options ndots:2\n",
			mock.port())};
	ResolverOptions resolver_opts;
	resolver_opts.resolv_conf = resolv.path();
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	ResolveOptions opts;
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};
	auto result = r.resolve_blocking("www", 80, opts);

	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(mock.query_count("www.example.test", 1) == 1);
	CHECK(mock.query_count("www", 1) == 0);
}
TEST_CASE(
	"dns: resolv.conf attempts retries native nameserver",
	"[dns][resolver][resolv-conf]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);

	DnsMockServer mock;
	mock.set_response(
		"attempts.test",
		1,
		{
			.kind = DnsMockServer::RespKind::no_response,
			.records = {},
		});

	TempTextFile const resolv{
		"resolv-attempts",
		std::format(
			"nameserver 127.0.0.1:{}\n"
			"options attempts:2\n",
			mock.port())};
	ResolverOptions resolver_opts;
	resolver_opts.resolv_conf = resolv.path();
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	ResolveOptions opts;
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};
	auto result = r.resolve_blocking("attempts.test", 80, opts);

	REQUIRE(result.has_value());
	CHECK(result->endpoints.empty());
	CHECK(mock.query_count("attempts.test", 1) == 2);
}
TEST_CASE(
	"dns: reload refreshes resolv.conf nameservers",
	"[dns][resolver][resolv-conf]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);

	DnsMockServer first;
	first.set_response(
		"before-reload.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 7, 0, 3}, .ttl = 60}},
		});
	DnsMockServer second;
	second.set_response(
		"after-reload.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 7, 0, 4}, .ttl = 60}},
		});

	TempTextFile const resolv{"resolv-reload", std::format("nameserver 127.0.0.1:{}\n", first.port())};
	ResolverOptions resolver_opts;
	resolver_opts.resolv_conf = resolv.path();
	resolver_opts.cache_capacity = 0;
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	ResolveOptions opts;
	opts.allow_v6 = false;
	auto before = r.resolve_blocking("before-reload.test", 80, opts);
	REQUIRE(before.has_value());
	CHECK(first.query_count("before-reload.test", 1) == 1);

	resolv.write(std::format("nameserver 127.0.0.1:{}\n", second.port()));
	r.reload();

	auto after = r.resolve_blocking("after-reload.test", 80, opts);
	REQUIRE(after.has_value());
	CHECK(second.query_count("after-reload.test", 1) == 1);
	CHECK(first.query_count("after-reload.test", 1) == 0);
}
TEST_CASE(
	"dns: AAAA record returned first (v6 preference)",
	"[dns][resolver][native]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"dual.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 0, 1}, .ttl = 60}},
		});
	mock.set_response(
		"dual.test",
		28,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, .ttl = 60}},
		});

	auto result = r.resolve_blocking("dual.test", 443, mock_opts(mock));
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 2);
	CHECK(result->endpoints[0].family == AddressFamily::v6);
	CHECK(result->endpoints[1].family == AddressFamily::v4);
}
TEST_CASE(
	"dns: A record returned first when v4 is preferred",
	"[dns][resolver][native]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"prefer4.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 0, 4}, .ttl = 60}},
		});
	mock.set_response(
		"prefer4.test",
		28,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4}, .ttl = 60}},
		});

	auto opts = mock_opts(mock);
	opts.prefer = AddressFamily::v4;
	auto result = r.resolve_blocking("prefer4.test", 443, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 2);
	CHECK(result->endpoints[0].family == AddressFamily::v4);
	CHECK(result->endpoints[1].family == AddressFamily::v6);
}
TEST_CASE(
	"dns: A+AAAA both queried in parallel",
	"[dns][resolver][native][happy-eyeballs]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"parallel.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 1, 1, 1}, .ttl = 60}},
		});
	mock.set_response(
		"parallel.test",
		28,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, .ttl = 60}},
		});

	auto result = r.resolve_blocking("parallel.test", 80, mock_opts(mock));
	REQUIRE(result.has_value());
	CHECK(mock.query_count("parallel.test", 1) == 1);
	CHECK(mock.query_count("parallel.test", 28) == 1);
}
TEST_CASE(
	"dns: AAAA nxdomain with A success returns v4",
	"[dns][resolver][native][happy-eyeballs]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"v4only.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {172, 16, 0, 1}, .ttl = 60}},
		});

	auto result = r.resolve_blocking("v4only.test", 80, mock_opts(mock));
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(result->endpoints[0].family == AddressFamily::v4);
}
TEST_CASE(
	"dns: both A and AAAA nxdomain returns nxdomain error",
	"[dns][resolver][native]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer const mock; // default: NXDOMAIN for everything

	auto result = r.resolve_blocking("no.such.domain", 80, mock_opts(mock));
	// For native_udp, resolve_blocking catches DnsError thrown from block_on
	// and returns unexpected{} with DnsErrorKind::nxdomain.
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == DnsErrorKind::nxdomain);
}
TEST_CASE(
	"dns: cache hit on second lookup sends no new queries",
	"[dns][resolver][native][cache]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud, ResolverOptions{.cache_capacity = 16}};

	DnsMockServer mock;
	mock.set_response(
		"cached.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 0, 2}, .ttl = 300}},
		});

	auto opts = mock_opts(mock);

	auto r1 = r.resolve_blocking("cached.test", 80, opts);
	REQUIRE(r1.has_value());
	CHECK_FALSE(r1->from_cache);
	std::size_t const after_first = mock.queries().size();

	auto r2 = r.resolve_blocking("cached.test", 80, opts);
	REQUIRE(r2.has_value());
	CHECK(r2->from_cache);
	CHECK(mock.queries().size() == after_first); // no new queries
	CHECK_FALSE(r2->endpoints.empty());
}
TEST_CASE(
	"dns: TTL in cache entry matches server response",
	"[dns][resolver][native][cache]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud, ResolverOptions{.cache_capacity = 16}};

	DnsMockServer mock;
	mock.set_response(
		"ttl.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 0, 3}, .ttl = 42}},
		});

	auto r1 = r.resolve_blocking("ttl.test", 80, mock_opts(mock));
	REQUIRE(r1.has_value());
	CHECK(r1->suggested_ttl == std::chrono::seconds{42});
}
TEST_CASE(
	"dns: deadlock detection — resolve_blocking on owned ring",
	"[dns][resolver][deadlock]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	CurrentResolverScope const scope{&r};
	auto result = r.resolve_blocking("example.com", 80);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == DnsErrorKind::cannot_block_on_owned_ring);
}
TEST_CASE(
	"dns: servfail does not propagate as nxdomain",
	"[dns][resolver][native]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	DnsMockServer::Response const sf{.kind = DnsMockServer::RespKind::servfail};
	mock.set_response("srv.test", 1, sf);
	mock.set_response("srv.test", 28, sf);

	auto result = r.resolve_blocking("srv.test", 80, mock_opts(mock));
	// SERVFAIL maps to BatchFailReason::network; both-fail → empty result, no nxdomain thrown.
	if (result.has_value()) {
		CHECK(result->endpoints.empty());
	} else {
		CHECK(result.error().kind != DnsErrorKind::nxdomain);
	}
}
TEST_CASE(
	"dns: truncated UDP with failed TCP fallback returns truncated error",
	"[dns][resolver][native][tcp]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"tcp-fallback.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {},
			.truncated = true,
		});

	auto opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};
	auto result = r.resolve_blocking("tcp-fallback.test", 80, opts);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == DnsErrorKind::truncated);
}
TEST_CASE(
	"dns: response with mismatched id is ignored",
	"[dns][resolver][native][validation]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"bad-id.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 9, 0, 1}, .ttl = 60}},
			.id_delta = 1,
		});

	auto opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};

	auto result = r.resolve_blocking("bad-id.test", 80, opts);
	REQUIRE(result.has_value());
	CHECK(result->endpoints.empty());
	CHECK(mock.query_count("bad-id.test", 1) == 1);
}
TEST_CASE(
	"dns: response with mismatched question is ignored",
	"[dns][resolver][native][validation]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"bad-question.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 9, 0, 2}, .ttl = 60}},
			.wrong_question = true,
		});

	auto opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};

	auto result = r.resolve_blocking("bad-question.test", 80, opts);
	REQUIRE(result.has_value());
	CHECK(result->endpoints.empty());
	CHECK(mock.query_count("bad-question.test", 1) == 1);
}
TEST_CASE(
	"dns: total timeout caps native query wait",
	"[dns][resolver][native][timeout]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"silent.test",
		1,
		{
			.kind = DnsMockServer::RespKind::no_response,
			.records = {},
		});

	auto opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{1000};
	opts.total_timeout = std::chrono::milliseconds{50};

	auto const start = std::chrono::steady_clock::now();
	auto result = r.resolve_blocking("silent.test", 80, opts);
	auto const elapsed = std::chrono::steady_clock::now() - start;

	REQUIRE(result.has_value());
	CHECK(result->endpoints.empty());
	CHECK(elapsed < std::chrono::milliseconds{500});
}
TEST_CASE(
	"dns: native resolver tries next nameserver after empty response",
	"[dns][resolver][native][nameserver]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer silent;
	silent.set_response(
		"fallback.test",
		1,
		{
			.kind = DnsMockServer::RespKind::no_response,
			.records = {},
		});
	DnsMockServer good;
	good.set_response(
		"fallback.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 8, 0, 1}, .ttl = 60}},
		});

	ResolveOptions opts;
	opts.override_nameservers = {silent.endpoint(), good.endpoint()};
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};

	auto result = r.resolve_blocking("fallback.test", 80, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(result->endpoints[0].family == AddressFamily::v4);
	CHECK(silent.query_count("fallback.test", 1) == 1);
	CHECK(good.query_count("fallback.test", 1) == 1);
}
TEST_CASE(
	"dns: malformed UDP response falls through to next nameserver",
	"[dns][resolver][native][nameserver][malformed]") {
	auto g = RingGuard::make();
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer malformed;
	malformed.set_response(
		"malformed-fallback.test",
		1,
		{
			.raw_response = {0x00, 0x01, 0x80},
    });
	DnsMockServer good;
	good.set_response(
		"malformed-fallback.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 8, 0, 2}, .ttl = 60}},
		});

	ResolveOptions opts;
	opts.override_nameservers = {malformed.endpoint(), good.endpoint()};
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{50};

	auto result = r.resolve_blocking("malformed-fallback.test", 8080, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(result->endpoints[0].family == AddressFamily::v4);
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&result->endpoints[0].addr);
	CHECK(sin.sin_addr.s_addr == htonl(0x0A080002U));
	CHECK(ntohs(sin.sin_port) == 8080);
	CHECK(malformed.query_count("malformed-fallback.test", 1) == 1);
	CHECK(good.query_count("malformed-fallback.test", 1) == 1);
}
// decode for block_on — matches pack_ud: gen in upper 32, slot in lower 32.
struct PackUdDecode {
	std::pair<std::uint32_t, std::uint32_t> operator ()(
		std::uint64_t ud) const noexcept {
		return {static_cast<std::uint32_t>(ud & 0xFFFFFFFFU), static_cast<std::uint32_t>(ud >> 32U)};
	}
};
TEST_CASE(
	"dns: async resolve cancellation stops pending UDP receive",
	"[dns][resolver][native][cancel][uring]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"cancel.test",
		1,
		{
			.kind = DnsMockServer::RespKind::no_response,
		});

	ResolveOptions opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{1000};
	opts.total_timeout = std::chrono::milliseconds{1000};

	auto task = r.resolve("cancel.test", 80, opts);
	REQUIRE(pump_until_query(*g, mock, "cancel.test", 1));
	task.cancel();

	bool cancelled = false;
	try {
		(void)block_on<ResolveResult>(
			*r.file_reader(),
			std::move(task),
			std::make_optional(std::chrono::milliseconds{5000}),
			PackUdDecode{});
	} catch (DnsError const &e) {
		cancelled = e.kind == DnsErrorKind::cancelled;
	} catch (conflux::work::root::CancelledError const &) { cancelled = true; }
	CHECK(cancelled);
}
TEST_CASE(
	"dns: async resolve cancellation after UDP send ignores late response",
	"[dns][resolver][native][cancel][uring][transport]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud, ResolverOptions{.cache_capacity = 0}};

	DnsMockServer mock;
	mock.set_response(
		"cancel-after-send.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 10, 0, 10}, .ttl = 60}},
			.response_delay = std::chrono::milliseconds{150},
		});
	mock.set_response(
		"after-cancel.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 10, 0, 11}, .ttl = 60}},
		});

	ResolveOptions opts = mock_opts(mock);
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{1000};
	opts.total_timeout = std::chrono::milliseconds{1000};

	auto task = r.resolve("cancel-after-send.test", 80, opts);
	REQUIRE(pump_until_query(*g, mock, "cancel-after-send.test", 1));
	task.cancel();

	bool cancelled = false;
	try {
		(void)block_on<ResolveResult>(
			*r.file_reader(),
			std::move(task),
			std::make_optional(std::chrono::milliseconds{5000}),
			PackUdDecode{});
	} catch (DnsError const &e) {
		cancelled = e.kind == DnsErrorKind::cancelled;
	} catch (conflux::work::root::CancelledError const &) { cancelled = true; }
	CHECK(cancelled);
	CHECK(mock.query_count("cancel-after-send.test", 1) == 1);

	auto result = r.resolve_blocking("after-cancel.test", 80, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(result->endpoints[0].family == AddressFamily::v4);
}
TEST_CASE(
	"dns: in-flight coalescing — two concurrent resolves send one query",
	"[dns][resolver][native][coalesce]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud, ResolverOptions{.cache_capacity = 16}};

	DnsMockServer mock;
	mock.set_response(
		"coalesce.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 0, 5}, .ttl = 60}},
		});
	mock.set_response(
		"coalesce.test",
		28,
		{
			.kind = DnsMockServer::RespKind::nxdomain,
			.records = {},
		});

	ResolveOptions const opts = mock_opts(mock);

	// Both calls happen before the ring is pumped → second attaches as waiter.
	using RR = ResolveResult;
	auto first = r.resolve("coalesce.test", 80, opts);
	auto second = r.resolve("coalesce.test", 80, opts);
	auto [res1, res2] = block_on<std::tuple<RR, RR>>(
		*r.file_reader(),
		join_all(std::move(first), std::move(second)),
		std::make_optional(std::chrono::milliseconds{5000}),
		PackUdDecode{});

	CHECK_FALSE(res1.endpoints.empty());
	CHECK_FALSE(res2.endpoints.empty());
	CHECK_FALSE(res1.from_coalesced);
	CHECK(res2.from_coalesced);

	// Mock server should have received only one A query and one AAAA query —
	// not doubled.
	CHECK(mock.query_count("coalesce.test", 1) == 1);
	CHECK(mock.query_count("coalesce.test", 28) == 1);
}
TEST_CASE(
	"dns: async resolve applies resolv.conf search domain",
	"[dns][resolver][native][resolv-conf][async]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);

	DnsMockServer mock;
	mock.set_response(
		"www.example.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 8, 0, 9}, .ttl = 60}},
		});

	TempTextFile const resolv{
		"resolv-async-search",
		std::format(
			"nameserver 127.0.0.1:{}\n"
			"search example.test\n"
			"options ndots:2\n",
			mock.port())};
	ResolverOptions resolver_opts;
	resolver_opts.resolv_conf = resolv.path();
	Resolver r{&g->ring, &g->ct, pack_ud, std::move(resolver_opts)};

	ResolveOptions opts;
	opts.allow_v6 = false;
	opts.query_timeout = std::chrono::milliseconds{100};
	auto result = block_on<ResolveResult>(
		*r.file_reader(),
		r.resolve("www", 80, opts),
		std::make_optional(std::chrono::milliseconds{5000}),
		PackUdDecode{});

	REQUIRE(result.endpoints.size() == 1);
	CHECK(mock.query_count("www.example.test", 1) == 1);
	CHECK(mock.query_count("www", 1) == 0);
}
TEST_CASE(
	"dns: NXDOMAIN is cached with negative TTL",
	"[dns][resolver][native][cache]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{
		&g->ring,
		&g->ct,
		pack_ud,
		ResolverOptions{.cache_capacity = 16, .cache_negative_ttl = std::chrono::seconds{30}}
    };

	DnsMockServer const mock; // default: NXDOMAIN for everything

	auto const opts = mock_opts(mock);

	auto r1 = r.resolve_blocking("negcache.test", 80, opts);
	REQUIRE_FALSE(r1.has_value());
	CHECK(r1.error().kind == DnsErrorKind::nxdomain);

	std::size_t const queries_after_first = mock.queries().size();

	// Second call must hit the negative cache — no new queries sent.
	auto r2 = r.resolve_blocking("negcache.test", 80, opts);
	REQUIRE_FALSE(r2.has_value());
	CHECK(r2.error().kind == DnsErrorKind::nxdomain);
	CHECK(mock.queries().size() == queries_after_first);
}
TEST_CASE(
	"dns: LRU eviction at capacity",
	"[dns][resolver][native][cache]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud, ResolverOptions{.cache_capacity = 1}};

	DnsMockServer mock;
	mock.set_response(
		"lru1.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 1, 1}, .ttl = 300}},
		});
	mock.set_response(
		"lru2.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 0, 1, 2}, .ttl = 300}},
		});

	auto opts = mock_opts(mock);

	// lru1 fills the single-slot cache.
	auto r1a = r.resolve_blocking("lru1.test", 80, opts);
	REQUIRE(r1a.has_value());
	CHECK_FALSE(r1a->from_cache);

	// lru2 evicts lru1.
	auto r2 = r.resolve_blocking("lru2.test", 80, opts);
	REQUIRE(r2.has_value());
	CHECK_FALSE(r2->from_cache);

	std::size_t const queries_before = mock.queries().size();

	// lru1 must be re-queried (evicted), not served from cache.
	auto r1b = r.resolve_blocking("lru1.test", 80, opts);
	REQUIRE(r1b.has_value());
	CHECK_FALSE(r1b->from_cache);
	CHECK(mock.queries().size() > queries_before);
}
// ---------------------------------------------------------------------------
// Tests: resolve(SocketTaskRing&, ...) — caller-provided ring
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: resolve(ring) drives query on caller-supplied ring, not resolver-owned ring",
	"[dns][resolver][native][async_ring]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"ext-ring.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 1, 2, 3}, .ttl = 60}},
		});

	auto gb = StrRingGuard::make();
	REQUIRE(gb);
	REQUIRE(gb->ring_ok);

	ResolveOptions opts = mock_opts(mock);
	opts.allow_v6 = false;
	auto result = block_on_str<ResolveResult>(
		*gb,
		r.resolve(gb->str, "ext-ring.test", 80, opts),
		std::chrono::milliseconds{5000});

	REQUIRE_FALSE(result.endpoints.empty());
	CHECK(result.endpoints[0].family == AddressFamily::v4);
	CHECK(mock.query_count("ext-ring.test", 1) == 1);
}
TEST_CASE(
	"dns: resolve(ring) does not coalesce with in-flight query on a different ring",
	"[dns][resolver][native][async_ring][coalesce]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"anti-coalesce.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 4, 5, 6}, .ttl = 60}},
		});

	auto gb = StrRingGuard::make();
	REQUIRE(gb);
	REQUIRE(gb->ring_ok);

	ResolveOptions opts = mock_opts(mock);
	opts.allow_v6 = false;

	// Start a query on the resolver-owned ring (pumped later).
	auto first = r.resolve("anti-coalesce.test", 80, opts);
	// Start a query on ring B — different InFlightKey, must not coalesce with first.
	auto second = block_on_str<ResolveResult>(
		*gb,
		r.resolve(gb->str, "anti-coalesce.test", 80, opts),
		std::chrono::milliseconds{5000});

	REQUIRE_FALSE(second.endpoints.empty());
	CHECK_FALSE(second.from_coalesced);

	// Now pump the resolver-owned ring so first completes cleanly.
	auto first_result = block_on<ResolveResult>(
		*r.file_reader(),
		std::move(first),
		std::make_optional(std::chrono::milliseconds{5000}),
		PackUdDecode{});
	REQUIRE_FALSE(first_result.endpoints.empty());
	CHECK_FALSE(first_result.from_coalesced);

	// Both rings sent independent queries — mock received at least 2.
	CHECK(mock.query_count("anti-coalesce.test", 1) >= 2);
}
TEST_CASE(
	"dns: two resolve(ring) calls for same host coalesce on same ring",
	"[dns][resolver][native][async_ring][coalesce]") {
	auto g = RingGuard::make();
	REQUIRE(g);
	REQUIRE(g->ok);
	Resolver r{&g->ring, &g->ct, pack_ud};

	DnsMockServer mock;
	mock.set_response(
		"coalesce-b.test",
		1,
		{
			.kind = DnsMockServer::RespKind::noerror,
			.records = {{.rdata = {10, 7, 8, 9}, .ttl = 60}},
		});
	mock.set_response("coalesce-b.test", 28, {.kind = DnsMockServer::RespKind::nxdomain});

	auto gb = StrRingGuard::make();
	REQUIRE(gb);
	REQUIRE(gb->ring_ok);

	ResolveOptions const opts = mock_opts(mock);

	// Both calls before ring B is pumped — second attaches as waiter.
	using RR = ResolveResult;
	auto first = r.resolve(gb->str, "coalesce-b.test", 80, opts);
	auto second = r.resolve(gb->str, "coalesce-b.test", 80, opts);
	auto [res1, res2] = block_on_str<std::tuple<RR, RR>>(
		*gb,
		join_all(std::move(first), std::move(second)),
		std::chrono::milliseconds{5000});

	CHECK_FALSE(res1.endpoints.empty());
	CHECK_FALSE(res2.endpoints.empty());
	CHECK_FALSE(res1.from_coalesced);
	CHECK(res2.from_coalesced);
	// Only one A and one AAAA query should have been sent.
	CHECK(mock.query_count("coalesce-b.test", 1) == 1);
	CHECK(mock.query_count("coalesce-b.test", 28) == 1);
}
TEST_CASE(
	"dns: resolve(ring) on nss_thread resolver rejects non-literal host, passes early exits",
	"[dns][resolver][nss_thread][async_ring]") {
	using namespace conflux;
	WorkPool pool{WorkPoolOptions{.threads = 1}};

	TempTextFile const hosts{"hosts-ext-ring", "127.0.0.1 hosts-hit.test\n"};
	ResolverOptions ropts;
	ropts.enable_etc_hosts = true;
	ropts.hosts_file = hosts.path();
	Resolver r{pool, std::move(ropts)};

	auto gb = StrRingGuard::make();
	REQUIRE(gb);
	REQUIRE(gb->ring_ok);

	// IP literal always succeeds regardless of backend.
	auto lit = block_on_str<ResolveResult>(*gb, r.resolve(gb->str, "192.168.0.1", 80), std::chrono::milliseconds{1000});
	REQUIRE(lit.endpoints.size() == 1);
	CHECK(lit.endpoints[0].family == AddressFamily::v4);

	// /etc/hosts hit succeeds without hitting the backend.
	auto hosts_hit =
		block_on_str<ResolveResult>(*gb, r.resolve(gb->str, "hosts-hit.test", 80), std::chrono::milliseconds{1000});
	REQUIRE_FALSE(hosts_hit.endpoints.empty());
	CHECK(hosts_hit.from_hosts_file);

	// Non-literal, non-cached host must return not_implemented.
	try {
		block_on_str<ResolveResult>(*gb, r.resolve(gb->str, "example.test", 80), std::chrono::milliseconds{1000});
		FAIL("expected DnsError");
	} catch (DnsError const &e) { CHECK(e.kind == DnsErrorKind::not_implemented); }
}
