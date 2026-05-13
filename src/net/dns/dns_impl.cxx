module;

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.dns;

import std;
import conflux.file_io_sync;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.socket_io.blocking;

namespace conflux::net::dns {
namespace root = conflux::work::root;

namespace dns_local {

struct AddrInfoDeleter {
	void operator ()(
		addrinfo *p) const noexcept {
		::freeaddrinfo(p);
	}
};
using UniqueAddrInfo = std::unique_ptr<addrinfo, AddrInfoDeleter>;

} // namespace dns_local
using dns_local::UniqueAddrInfo;
namespace {

thread_local Resolver *tls_current_resolver{nullptr};

} // namespace
[[nodiscard]] Resolver *current_resolver() noexcept {
	return tls_current_resolver;
}

CurrentResolverScope::CurrentResolverScope(Resolver *next) noexcept
	: prev_{tls_current_resolver} {
	tls_current_resolver = next;
}

CurrentResolverScope::~CurrentResolverScope() {
	tls_current_resolver = prev_;
}
// ─── file-local helpers ──────────────────────────────────────────────────────

struct ResolvConfig {
	V<NameserverEndpoint> nameservers;
	V<S> search_domains;
	size_t ndots{1};
	chrono::milliseconds query_timeout{0};
	size_t attempts{1};
};
[[nodiscard]] S trim_ascii_copy(
	SV sv) {
	while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
		sv.remove_prefix(1);
	}
	while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
		sv.remove_suffix(1);
	}
	return S{sv};
}
[[nodiscard]] Opt<size_t> parse_decimal_size(
	SV sv) noexcept {
	if (sv.empty()) {
		return nullopt;
	}
	size_t out = 0;
	for (char const c: sv) {
		if (c < '0' || c > '9') {
			return nullopt;
		}
		out = (out * 10U) + static_cast<size_t>(c - '0');
	}
	return out;
}
void parse_resolv_options(
	SV rest,
	ResolvConfig &cfg) {
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		SV const token = end == SV::npos ? rest : rest.substr(0, end);
		if (end == SV::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}

		auto parse_after_colon = [](SV value, SV prefix) -> Opt<size_t> {
			if (!value.starts_with(prefix)) {
				return nullopt;
			}
			return parse_decimal_size(value.substr(prefix.size()));
		};

		if (auto timeout = parse_after_colon(token, "timeout:"); timeout.has_value() && *timeout > 0) {
			cfg.query_timeout = chrono::seconds{*timeout};
		} else if (auto attempts = parse_after_colon(token, "attempts:"); attempts.has_value() && *attempts > 0) {
			cfg.attempts = *attempts;
		} else if (auto ndots = parse_after_colon(token, "ndots:"); ndots.has_value()) {
			cfg.ndots = *ndots;
		}
	}
}
[[nodiscard]] V<S> parse_search_domains(
	SV rest) {
	V<S> out;
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		S token = trim_ascii_copy(end == SV::npos ? rest : rest.substr(0, end));
		if (end == SV::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}
		if (token.empty()) {
			continue;
		}
		for (char &c: token) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c + ('a' - 'A'));
			}
		}
		if (!token.empty() && token.back() == '.') {
			token.pop_back();
		}
		if (!token.empty()) {
			out.push_back(move(token));
		}
	}
	return out;
}
[[nodiscard]] Opt<S> read_small_text_file(
	fs::path const &path,
	SZ max_bytes = SZ{4} * 1024 * 1024) noexcept {
	try {
		S const native = path.string();
		auto bytes = read_file_at_sync(AT_FDCWD, native, max_bytes);
		if (!bytes) {
			return {};
		}
		return S{move(*bytes)};
	} catch (...) { return {}; }
}
template<class F>
void for_each_line(
	SV text,
	F &&fn) {
	while (!text.empty()) {
		auto const eol = text.find('\n');
		SV line = eol == SV::npos ? text : text.substr(0, eol);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		fn(S{line});
		if (eol == SV::npos) {
			break;
		}
		text.remove_prefix(eol + 1);
	}
}
[[nodiscard]] ResolvConfig parse_resolv_conf(
	fs::path const &path) noexcept {
	ResolvConfig out;
	auto const contents = read_small_text_file(path);
	if (!contents) {
		return out;
	}
	try {
		for_each_line(*contents, [&](S line) {
			if (auto comment = line.find_first_of("#;"); comment != S::npos) {
				line.resize(comment);
			}
			auto trimmed = trim_ascii_copy(line);
			if (trimmed.empty()) {
				return;
			}
			auto const split = trimmed.find_first_of(" \t");
			SV const key{trimmed.data(), split == S::npos ? trimmed.size() : split};
			SV rest{};
			if (split != S::npos) {
				rest = SV{trimmed.data() + split + 1, trimmed.size() - split - 1};
			}
			if (key == "nameserver") {
				auto const sv = trim_ascii_copy(rest);
				if (auto ns = parse_nameserver(sv); ns.has_value()) {
					out.nameservers.push_back(*ns);
				}
				return;
			}
			if (key == "options") {
				parse_resolv_options(rest, out);
				return;
			}
			if (key == "search") {
				out.search_domains = parse_search_domains(rest);
				return;
			}
		});
	} catch (...) {} // NOLINT(bugprone-empty-catch)
	return out;
}
[[nodiscard]] UM<S, V<Endpoint>> parse_hosts_file(
	fs::path const &path) noexcept {
	UM<S, V<Endpoint>> out;
	auto const contents = read_small_text_file(path);
	if (!contents) {
		return out;
	}
	try {
		for_each_line(*contents, [&](S line) {
			if (auto hash = line.find('#'); hash != S::npos) {
				line.resize(hash);
			}
			size_t pos = 0;
			auto skip_ws = [&] {
				while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
					++pos;
				}
			};
			auto next_token = [&]() -> SV {
				skip_ws();
				if (pos == line.size()) {
					return {};
				}
				size_t const start = pos;
				while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
					++pos;
				}
				return {line.data() + start, pos - start};
			};

			auto ip_sv = next_token();
			if (ip_sv.empty()) {
				return;
			}

			Endpoint ep{};
			::in_addr v4{};
			::in6_addr v6{};
			S const ip_str{ip_sv};
			if (::inet_pton(AF_INET, ip_str.c_str(), &v4) == 1) {
				auto *sin = reinterpret_cast<::sockaddr_in *>(&ep.addr);
				sin->sin_family = AF_INET;
				sin->sin_addr = v4;
				sin->sin_port = 0;
				ep.addr_len = sizeof(::sockaddr_in);
				ep.family = AddressFamily::v4;
			} else if (::inet_pton(AF_INET6, ip_str.c_str(), &v6) == 1) {
				auto *sin6 = reinterpret_cast<::sockaddr_in6 *>(&ep.addr);
				sin6->sin6_family = AF_INET6;
				sin6->sin6_addr = v6;
				sin6->sin6_port = 0;
				ep.addr_len = sizeof(::sockaddr_in6);
				ep.family = AddressFamily::v6;
			} else {
				return;
			}

			while (true) {
				auto name_sv = next_token();
				if (name_sv.empty()) {
					break;
				}
				S name{name_sv};
				for (char &c: name) {
					if (c >= 'A' && c <= 'Z') {
						c += 'a' - 'A';
					}
				}
				out[name].push_back(ep);
			}
		});
	} catch (...) {} // NOLINT(bugprone-empty-catch)
	return out;
}
[[nodiscard]] S lowercase_ascii(
	SV value) {
	S out{value};
	for (char &c: out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	return out;
}
[[nodiscard]] bool same_dns_peer(
	::sockaddr_storage const &actual,
	::socklen_t actual_len,
	NameserverEndpoint const &expected) noexcept {
	if (actual.ss_family != expected.addr.ss_family || actual_len == 0) {
		return false;
	}
	if (actual.ss_family == AF_INET) {
		if (actual_len < static_cast<::socklen_t>(sizeof(::sockaddr_in))
			|| expected.addr_len < static_cast<::socklen_t>(sizeof(::sockaddr_in))) {
			return false;
		}
		auto const *a = reinterpret_cast<::sockaddr_in const *>(&actual);
		auto const *e = reinterpret_cast<::sockaddr_in const *>(&expected.addr);
		return a->sin_port == e->sin_port && a->sin_addr.s_addr == e->sin_addr.s_addr;
	}
	if (actual.ss_family == AF_INET6) {
		if (actual_len < static_cast<::socklen_t>(sizeof(::sockaddr_in6))
			|| expected.addr_len < static_cast<::socklen_t>(sizeof(::sockaddr_in6))) {
			return false;
		}
		auto const *a = reinterpret_cast<::sockaddr_in6 const *>(&actual);
		auto const *e = reinterpret_cast<::sockaddr_in6 const *>(&expected.addr);
		return a->sin6_port == e->sin6_port
			&& std::memcmp(&a->sin6_addr, &e->sin6_addr, sizeof(::in6_addr)) == 0
			&& a->sin6_scope_id == e->sin6_scope_id;
	}
	return false;
}
[[nodiscard]] bool has_expected_question(
	codec::Message const &msg,
	u16 expected_id,
	SV expected_qname,
	codec::QType expected_qtype) noexcept {
	if (msg.header.id != expected_id || msg.questions.empty()) {
		return false;
	}
	auto const &q = msg.questions.front();
	return q.name == expected_qname && q.qtype == expected_qtype && q.qclass == codec::QClass::in;
}
void validate_accepted_response_status(
	codec::Message const &msg) {
	if (!msg.header.qr()) {
		throw DnsError{DnsErrorKind::malformed, "dns: response QR=0"};
	}
	if (auto k = codec::rcode_to_error(msg.header.rcode()); k.has_value()) {
		throw DnsError{
			*k,
			format("dns: RCODE {}", static_cast<u8>(msg.header.rcode())),
			0,
			Opt<u8>{static_cast<u8>(msg.header.rcode())}};
	}
	if (msg.header.tc()) {
		throw DnsError{DnsErrorKind::truncated, "dns: response TC=1"};
	}
}
struct DnsQueryState {
	mutex m;
	Opt<root::TaskControl> active;
	Atom<bool> cancel_requested{false};
	void set_active(
		root::TaskControl c) {
		Opt<root::TaskControl> to_cancel;
		{
			SL lk{m};
			active.emplace(move(c));
			if (cancel_requested.load(memory_order_acquire)) {
				to_cancel = active;
			}
		}
		if (to_cancel) {
			auto _ = to_cancel->request_cancel();
		}
	}
	void clear_active() {
		SL lk{m};
		active.reset();
	}
	void cancel() {
		Opt<root::TaskControl> to_cancel;
		{
			SL lk{m};
			cancel_requested.store(true, memory_order_release);
			to_cancel = active;
		}
		if (to_cancel) {
			auto _ = to_cancel->request_cancel();
		}
	}
	[[nodiscard]] bool cancelled() const noexcept { return cancel_requested.load(memory_order_acquire); }
};
struct ActiveTaskGuard {
	DnsQueryState &state;
	explicit ActiveTaskGuard(
		DnsQueryState &s,
		root::TaskControl c)
		: state{s} {
		state.set_active(move(c));
	}
	~ActiveTaskGuard() {
		try {
			state.clear_active();
		} catch (...) {}
	}
	ActiveTaskGuard(ActiveTaskGuard const &) = delete;
	ActiveTaskGuard &operator =(ActiveTaskGuard const &) = delete;
};
[[nodiscard]] root::Task<void> run_udp_query_driver(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	V<u8> wire,
	u16 expected_id,
	S expected_qname,
	codec::QType expected_qtype,
	chrono::milliseconds timeout,
	SP<root::TaskSource<codec::Message>> src,
	SP<DnsQueryState> state) {
	constexpr SZ kRxSize = 4096;
	auto check_cancelled = [&] {
		if (state->cancelled()) {
			throw DnsError{DnsErrorKind::cancelled, "dns: query cancelled"};
		}
	};
	try {
		check_cancelled();
		UdpSocket sock = UdpSocket::ephemeral(ring, static_cast<int>(ns.addr.ss_family));
		A<u8, kRxSize> rx_buf{};
		// P1-08: UDP send cancel not covered; cancellation detected on next recv
		co_await sock.send_to_borrowed(span<u8 const>{wire.data(), wire.size()}, ns.addr, ns.addr_len);
		check_cancelled();
		auto const deadline = chrono::steady_clock::now() + timeout;
		for (;;) {
			auto const now = chrono::steady_clock::now();
			if (now >= deadline) {
				throw DnsError{DnsErrorKind::timeout, "dns: query timed out"};
			}
			auto const remaining = chrono::ceil<chrono::milliseconds>(deadline - now);
			auto recv_task = sock.recv_from(span<u8>{rx_buf.data(), rx_buf.size()}, remaining);
			ActiveTaskGuard g{*state, recv_task.control()};
			auto const result = co_await move(recv_task);
			auto msg = codec::decode_message(span<u8 const>{rx_buf.data(), result.bytes});
			if (!same_dns_peer(result.from, result.from_len, ns)) {
				continue;
			}
			if (!has_expected_question(msg, expected_id, expected_qname, expected_qtype)) {
				continue;
			}
			validate_accepted_response_status(msg);
			check_cancelled();
			auto _ = src->try_set_value(root::Success<codec::Message>{move(msg)});
			co_return;
		}
	} catch (root::CancelledError const &) {
		auto _ = src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
	} catch (DnsError const &) { auto _ = src->try_set_exception(current_exception()); } catch (IoError const &e) {
		if (e.code().value() == ECANCELED) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
		} else if (e.code().value() == ETIMEDOUT) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: query timed out"}));
		} else {
			auto _ = src->try_set_exception(make_exception_ptr(
				DnsError{DnsErrorKind::network, format("dns: udp error: {}", e.what()), e.code().value()}));
		}
	} catch (...) {
		auto _ = src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::network, "dns: udp query failed"}));
	}
}
// Single UDP DNS query: plain factory. Driver coroutine owns all buffers and
// drives the send/recv loop; out_task has the cancel hook.
[[nodiscard]] root::Task<codec::Message> udp_single_query(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	V<u8> wire,
	u16 expected_id,
	S expected_qname,
	codec::QType expected_qtype,
	chrono::milliseconds timeout) {
	auto [out_task, raw_src] = root::make_task_source<codec::Message>(root::SubmitOptions{.enable_cancellation = true});
	auto src = make_shared<root::TaskSource<codec::Message>>(move(raw_src));
	auto state = make_shared<DnsQueryState>();
	auto _ = src->install_cancel_hook([state](root::CancelReason) noexcept {
		try {
			state->cancel();
		} catch (...) {}
	});
	auto driver = run_udp_query_driver(
		ring,
		ns,
		move(wire),
		expected_id,
		move(expected_qname),
		expected_qtype,
		timeout,
		src,
		state);
	move(driver).detach();
	return move(out_task);
}
[[nodiscard]] root::Task<void> run_tcp_query_driver(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	V<u8> wire,
	u16 expected_id,
	S expected_qname,
	codec::QType expected_qtype,
	chrono::milliseconds timeout,
	SP<root::TaskSource<codec::Message>> src,
	SP<DnsQueryState> state) {
	auto check_cancelled = [&] {
		if (state->cancelled()) {
			throw DnsError{DnsErrorKind::cancelled, "dns: tcp query cancelled"};
		}
	};
	try {
		if (timeout.count() <= 0) {
			auto _ = src->try_set_exception(
				make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: tcp query requires positive timeout"}));
			co_return;
		}
		check_cancelled();
		V<u8> framed;
		framed.reserve(2 + wire.size());
		auto const wlen = static_cast<u16>(wire.size());
		framed.push_back(static_cast<u8>(wlen >> 8U));
		framed.push_back(static_cast<u8>(wlen & 0xFFU));
		framed.insert(framed.end(), wire.begin(), wire.end());
		int const family = static_cast<int>(ns.addr.ss_family);
		auto const deadline = chrono::steady_clock::now() + timeout;
		auto remaining_or_throw = [&]() -> chrono::milliseconds {
			auto const now = chrono::steady_clock::now();
			if (now >= deadline) {
				throw DnsError{DnsErrorKind::timeout, "dns: tcp query timed out"};
			}
			return chrono::ceil<chrono::milliseconds>(deadline - now);
		};
		ConnectOptions copts{};
		copts.timeout = timeout;
		TcpStream stream{};
		{
			auto connect_task = tcp_connect(ring, family, ns.addr, ns.addr_len, copts);
			ActiveTaskGuard g{*state, connect_task.control()};
			stream = co_await move(connect_task);
		}
		check_cancelled();
		{
			SZ sent = 0;
			while (sent < framed.size()) {
				auto write_task = stream.write_borrowed(span<u8 const>{framed.data() + sent, framed.size() - sent});
				ActiveTaskGuard g{*state, write_task.control()};
				SZ const n = co_await move(write_task);
				if (n == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp write failed"};
				}
				sent += n;
			}
		}
		check_cancelled();
		A<u8, 2> len_buf{};
		{
			SZ n = 0;
			while (n < 2) {
				root::Task<SZ> recv_task =
					stream.recv_borrowed(span<u8>{len_buf.data() + n, 2 - n}, remaining_or_throw());
				ActiveTaskGuard g{*state, recv_task.control()};
				SZ const got = co_await move(recv_task);
				if (got == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp short length prefix"};
				}
				n += got;
			}
		}
		u16 const resp_len = static_cast<u16>((static_cast<u16>(len_buf[0]) << 8U) | static_cast<u16>(len_buf[1]));
		if (resp_len == 0) {
			throw DnsError{DnsErrorKind::malformed, "dns: tcp zero-length response"};
		}
		V<u8> resp_buf(resp_len);
		{
			SZ resp_n = 0;
			while (resp_n < static_cast<SZ>(resp_len)) {
				root::Task<SZ> recv_task = stream.recv_borrowed(
					span<u8>{resp_buf.data() + resp_n, static_cast<SZ>(resp_len) - resp_n},
					remaining_or_throw());
				ActiveTaskGuard g{*state, recv_task.control()};
				SZ const got = co_await move(recv_task);
				if (got == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp short response"};
				}
				resp_n += got;
			}
		}
		auto msg = codec::decode_message(span<u8 const>{resp_buf.data(), static_cast<SZ>(resp_len)});
		if (!has_expected_question(msg, expected_id, expected_qname, expected_qtype)) {
			throw DnsError{DnsErrorKind::malformed, "dns: tcp response mismatch"};
		}
		validate_accepted_response_status(msg);
		check_cancelled();
		auto _ = src->try_set_value(root::Success<codec::Message>{move(msg)});
	} catch (root::CancelledError const &) {
		auto _ =
			src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: tcp query cancelled"}));
	} catch (DnsError const &) { auto _ = src->try_set_exception(current_exception()); } catch (IoError const &e) {
		if (e.code().value() == ECANCELED) {
			auto _ = src->try_set_exception(
				make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: tcp query cancelled"}));
		} else if (e.code().value() == ETIMEDOUT) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: tcp query timed out"}));
		} else {
			auto _ = src->try_set_exception(make_exception_ptr(
				DnsError{DnsErrorKind::network, format("dns: tcp error: {}", e.what()), e.code().value()}));
		}
	} catch (...) {
		auto _ = src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::network, "dns: tcp query failed"}));
	}
}
// TCP DNS query per RFC 1035 §4.2.2: plain factory; driver owns all buffers.
[[nodiscard]] root::Task<codec::Message> tcp_single_query(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	V<u8> wire,
	u16 expected_id,
	S expected_qname,
	codec::QType expected_qtype,
	chrono::milliseconds timeout) {
	auto [out_task, raw_src] = root::make_task_source<codec::Message>(root::SubmitOptions{.enable_cancellation = true});
	auto src = make_shared<root::TaskSource<codec::Message>>(move(raw_src));
	auto state = make_shared<DnsQueryState>();
	auto _ = src->install_cancel_hook([state](root::CancelReason) noexcept {
		try {
			state->cancel();
		} catch (...) {}
	});
	auto driver = run_tcp_query_driver(
		ring,
		ns,
		move(wire),
		expected_id,
		move(expected_qname),
		expected_qtype,
		timeout,
		src,
		state);
	move(driver).detach();
	return move(out_task);
}
// Minimum TTL across all answer RRs of the given family (UINT32_MAX if none).
[[nodiscard]] u32 min_answer_ttl(
	codec::Message const &msg,
	AddressFamily family) noexcept {
	u32 min_ttl = NL<u32>::max();
	for (auto const &rr: msg.answers) {
		auto const is_match = (family == AddressFamily::v4 && rr.type == codec::QType::a)
						   || (family == AddressFamily::v6 && rr.type == codec::QType::aaaa);
		if (is_match) {
			min_ttl = min(min_ttl, rr.ttl);
		}
	}
	return min_ttl;
}

// Ordered by severity so max(a,b) gives the dominant failure reason.
enum class BatchFailReason : u8 {
	none = 0,
	timeout = 1,
	network = 2,
	nxdomain = 3,
	truncated = 4,
};
struct EndpointBatch {
	V<Endpoint> eps;
	u32 min_ttl{NL<u32>::max()};
	BatchFailReason fail_reason{BatchFailReason::none};
	bool was_queried{false};
};
// ─── UDP flow builder (shared by resolve() and resolve_blocking()) ──────────

// Build a Task<EndpointBatch> for a single address family. Errors are absorbed
// into a BatchFailReason field so the parallel join (join_all) always completes.
// Cancellation is the only exception that still propagates.
[[nodiscard]] root::Task<EndpointBatch> build_family_flow(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	SV hostname,
	u16 port,
	u16 qid,
	codec::QType qtype,
	AddressFamily fam,
	chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	auto wire = codec::encode_query(qid, hostname, qtype, edns);
	auto wire_ptr = make_shared<V<u8>>(wire); // copy for TCP fallback
	auto expected_qname = lowercase_ascii(hostname);
	bool needs_tcp_fallback = false;
	try {
		auto udp_task = udp_single_query(ring, ns, move(wire), qid, expected_qname, qtype, timeout);
		auto const msg = co_await move(udp_task);
		EndpointBatch batch;
		batch.was_queried = true;
		batch.min_ttl = min_answer_ttl(msg, fam);
		for (auto const &rr: msg.answers) {
			if (auto ep = codec::rdata_to_endpoint(rr, port); ep.has_value() && ep->family == fam) {
				batch.eps.push_back(*ep);
			}
		}
		co_return batch;
	} catch (DnsError const &de) {
		if (de.kind == DnsErrorKind::cancelled) {
			throw;
		}
		if (de.kind == DnsErrorKind::truncated) {
			needs_tcp_fallback = true;
		} else {
			auto const r = (de.kind == DnsErrorKind::nxdomain) ? BatchFailReason::nxdomain :
						   (de.kind == DnsErrorKind::timeout)  ? BatchFailReason::timeout :
																 BatchFailReason::network;
			co_return EndpointBatch{.fail_reason = r, .was_queried = true};
		}
	} catch (...) { co_return EndpointBatch{.fail_reason = BatchFailReason::network, .was_queried = true}; }
	auto _ = needs_tcp_fallback; // always true here
	// TCP fallback (co_await must be outside catch block):
	try {
		auto tcp_task = tcp_single_query(ring, ns, *wire_ptr, qid, lowercase_ascii(hostname), qtype, timeout);
		auto const msg2 = co_await move(tcp_task);
		EndpointBatch b;
		b.was_queried = true;
		b.min_ttl = min_answer_ttl(msg2, fam);
		for (auto const &rr: msg2.answers) {
			if (auto ep = codec::rdata_to_endpoint(rr, port); ep.has_value() && ep->family == fam) {
				b.eps.push_back(*ep);
			}
		}
		co_return b;
	} catch (...) { co_return EndpointBatch{.fail_reason = BatchFailReason::truncated, .was_queried = true}; }
}
// Immediate empty batch for a disabled address family (was_queried=false).
[[nodiscard]] root::Task<EndpointBatch> make_empty_batch_task() {
	co_return EndpointBatch{};
}
// Fire A and AAAA queries in parallel (RFC 8305 §3). Connection-attempt
// staggering belongs in the caller's connect loop, not here.
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	S const &hostname,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	static thread_local std::mt19937 tl_rng{std::random_device{}()};
	u16 const qid_a = static_cast<u16>(tl_rng() & 0xFFFFU);
	u16 const qid_aaaa = static_cast<u16>((static_cast<u32>(qid_a) + 1U) & 0xFFFFU);
	auto v4_task = do_v4 ? build_family_flow(ring, ns, hostname, port, qid_a, codec::QType::a, AddressFamily::v4, timeout, edns) :
			make_empty_batch_task();
	auto v6_task = do_v6 ? build_family_flow(
				ring,
				ns,
				hostname,
				port,
				qid_aaaa,
				codec::QType::aaaa,
				AddressFamily::v6,
				timeout,
				edns) :
			make_empty_batch_task();
	auto [v4, v6] = co_await join_all(move(v4_task), move(v6_task));
	if (v4.eps.empty() && v6.eps.empty()) {
		// Both families have no results. Propagate the dominant failure.
		auto const w =
			(static_cast<u8>(v4.fail_reason) >= static_cast<u8>(v6.fail_reason)) ? v4.fail_reason : v6.fail_reason;
		if (w == BatchFailReason::truncated) {
			throw DnsError{DnsErrorKind::truncated, "dns: udp truncated and tcp fallback failed"};
		}
		if (w == BatchFailReason::nxdomain) {
			throw DnsError{DnsErrorKind::nxdomain, "dns: name not found"};
		}
	}
	V<Endpoint> all;
	all.reserve(v6.eps.size() + v4.eps.size());
	auto append_all = [&all](V<Endpoint> const &eps) {
		for (auto const &ep: eps) {
			all.push_back(ep);
		}
	};
	if (prefer == AddressFamily::v4) {
		append_all(v4.eps);
		append_all(v6.eps);
	} else {
		append_all(v6.eps);
		append_all(v4.eps);
	}
	u32 const min_ttl = min(v4.min_ttl, v6.min_ttl);
	ResolveResult r;
	r.endpoints = move(all);
	if (min_ttl != NL<u32>::max()) {
		r.suggested_ttl = chrono::seconds{min_ttl};
	}
	co_return r;
}
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow_with_nameservers(
	SocketTaskRing &ring,
	V<NameserverEndpoint> nameservers,
	size_t index,
	S hostname,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	if (index >= nameservers.size()) {
		throw DnsError{DnsErrorKind::no_servers, "dns: no nameservers configured"};
	}
	auto const ns = nameservers[index];
	auto query_host = hostname;
	auto udp_task = build_native_udp_flow(ring, ns, query_host, port, do_v4, do_v6, prefer, timeout, edns);
	auto result = co_await move(udp_task);
	if (!result.endpoints.empty() || index + 1 >= nameservers.size()) {
		co_return move(result);
	}
	auto next_task = build_native_udp_flow_with_nameservers(
		ring,
		move(nameservers),
		index + 1,
		move(hostname),
		port,
		do_v4,
		do_v6,
		prefer,
		timeout,
		edns);
	co_return co_await move(next_task);
}
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow_with_candidates(
	SocketTaskRing &ring,
	V<NameserverEndpoint> nameservers,
	V<S> candidates,
	size_t index,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	if (index >= candidates.size()) {
		throw DnsError{DnsErrorKind::nxdomain, "dns: name not found"};
	}
	auto candidate = candidates[index];
	auto query_nameservers = nameservers;
	bool try_next = false;
	try {
		auto ns_task = build_native_udp_flow_with_nameservers(
			ring,
			move(query_nameservers),
			0,
			candidate,
			port,
			do_v4,
			do_v6,
			prefer,
			timeout,
			edns);
		co_return co_await move(ns_task);
	} catch (DnsError const &de) {
		if (de.kind != DnsErrorKind::nxdomain || index + 1 >= candidates.size()) {
			throw;
		}
		try_next = true;
	}
	auto _ = try_next; // always true here
	// recursive fallback outside catch block:
	auto next_task = build_native_udp_flow_with_candidates(
		ring,
		move(nameservers),
		move(candidates),
		index + 1,
		port,
		do_v4,
		do_v6,
		prefer,
		timeout,
		edns);
	co_return co_await move(next_task);
}
// ─── LRU TTL cache ──────────────────────────────────────────────────────────

struct DnsCacheEntry {
	ResolveResult result;
	chrono::steady_clock::time_point expires;
};
class LruDnsCache {
	using List = std::list<std::pair<S, DnsCacheEntry>>;
	size_t capacity_;
	List order_;
	UM<S, List::iterator> index_;
	mutable mutex mtx_;

public:
	explicit LruDnsCache(
		size_t cap)
		: capacity_{cap} {}
	[[nodiscard]] Opt<ResolveResult> get(
		S const &key) {
		std::scoped_lock const lk{mtx_};
		auto it = index_.find(key);
		if (it == index_.end()) {
			return nullopt;
		}
		if (chrono::steady_clock::now() >= it->second->second.expires) {
			order_.erase(it->second);
			index_.erase(it);
			return nullopt;
		}
		order_.splice(order_.begin(), order_, it->second);
		return it->second->second.result;
	}
	void put(
		S const &key,
		ResolveResult result,
		chrono::seconds ttl) {
		auto const expires = chrono::steady_clock::now() + ttl;
		std::scoped_lock const lk{mtx_};
		auto it = index_.find(key);
		if (it != index_.end()) {
			it->second->second = {move(result), expires};
			order_.splice(order_.begin(), order_, it->second);
			return;
		}
		if (order_.size() >= capacity_) {
			auto lru = std::prev(order_.end());
			index_.erase(lru->first);
			order_.erase(lru);
		}
		order_.push_front({
			key,
			{move(result), expires}
        });
		index_[key] = order_.begin();
	}
	void invalidate_by_host(
		SV host) {
		S const prefix = format("{}:", host);
		std::scoped_lock const lk{mtx_};
		for (auto it = order_.begin(); it != order_.end();) {
			if (it->first.starts_with(prefix)) {
				index_.erase(it->first);
				it = order_.erase(it);
			} else {
				++it;
			}
		}
	}
	void clear() {
		std::scoped_lock const lk{mtx_};
		order_.clear();
		index_.clear();
	}
};
[[nodiscard]] S make_cache_key(
	SV host,
	u16 port,
	AddressFamily prefer,
	bool v4,
	bool v6) {
	return format("{}:{}:{}{}{}", host, port, prefer == AddressFamily::v4 ? '4' : '6', v4 ? '4' : '-', v6 ? '6' : '-');
}
[[nodiscard]] chrono::milliseconds effective_native_timeout(
	ResolveOptions const &opts) noexcept {
	if (opts.query_timeout.count() <= 0) {
		return opts.total_timeout;
	}
	if (opts.total_timeout.count() <= 0) {
		return opts.query_timeout;
	}
	return min(opts.query_timeout, opts.total_timeout);
}
[[nodiscard]] ResolveOptions apply_resolv_defaults(
	ResolveOptions opts,
	chrono::milliseconds resolv_query_timeout) noexcept {
	ResolveOptions const defaults;
	if (opts.query_timeout == defaults.query_timeout && resolv_query_timeout.count() > 0) {
		opts.query_timeout = resolv_query_timeout;
	}
	return opts;
}
[[nodiscard]] V<NameserverEndpoint> nameservers_with_attempts(
	V<NameserverEndpoint> const &base,
	size_t attempts) {
	V<NameserverEndpoint> out;
	if (base.empty()) {
		return out;
	}
	attempts = max<size_t>(attempts, 1);
	out.reserve(base.size() * attempts);
	for (size_t attempt = 0; attempt < attempts; ++attempt) {
		out.insert(out.end(), base.begin(), base.end());
	}
	return out;
}
[[nodiscard]] V<S> resolve_candidates(
	SV host,
	V<S> const &search_domains,
	size_t ndots) {
	S normalized{host};
	if (!normalized.empty() && normalized.back() == '.') {
		normalized.pop_back();
		return {move(normalized)};
	}
	auto const dot_count = static_cast<size_t>(ranges::count(normalized, '.'));
	if (search_domains.empty() || dot_count >= ndots) {
		return {move(normalized)};
	}
	V<S> out;
	out.reserve(search_domains.size() + 1);
	for (auto const &domain: search_domains) {
		out.push_back(format("{}.{}", normalized, domain));
	}
	out.push_back(move(normalized));
	return out;
}
// ─── Resolver::Impl ─────────────────────────────────────────────────────────

struct CoalescedBroadcast {
	V<SP<root::TaskSource<ResolveResult>>> waiters;
};
struct InFlightKey {
	S cache_key;
	SocketTaskRing *ring{};
};
struct InFlightKeyHash {
	size_t operator ()(
		InFlightKey const &k) const noexcept {
		size_t h1 = hash<S>{}(k.cache_key);
		size_t h2 = hash<void const *>{}(k.ring);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
	}
};
struct InFlightKeyEq {
	bool operator ()(
		InFlightKey const &a,
		InFlightKey const &b) const noexcept {
		return a.ring == b.ring && a.cache_key == b.cache_key;
	}
};
struct Resolver::Impl {
	ResolverBackend backend{};
	UP<FileReader> reader{};
	UP<SocketTaskRing> task_ring{};
	WorkPool *pool{nullptr};
	ResolverOptions opts;
	V<NameserverEndpoint> nameservers;
	V<S> search_domains;
	size_t ndots{1};
	chrono::milliseconds resolv_query_timeout{0};
	size_t attempts{1};
	UM<S, V<Endpoint>> hosts_cache;
	SP<LruDnsCache> cache{};
	std::unordered_map<InFlightKey, CoalescedBroadcast, InFlightKeyHash, InFlightKeyEq> in_flight;
	mutex in_flight_mutex;
};
Resolver::Resolver(
	::io_uring *ring,
	CompletionTable *completions,
	UserDataFn encode_ud,
	ResolverOptions opts)
	: impl_{std::make_shared<Impl>()} {
	impl_->backend = ResolverBackend::native_udp;
	auto shared_ud = make_shared<UserDataFn>(move(encode_ud));
	impl_->reader =
		make_unique<FileReader>(ring, completions, [shared_ud](u32 s, u32 g) -> u64 { return (*shared_ud)(s, g); });
	impl_->task_ring = make_unique<SocketTaskRing>(SocketRawRing{ring}, *completions, [shared_ud](u32 s, u32 g) -> u64 {
		return (*shared_ud)(s, g);
	});
	impl_->opts = move(opts);
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	impl_->nameservers =
		impl_->opts.override_nameservers.empty() ? resolv.nameservers : impl_->opts.override_nameservers;
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	if (impl_->opts.cache_capacity > 0) {
		impl_->cache = make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}
Resolver::Resolver(
	WorkPool &pool,
	ResolverOptions opts)
	: impl_{make_shared<Impl>()} {
	impl_->backend = ResolverBackend::nss_thread;
	impl_->pool = &pool;
	impl_->opts = move(opts);
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	impl_->nameservers =
		impl_->opts.override_nameservers.empty() ? resolv.nameservers : impl_->opts.override_nameservers;
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	if (impl_->opts.cache_capacity > 0) {
		impl_->cache = make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}
Resolver::~Resolver() = default;
root::Task<ResolveResult> Resolver::resolve_flow(
	SocketTaskRing *external_ring,
	SV host,
	u16 port,
	ResolveOptions const &per_opts) {
	auto const effective_opts = apply_resolv_defaults(per_opts, impl_->resolv_query_timeout);
	if (auto ep = try_parse_ip_literal(host, port); ep.has_value()) {
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		ResolveResult r;
		r.endpoints.push_back(*ep);
		auto _ = raw_src.try_set_value(root::Success<ResolveResult>{move(r)});
		return move(task);
	}

	if (!is_valid_hostname(host)) {
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto _ = raw_src.try_set_exception(
			make_exception_ptr(DnsError{DnsErrorKind::invalid_hostname, format("invalid hostname '{}'", host)}));
		return move(task);
	}

	// /etc/hosts lookup
	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
		S key{host};
		for (char &c: key) {
			if (c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';
			}
		}
		if (!key.empty() && key.back() == '.') {
			key.pop_back();
		}
		auto it = impl_->hosts_cache.find(key);
		if (it != impl_->hosts_cache.end()) {
			V<Endpoint> eps;
			for (auto const &ep: it->second) {
				if (ep.family == AddressFamily::v4 && effective_opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && effective_opts.allow_v6) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in6 *>(&e.addr)->sin6_port = htons(port);
					eps.push_back(e);
				}
			}
			if (!eps.empty()) {
				auto [task, raw_src] =
					root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
				ResolveResult r;
				r.endpoints = move(eps);
				r.from_hosts_file = true;
				auto _ = raw_src.try_set_value(root::Success<ResolveResult>{move(r)});
				return move(task);
			}
		}
	}

	S const cache_key =
		effective_opts.bypass_cache ?
			S{} :
			make_cache_key(host, port, effective_opts.prefer, effective_opts.allow_v4, effective_opts.allow_v6);
	InFlightKey const inflight_key{cache_key, external_ring};

	// LRU cache lookup
	if (impl_->cache && !cache_key.empty()) {
		if (auto hit = impl_->cache->get(cache_key); hit.has_value()) {
			auto [task, raw_src] =
				root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
			if (hit->is_negative) {
				auto _ = raw_src.try_set_exception(
					make_exception_ptr(DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"}));
				return move(task);
			}
			hit->from_cache = true;
			auto _ = raw_src.try_set_value(root::Success<ResolveResult>{move(*hit)});
			return move(task);
		}
	}

	auto cache_insert = [cache = impl_->cache, cache_key = cache_key, max_ttl = impl_->opts.cache_max_ttl](
							ResolveResult r) -> ResolveResult { // NOLINT(bugprone-exception-escape)
		try {
			if (cache && !cache_key.empty() && !r.endpoints.empty()) {
				auto const ttl = (r.suggested_ttl.count() > 0) ? min(r.suggested_ttl, max_ttl) : max_ttl;
				cache->put(cache_key, r, ttl);
			}
		} catch (...) {} // NOLINT(bugprone-empty-catch)
		return r;
	};

	if (impl_->backend == ResolverBackend::nss_thread) {
		if (external_ring != nullptr) {
			auto [task, raw_src] =
				root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
			auto _ = raw_src.try_set_exception(make_exception_ptr(
				DnsError{
					DnsErrorKind::not_implemented,
					"resolve: nss_thread resolver does not support caller-provided ring"}));
			return move(task);
		}
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<ResolveResult>>(move(raw_src));
		bool const ok = impl_->pool->enqueue([shared_src, // NOLINT(bugprone-exception-escape)
											  h = S{host},
											  port,
											  allow_v4 = effective_opts.allow_v4,
											  allow_v6 = effective_opts.allow_v6,
											  cache = impl_->cache,
											  cache_key = cache_key,
											  ttl = impl_->opts.cache_max_ttl]() mutable {
			try {
				addrinfo hints{};
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_flags = AI_ADDRCONFIG;
				addrinfo *res_raw = nullptr;
				S const p = to_string(port);
				int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res_raw);
				if (gai != 0 || res_raw == nullptr) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(
						DnsError{DnsErrorKind::nxdomain, format("getaddrinfo: {}", ::gai_strerror(gai))}));
					return;
				}
				UniqueAddrInfo const res{res_raw};
				ResolveResult result;
				for (auto *rp = res.get(); rp != nullptr; rp = rp->ai_next) {
					if (rp->ai_family == AF_INET && allow_v4) {
						Endpoint ep{};
						ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
						ep.family = AddressFamily::v4;
						std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
						result.endpoints.push_back(ep);
					} else if (rp->ai_family == AF_INET6 && allow_v6) {
						Endpoint ep{};
						ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
						ep.family = AddressFamily::v6;
						std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
						result.endpoints.push_back(ep);
					}
				}
				if (result.endpoints.empty()) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(
						DnsError{DnsErrorKind::nxdomain, format("no usable addresses for '{}'", h)}));
					return;
				}
				if (cache && !cache_key.empty() && !result.endpoints.empty()) {
					cache->put(cache_key, result, ttl);
				}
				auto _ = shared_src->try_set_value(root::Success<ResolveResult>{move(result)});
			} catch (...) {
				auto _ = shared_src->try_set_exception(current_exception());
			} // NOLINT(bugprone-empty-catch)
		});
		if (!ok) {
			auto _ = shared_src->try_set_exception(
				make_exception_ptr(DnsError{DnsErrorKind::cancelled, "nss_thread: work pool not accepting jobs"}));
		}
		return move(task);
	}

	auto const base_ns =
		effective_opts.override_nameservers.empty() ? impl_->nameservers : effective_opts.override_nameservers;
	auto const ns_list = nameservers_with_attempts(base_ns, impl_->attempts);

	if (ns_list.empty()) {
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto _ = raw_src.try_set_exception(
			make_exception_ptr(DnsError{DnsErrorKind::no_servers, "resolve: no nameservers configured"}));
		return move(task);
	}

	SocketTaskRing *task_ring = external_ring ? external_ring : impl_->task_ring.get();
	if (task_ring == nullptr) {
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto _ = raw_src.try_set_exception(
			make_exception_ptr(DnsError{DnsErrorKind::no_ring, "resolve: no ring available"}));
		return move(task);
	}

	Opt<root::Task<ResolveResult>> coalesced_out;
	bool max_inflight_exceeded = false;
	{
		lock_guard lock{impl_->in_flight_mutex};
		if (impl_->in_flight.size() >= impl_->opts.max_in_flight_queries) {
			max_inflight_exceeded = true;
		} else if (!inflight_key.cache_key.empty()) {
			if (auto it = impl_->in_flight.find(inflight_key); it != impl_->in_flight.end()) {
				auto [wtask, wraw_src] =
					root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
				auto shared_waiter = make_shared<root::TaskSource<ResolveResult>>(move(wraw_src));
				it->second.waiters.push_back(shared_waiter);
				auto [out_task, out_raw_src] =
					root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
				auto out_src = make_shared<root::TaskSource<ResolveResult>>(move(out_raw_src));
				[](SP<root::TaskSource<ResolveResult>> out_src, root::Task<ResolveResult> wt) -> root::Task<void> {
					try {
						auto r = co_await move(wt);
						r.from_coalesced = true;
						auto _ = out_src->try_set_value(root::Success<ResolveResult>{move(r)});
					} catch (Cancelled const &) {
						auto _ = out_src->try_set_exception(
							make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
					} catch (...) { auto _ = out_src->try_set_exception(current_exception()); }
				}(out_src, move(wtask))
																									 .detach();
				coalesced_out = move(out_task);
			} else {
				impl_->in_flight.emplace(inflight_key, CoalescedBroadcast{});
			}
		}
	}
	if (max_inflight_exceeded) {
		auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto _ = raw_src.try_set_exception(
			make_exception_ptr(DnsError{DnsErrorKind::cancelled, "resolve: max in-flight queries exceeded"}));
		return move(task);
	}
	if (coalesced_out.has_value()) {
		return move(*coalesced_out);
	}
	auto const timeout = effective_native_timeout(effective_opts);
	codec::Edns0Options const edns{.udp_size = impl_->opts.edns0_udp_size};
	bool const do_v4 = effective_opts.allow_v4;
	bool const do_v6 = effective_opts.allow_v6;
	auto const candidates = resolve_candidates(host, impl_->search_domains, impl_->ndots);

	auto fanout_success = // NOLINT(bugprone-exception-escape)
		[impl = impl_, inflight_key](ResolveResult r) -> ResolveResult {
		auto impl_keep = impl;
		V<SP<root::TaskSource<ResolveResult>>> waiters;
		if (!inflight_key.cache_key.empty()) {
			lock_guard lock{impl_keep->in_flight_mutex};
			if (auto it = impl_keep->in_flight.find(inflight_key); it != impl_keep->in_flight.end()) {
				waiters = move(it->second.waiters);
				impl_keep->in_flight.erase(it);
			}
		}
		for (auto const &w: waiters) {
			auto copy = r;
			copy.from_coalesced = true;
			auto _ = w->try_set_value(root::Success<ResolveResult>{move(copy)});
		}
		return r;
	};

	auto fanout_error = // NOLINT(bugprone-exception-escape)
		[impl = impl_, inflight_key](std::exception_ptr const &ep) -> ResolveResult {
		auto impl_keep = impl;
		V<SP<root::TaskSource<ResolveResult>>> waiters;
		if (!inflight_key.cache_key.empty()) {
			lock_guard lock{impl_keep->in_flight_mutex};
			if (auto it = impl_keep->in_flight.find(inflight_key); it != impl_keep->in_flight.end()) {
				waiters = move(it->second.waiters);
				impl_keep->in_flight.erase(it);
			}
		}
		for (auto const &w: waiters) {
			auto _ = w->try_set_exception(ep);
		}
		// Negative caching: store NXDOMAIN entries so repeat lookups skip the wire.
		try {
			rethrow_exception(ep);
		} catch (DnsError const &de) {
			if (de.kind == DnsErrorKind::nxdomain && impl_keep->cache && !inflight_key.cache_key.empty()) {
				ResolveResult neg;
				neg.is_negative = true;
				try {
					impl_keep->cache->put(inflight_key.cache_key, neg, impl_keep->opts.cache_negative_ttl);
				} catch (...) {} // NOLINT(bugprone-empty-catch)
			}
			throw;
		}
		return {};
	};

	auto [out_task, out_raw_src] =
		root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
	auto out_src = make_shared<root::TaskSource<ResolveResult>>(move(out_raw_src));
	[](SP<root::TaskSource<ResolveResult>> out_src,
	   root::Task<ResolveResult> inner,
	   auto cache_insert,
	   auto fanout_success,
	   auto fanout_error,
	   SP<Resolver::Impl> impl,
	   InFlightKey inflight_key) mutable -> root::Task<void> {
		try {
			auto out = out_src;
			auto r = co_await move(inner);
			r = cache_insert(move(r));
			r = fanout_success(move(r));
			auto _ = out->try_set_value(root::Success<ResolveResult>{move(r)});
		} catch (Cancelled const &) {
			auto out = out_src;
			auto key = inflight_key;
			V<SP<root::TaskSource<ResolveResult>>> waiters;
			if (!key.cache_key.empty()) {
				lock_guard lock{impl->in_flight_mutex};
				if (auto it = impl->in_flight.find(key); it != impl->in_flight.end()) {
					waiters = move(it->second.waiters);
					impl->in_flight.erase(it);
				}
			}
			auto cancelled = make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"});
			for (auto const &w: waiters) {
				auto _ = w->try_set_exception(cancelled);
			}
			auto _ =
				out->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
		} catch (...) {
			auto out = out_src;
			try {
				fanout_error(current_exception());
			} catch (...) { auto _ = out->try_set_exception(current_exception()); }
		}
	}(out_src,
	  build_native_udp_flow_with_candidates(
		  *task_ring,
		  ns_list,
		  candidates,
		  0,
		  port,
		  do_v4,
		  do_v6,
		  effective_opts.prefer,
		  timeout,
		  edns),
	  move(cache_insert),
	  move(fanout_success),
	  move(fanout_error),
	  impl_,
	  inflight_key)
												.detach();
	return move(out_task);
}
conflux::work::root::Task<ResolveResult> Resolver::resolve(
	SV host,
	u16 port,
	ResolveOptions const &opts) {
	return resolve_flow(nullptr, host, port, opts);
}
conflux::work::root::Task<ResolveResult> Resolver::resolve(
	SocketTaskRing &ring,
	SV host,
	u16 port,
	ResolveOptions const &opts) {
	return resolve_flow(&ring, host, port, opts);
}
namespace {

struct TlsRingBase {
	::io_uring ring{};
	CompletionTable ct;
	bool initialized{false};
	~TlsRingBase() noexcept {
		if (initialized) {
			::io_uring_queue_exit(&ring);
		}
	}
};
thread_local TlsRingBase tls_rb_;

} // namespace
expected<ResolveResult, DnsError> Resolver::resolve_blocking(
	SV host,
	u16 port,
	ResolveOptions const &opts) {
	if (current_resolver() == this && impl_->backend == ResolverBackend::native_udp) {
		return unexpected{
			DnsError{
					 DnsErrorKind::cannot_block_on_owned_ring,
					 "resolve_blocking: caller owns this resolver's ring; would deadlock"}
        };
	}
	auto const effective_opts = apply_resolv_defaults(opts, impl_->resolv_query_timeout);

	if (auto ep = try_parse_ip_literal(host, port); ep.has_value()) {
		ResolveResult r;
		r.endpoints.push_back(*ep);
		return r;
	}

	if (!is_valid_hostname(host)) {
		return unexpected{
			DnsError{DnsErrorKind::invalid_hostname, format("invalid hostname '{}'", host)}
        };
	}

	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
		S key{host};
		for (char &c: key) {
			if (c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';
			}
		}
		if (!key.empty() && key.back() == '.') {
			key.pop_back();
		}
		auto it = impl_->hosts_cache.find(key);
		if (it != impl_->hosts_cache.end()) {
			V<Endpoint> eps;
			for (auto const &ep: it->second) {
				if (ep.family == AddressFamily::v4 && effective_opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && effective_opts.allow_v6) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in6 *>(&e.addr)->sin6_port = htons(port);
					eps.push_back(e);
				}
			}
			if (!eps.empty()) {
				ResolveResult r;
				r.endpoints = move(eps);
				r.from_hosts_file = true;
				return r;
			}
		}
	}

	if (impl_->backend == ResolverBackend::nss_thread) {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_ADDRCONFIG;
		addrinfo *res_raw = nullptr;
		S const h{host};
		S const p = to_string(port);
		auto const t0 = chrono::steady_clock::now();
		int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res_raw);
		auto const elapsed = chrono::steady_clock::now() - t0;
		if (gai != 0 || res_raw == nullptr) {
			return unexpected{
				DnsError{DnsErrorKind::nxdomain, format("getaddrinfo: {}", ::gai_strerror(gai))}
            };
		}
		UniqueAddrInfo const res{res_raw};
		ResolveResult result;
		result.elapsed = elapsed;
		for (auto *rp = res.get(); rp != nullptr; rp = rp->ai_next) {
			if (rp->ai_family == AF_INET && effective_opts.allow_v4) {
				Endpoint ep{};
				ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
				ep.family = AddressFamily::v4;
				std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
				result.endpoints.push_back(ep);
			} else if (rp->ai_family == AF_INET6 && effective_opts.allow_v6) {
				Endpoint ep{};
				ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
				ep.family = AddressFamily::v6;
				std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
				result.endpoints.push_back(ep);
			}
		}
		if (result.endpoints.empty()) {
			return unexpected{
				DnsError{DnsErrorKind::nxdomain, format("no usable addresses for '{}'", host)}
            };
		}
		return result;
	}

	// native_udp: spin a temporary ring for this synchronous call
	{
		auto const base_ns =
			effective_opts.override_nameservers.empty() ? impl_->nameservers : effective_opts.override_nameservers;
		auto const ns_list = nameservers_with_attempts(base_ns, impl_->attempts);
		if (ns_list.empty()) {
			return unexpected{
				DnsError{DnsErrorKind::no_servers, "resolve_blocking: no nameservers configured"}
            };
		}

		if (!tls_rb_.initialized) {
			if (::io_uring_queue_init(32, &tls_rb_.ring, 0) < 0) {
				return unexpected{
					DnsError{DnsErrorKind::no_ring, "resolve_blocking: io_uring_queue_init failed"}
                };
			}
			tls_rb_.initialized = true;
		}
		SocketTaskRing tmp_str{SocketRawRing{&tls_rb_.ring}, tls_rb_.ct, [](u32 slot, u32 gen) noexcept -> u64 {
								   return (static_cast<u64>(gen) << 32U) | slot;
							   }};
		codec::Edns0Options const edns{.udp_size = impl_->opts.edns0_udp_size};
		Opt<DnsError> last_nxdomain;
		for (auto const &candidate: resolve_candidates(host, impl_->search_domains, impl_->ndots)) {
			S const cache_key = impl_->cache && !effective_opts.bypass_cache ? make_cache_key(
																						candidate,
																						port,
																						effective_opts.prefer,
																						effective_opts.allow_v4,
																						effective_opts.allow_v6) :
																					S{};
			if (impl_->cache && !effective_opts.bypass_cache) {
				if (auto hit = impl_->cache->get(cache_key); hit.has_value()) {
					if (hit->is_negative) {
						last_nxdomain = DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"};
						continue;
					}
					hit->from_cache = true;
					return move(*hit);
				}
			}
			auto flow = build_native_udp_flow_with_nameservers(
				tmp_str,
				ns_list,
				0,
				candidate,
				port,
				effective_opts.allow_v4,
				effective_opts.allow_v6,
				effective_opts.prefer,
				effective_native_timeout(effective_opts),
				edns);
			try {
				auto budget = effective_native_timeout(effective_opts) + chrono::milliseconds{500};
				auto result = block_on_socket_task(tmp_str, move(flow), budget);
				if (result.endpoints.empty()) {
					return result;
				}
				if (impl_->cache && !cache_key.empty() && !result.endpoints.empty()) {
					auto const max_ttl = impl_->opts.cache_max_ttl;
					auto const ttl = (result.suggested_ttl.count() > 0) ? min(result.suggested_ttl, max_ttl) : max_ttl;
					try {
						impl_->cache->put(cache_key, result, ttl);
					} catch (...) {} // NOLINT(bugprone-empty-catch)
				}
				return result;
			} catch (BlockOnSocketTaskTimeout const &) {
				return unexpected{
					DnsError{DnsErrorKind::timeout, "resolve_blocking: pump timeout"}
                };
			} catch (DnsError const &e) {
				if (e.kind == DnsErrorKind::nxdomain) {
					last_nxdomain = e;
					if (impl_->cache && !cache_key.empty()) {
						ResolveResult neg;
						neg.is_negative = true;
						try {
							impl_->cache->put(cache_key, neg, impl_->opts.cache_negative_ttl);
						} catch (...) {} // NOLINT(bugprone-empty-catch)
					}
					continue;
				}
				return unexpected{e};
			} catch (exception const &e) {
				return unexpected{
					DnsError{DnsErrorKind::network, format("resolve_blocking: {}", e.what())}
                };
			}
		}
		return unexpected{last_nxdomain.value_or(DnsError{DnsErrorKind::nxdomain, "dns: name not found"})};
	}
}
void Resolver::invalidate(
	SV host) {
	if (impl_->cache) {
		impl_->cache->invalidate_by_host(host);
	}
}
void Resolver::clear_cache() {
	if (impl_->cache) {
		impl_->cache->clear();
	}
}
void Resolver::reload() {
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	if (impl_->opts.override_nameservers.empty()) {
		impl_->nameservers = resolv.nameservers;
	}
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	clear_cache();
}
ResolverBackend Resolver::backend() const noexcept {
	return impl_->backend;
}
FileReader *Resolver::file_reader() const noexcept {
	return impl_->reader.get();
}

} // namespace conflux::net::dns
