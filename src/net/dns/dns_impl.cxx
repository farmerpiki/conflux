module;

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.dns;

import std;
import conflux.file_io_sync;
import conflux.utils;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.socket_io.blocking;

namespace conflux::net::dns {
namespace root = conflux::work::root;
using conflux::socket_io::ConnectOptions;
using conflux::socket_io::SocketRawRing;
using conflux::socket_io::SyncWaitSocketTaskTimeout;
using conflux::socket_io::TcpStream;
using conflux::socket_io::UdpSocket;
using conflux::uring::CompletionTable;
using conflux::uring::UserDataFn;
using conflux::utils::ascii_lower;
using conflux::utils::ascii_lower_inplace;
using conflux::utils::LineRange;
using conflux::utils::trim;
using conflux::work::Cancelled;
using conflux::work::join_all;

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
void ignore_best_effort_dns_failure() noexcept {}

} // namespace
[[nodiscard]] Resolver *current_resolver() noexcept {
	return tls_current_resolver;
}

CurrentResolverScope::CurrentResolverScope(
	Resolver *next) noexcept
	: prev_{tls_current_resolver} {
	tls_current_resolver = next;
}

CurrentResolverScope::~CurrentResolverScope() {
	tls_current_resolver = prev_;
}
// ─── file-local helpers ──────────────────────────────────────────────────────

struct ResolvConfig {
	std::vector<NameserverEndpoint> nameservers;
	std::vector<std::string> search_domains;
	size_t ndots{1};
	std::chrono::milliseconds query_timeout{0};
	size_t attempts{1};
};
[[nodiscard]] std::optional<size_t> parse_decimal_size(
	std::string_view sv) noexcept {
	if (sv.empty()) {
		return std::nullopt;
	}
	size_t out = 0;
	for (char const c: sv) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		out = (out * 10U) + static_cast<size_t>(c - '0');
	}
	return out;
}
void parse_resolv_options(
	std::string_view rest,
	ResolvConfig &cfg) {
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		std::string_view const token = end == std::string_view::npos ? rest : rest.substr(0, end);
		if (end == std::string_view::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}

		auto parse_after_colon = [](std::string_view value, std::string_view prefix) -> std::optional<size_t> {
			if (!value.starts_with(prefix)) {
				return std::nullopt;
			}
			return parse_decimal_size(value.substr(prefix.size()));
		};

		if (auto timeout = parse_after_colon(token, "timeout:"); timeout.has_value() && *timeout > 0) {
			cfg.query_timeout = std::chrono::seconds{*timeout};
		} else if (auto attempts = parse_after_colon(token, "attempts:"); attempts.has_value() && *attempts > 0) {
			cfg.attempts = *attempts;
		} else if (auto ndots = parse_after_colon(token, "ndots:"); ndots.has_value()) {
			cfg.ndots = *ndots;
		}
	}
}
[[nodiscard]] std::vector<std::string> parse_search_domains(
	std::string_view rest) {
	std::vector<std::string> out;
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		std::string token{trim(end == std::string_view::npos ? rest : rest.substr(0, end))};
		if (end == std::string_view::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}
		if (token.empty()) {
			continue;
		}
		ascii_lower_inplace(token);
		if (!token.empty() && token.back() == '.') {
			token.pop_back();
		}
		if (!token.empty()) {
			out.push_back(std::move(token));
		}
	}
	return out;
}
[[nodiscard]] ResolvConfig parse_resolv_conf(
	std::filesystem::path const &path) noexcept {
	ResolvConfig out;
	auto const contents =
		conflux::file_io_sync::blocking_read_text_file_nothrow(path.string(), std::size_t{4} * 1024 * 1024);
	if (!contents) {
		return out;
	}
	try {
		for (auto line_view: LineRange{*contents}) {
			std::string line{line_view.text};
			if (auto comment = line.find_first_of("#;"); comment != std::string::npos) {
				line.resize(comment);
			}
			auto trimmed = trim(line);
			if (trimmed.empty()) {
				continue;
			}
			auto const split = trimmed.find_first_of(" \t");
			std::string_view const key{trimmed.data(), split == std::string::npos ? trimmed.size() : split};
			std::string_view rest{};
			if (split != std::string::npos) {
				rest = std::string_view{trimmed.data() + split + 1, trimmed.size() - split - 1};
			}
			if (key == "nameserver") {
				if (auto ns = parse_nameserver(trim(rest)); ns.has_value()) {
					out.nameservers.push_back(*ns);
				}
				continue;
			}
			if (key == "options") {
				parse_resolv_options(rest, out);
				continue;
			}
			if (key == "search") {
				out.search_domains = parse_search_domains(rest);
				continue;
			}
		}
	} catch (...) { ignore_best_effort_dns_failure(); }
	return out;
}
[[nodiscard]] std::unordered_map<std::string, std::vector<Endpoint>> parse_hosts_file(
	std::filesystem::path const &path) noexcept {
	std::unordered_map<std::string, std::vector<Endpoint>> out;
	auto const contents =
		conflux::file_io_sync::blocking_read_text_file_nothrow(path.string(), std::size_t{4} * 1024 * 1024);
	if (!contents) {
		return out;
	}
	try {
		for (auto line_view: LineRange{*contents}) {
			std::string line{line_view.text};
			if (auto hash = line.find('#'); hash != std::string::npos) {
				line.resize(hash);
			}
			size_t pos = 0;
			auto skip_ws = [&] {
				while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
					++pos;
				}
			};
			auto next_token = [&]() -> std::string_view {
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
				continue;
			}

			Endpoint ep{};
			::in_addr v4{};
			::in6_addr v6{};
			std::string const ip_str{ip_sv};
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
				continue;
			}

			while (true) {
				auto name_sv = next_token();
				if (name_sv.empty()) {
					break;
				}
				std::string name{name_sv};
				ascii_lower_inplace(name);
				out[name].push_back(ep);
			}
		}
	} catch (...) { ignore_best_effort_dns_failure(); }
	return out;
}
void set_endpoint_port(
	Endpoint &ep,
	std::uint16_t port) noexcept {
	if (ep.family == AddressFamily::v4) {
		reinterpret_cast<::sockaddr_in *>(&ep.addr)->sin_port = htons(port);
	} else if (ep.family == AddressFamily::v6) {
		reinterpret_cast<::sockaddr_in6 *>(&ep.addr)->sin6_port = htons(port);
	}
}
[[nodiscard]] bool endpoint_family_allowed(
	Endpoint const &ep,
	ResolveOptions const &opts) noexcept {
	return (ep.family == AddressFamily::v4 && opts.allow_v4) || (ep.family == AddressFamily::v6 && opts.allow_v6);
}
[[nodiscard]] std::vector<Endpoint> hosts_endpoints_for_options(
	std::vector<Endpoint> const &cached,
	std::uint16_t port,
	ResolveOptions const &opts) {
	std::vector<Endpoint> eps;
	eps.reserve(cached.size());
	for (auto const &ep: cached) {
		if (!endpoint_family_allowed(ep, opts)) {
			continue;
		}
		auto e = ep;
		set_endpoint_port(e, port);
		eps.push_back(e);
	}
	return eps;
}
[[nodiscard]] std::optional<ResolveResult> hosts_lookup_result(
	std::unordered_map<std::string, std::vector<Endpoint>> const &hosts_cache,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts) {
	std::string key{host};
	ascii_lower_inplace(key);
	if (!key.empty() && key.back() == '.') {
		key.pop_back();
	}
	auto const it = hosts_cache.find(key);
	if (it == hosts_cache.end()) {
		return std::nullopt;
	}
	auto eps = hosts_endpoints_for_options(it->second, port, opts);
	if (eps.empty()) {
		return std::nullopt;
	}
	ResolveResult r;
	r.endpoints = std::move(eps);
	r.from_hosts_file = true;
	return r;
}
[[nodiscard]] std::string lowercase_ascii(
	std::string_view value) {
	return ascii_lower(value);
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
	std::uint16_t expected_id,
	std::string_view expected_qname,
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
			std::format("dns: RCODE {}", static_cast<std::uint8_t>(msg.header.rcode())),
			0,
			std::optional<std::uint8_t>{static_cast<std::uint8_t>(msg.header.rcode())}};
	}
	if (msg.header.tc()) {
		throw DnsError{DnsErrorKind::truncated, "dns: response TC=1"};
	}
}
struct DnsQueryState {
	std::mutex m;
	std::optional<root::TaskControl> active;
	std::atomic<bool> cancel_requested{false};
	void set_active(
		root::TaskControl c) {
		std::optional<root::TaskControl> to_cancel;
		{
			std::scoped_lock const lk{m};
			active.emplace(std::move(c));
			if (cancel_requested.load(std::memory_order_acquire)) {
				to_cancel = active;
			}
		}
		if (to_cancel) {
			auto _ = to_cancel->request_cancel();
		}
	}
	void clear_active() {
		std::scoped_lock const lk{m};
		active.reset();
	}
	void cancel() {
		std::optional<root::TaskControl> to_cancel;
		{
			std::scoped_lock const lk{m};
			cancel_requested.store(true, std::memory_order_release);
			to_cancel = active;
		}
		if (to_cancel) {
			auto _ = to_cancel->request_cancel();
		}
	}
	[[nodiscard]] bool cancelled() const noexcept { return cancel_requested.load(std::memory_order_acquire); }
};
struct ActiveTaskGuard {
	DnsQueryState &state;
	explicit ActiveTaskGuard(
		DnsQueryState &s,
		root::TaskControl c)
		: state{s} {
		state.set_active(std::move(c));
	}
	~ActiveTaskGuard() {
		try {
			state.clear_active();
		} catch (...) {} // NOLINT(bugprone-empty-catch): guard destructor must not propagate.
	}
	ActiveTaskGuard(ActiveTaskGuard const &) = delete;
	ActiveTaskGuard &operator =(ActiveTaskGuard const &) = delete;
};
[[nodiscard]] root::Task<void> run_udp_query_driver(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	std::vector<std::uint8_t> wire,
	std::uint16_t expected_id,
	std::string expected_qname,
	codec::QType expected_qtype,
	std::chrono::milliseconds timeout,
	std::shared_ptr<root::TaskSource<codec::Message>> src,
	std::shared_ptr<DnsQueryState> state) {
	constexpr std::size_t kRxSize = 4096;
	auto check_cancelled = [&] {
		if (state->cancelled()) {
			throw DnsError{DnsErrorKind::cancelled, "dns: query cancelled"};
		}
	};
	try {
		check_cancelled();
		UdpSocket sock = UdpSocket::ephemeral(ring, static_cast<int>(ns.addr.ss_family));
		std::array<std::uint8_t, kRxSize> rx_buf{};
		co_await sock.async_send_to_borrowed(
			std::span<std::uint8_t const>{wire.data(), wire.size()},
			ns.addr,
			ns.addr_len);
		check_cancelled();
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		for (;;) {
			auto const now = std::chrono::steady_clock::now();
			if (now >= deadline) {
				throw DnsError{DnsErrorKind::timeout, "dns: query timed out"};
			}
			auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
			auto recv_task = sock.async_recv_from(std::span<std::uint8_t>{rx_buf.data(), rx_buf.size()}, remaining);
			ActiveTaskGuard const g{*state, recv_task.control()};
			auto const result = co_await std::move(recv_task);
			auto msg = codec::decode_message(std::span<std::uint8_t const>{rx_buf.data(), result.bytes});
			if (!same_dns_peer(result.from, result.from_len, ns)) {
				continue;
			}
			if (!has_expected_question(msg, expected_id, expected_qname, expected_qtype)) {
				continue;
			}
			validate_accepted_response_status(msg);
			check_cancelled();
			auto _ = src->try_set_value(root::Success<codec::Message>{std::move(msg)});
			co_return;
		}
	} catch (root::CancelledError const &) {
		auto _ = src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
	} catch (DnsError const &) { auto _ = src->try_set_exception(std::current_exception()); } catch (IoError const &e) {
		if (e.code().value() == ECANCELED) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
		} else if (e.code().value() == ETIMEDOUT) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: query timed out"}));
		} else {
			auto _ = src->try_set_exception(make_exception_ptr(
				DnsError{DnsErrorKind::network, std::format("dns: udp error: {}", e.what()), e.code().value()}));
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
	std::vector<std::uint8_t> wire,
	std::uint16_t expected_id,
	std::string expected_qname,
	codec::QType expected_qtype,
	std::chrono::milliseconds timeout) {
	auto [out_task, raw_src] = root::make_task_source<codec::Message>(root::SubmitOptions{.enable_cancellation = true});
	auto src = std::make_shared<root::TaskSource<codec::Message>>(std::move(raw_src));
	auto state = std::make_shared<DnsQueryState>();
	auto _ = src->install_cancel_hook([state](root::CancelReason) noexcept {
		try {
			state->cancel();
		} catch (...) {} // NOLINT(bugprone-empty-catch): cancellation callback is best-effort.
	});
	auto driver = run_udp_query_driver(
		ring,
		ns,
		std::move(wire),
		expected_id,
		std::move(expected_qname),
		expected_qtype,
		timeout,
		src,
		state);
	std::move(driver).detach();
	return std::move(out_task);
}
[[nodiscard]] root::Task<void> run_tcp_query_driver(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	std::vector<std::uint8_t> wire,
	std::uint16_t expected_id,
	std::string expected_qname,
	codec::QType expected_qtype,
	std::chrono::milliseconds timeout,
	std::shared_ptr<root::TaskSource<codec::Message>> src,
	std::shared_ptr<DnsQueryState> state) {
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
		std::vector<std::uint8_t> framed;
		framed.reserve(2 + wire.size());
		auto const wlen = static_cast<std::uint16_t>(wire.size());
		framed.push_back(static_cast<std::uint8_t>(wlen >> 8U));
		framed.push_back(static_cast<std::uint8_t>(wlen & 0xFFU));
		framed.insert(framed.end(), wire.begin(), wire.end());
		int const family = static_cast<int>(ns.addr.ss_family);
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		auto remaining_or_throw = [&]() -> std::chrono::milliseconds {
			auto const now = std::chrono::steady_clock::now();
			if (now >= deadline) {
				throw DnsError{DnsErrorKind::timeout, "dns: tcp query timed out"};
			}
			return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
		};
		ConnectOptions copts{};
		copts.timeout = timeout;
		TcpStream stream{};
		{
			auto connect_task = async_tcp_connect(ring, family, ns.addr, ns.addr_len, copts);
			ActiveTaskGuard const g{*state, connect_task.control()};
			stream = co_await std::move(connect_task);
		}
		check_cancelled();
		{
			std::size_t sent = 0;
			while (sent < framed.size()) {
				auto write_task = stream.async_write_borrowed(
					std::span<std::uint8_t const>{framed.data() + sent, framed.size() - sent});
				ActiveTaskGuard const g{*state, write_task.control()};
				std::size_t const n = co_await std::move(write_task);
				if (n == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp write failed"};
				}
				sent += n;
			}
		}
		check_cancelled();
		std::array<std::uint8_t, 2> len_buf{};
		{
			std::size_t n = 0;
			while (n < 2) {
				root::Task<std::size_t> recv_task = stream.async_recv_borrowed(
					std::span<std::uint8_t>{len_buf.data() + n, 2 - n},
					remaining_or_throw());
				ActiveTaskGuard const g{*state, recv_task.control()};
				std::size_t const got = co_await std::move(recv_task);
				if (got == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp short length prefix"};
				}
				n += got;
			}
		}
		std::uint16_t const resp_len = static_cast<std::uint16_t>(
			(static_cast<std::uint16_t>(len_buf[0]) << 8U) | static_cast<std::uint16_t>(len_buf[1]));
		if (resp_len == 0) {
			throw DnsError{DnsErrorKind::malformed, "dns: tcp zero-length response"};
		}
		std::vector<std::uint8_t> resp_buf(resp_len);
		{
			std::size_t resp_n = 0;
			while (resp_n < static_cast<std::size_t>(resp_len)) {
				root::Task<std::size_t> recv_task = stream.async_recv_borrowed(
					std::span<std::uint8_t>{resp_buf.data() + resp_n, static_cast<std::size_t>(resp_len) - resp_n},
					remaining_or_throw());
				ActiveTaskGuard const g{*state, recv_task.control()};
				std::size_t const got = co_await std::move(recv_task);
				if (got == 0) {
					throw DnsError{DnsErrorKind::network, "dns: tcp short response"};
				}
				resp_n += got;
			}
		}
		auto msg =
			codec::decode_message(std::span<std::uint8_t const>{resp_buf.data(), static_cast<std::size_t>(resp_len)});
		if (!has_expected_question(msg, expected_id, expected_qname, expected_qtype)) {
			throw DnsError{DnsErrorKind::malformed, "dns: tcp response mismatch"};
		}
		validate_accepted_response_status(msg);
		check_cancelled();
		auto _ = src->try_set_value(root::Success<codec::Message>{std::move(msg)});
	} catch (root::CancelledError const &) {
		auto _ =
			src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: tcp query cancelled"}));
	} catch (DnsError const &) { auto _ = src->try_set_exception(std::current_exception()); } catch (IoError const &e) {
		if (e.code().value() == ECANCELED) {
			auto _ = src->try_set_exception(
				make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: tcp query cancelled"}));
		} else if (e.code().value() == ETIMEDOUT) {
			auto _ =
				src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: tcp query timed out"}));
		} else {
			auto _ = src->try_set_exception(make_exception_ptr(
				DnsError{DnsErrorKind::network, std::format("dns: tcp error: {}", e.what()), e.code().value()}));
		}
	} catch (...) {
		auto _ = src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::network, "dns: tcp query failed"}));
	}
}
// TCP DNS query per RFC 1035 §4.2.2: plain factory; driver owns all buffers.
[[nodiscard]] root::Task<codec::Message> tcp_single_query(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	std::vector<std::uint8_t> wire,
	std::uint16_t expected_id,
	std::string expected_qname,
	codec::QType expected_qtype,
	std::chrono::milliseconds timeout) {
	auto [out_task, raw_src] = root::make_task_source<codec::Message>(root::SubmitOptions{.enable_cancellation = true});
	auto src = std::make_shared<root::TaskSource<codec::Message>>(std::move(raw_src));
	auto state = std::make_shared<DnsQueryState>();
	auto _ = src->install_cancel_hook([state](root::CancelReason) noexcept {
		try {
			state->cancel();
		} catch (...) {} // NOLINT(bugprone-empty-catch): cancellation callback is best-effort.
	});
	auto driver = run_tcp_query_driver(
		ring,
		ns,
		std::move(wire),
		expected_id,
		std::move(expected_qname),
		expected_qtype,
		timeout,
		src,
		state);
	std::move(driver).detach();
	return std::move(out_task);
}
// Minimum TTL across all answer RRs of the given family (UINT32_MAX if none).
[[nodiscard]] std::uint32_t min_answer_ttl(
	codec::Message const &msg,
	AddressFamily family) noexcept {
	std::uint32_t min_ttl = std::numeric_limits<std::uint32_t>::max();
	for (auto const &rr: msg.answers) {
		auto const is_match = (family == AddressFamily::v4 && rr.type == codec::QType::a)
						   || (family == AddressFamily::v6 && rr.type == codec::QType::aaaa);
		if (is_match) {
			min_ttl = std::min(min_ttl, rr.ttl);
		}
	}
	return min_ttl;
}

// Ordered by severity so std::max(a,b) gives the dominant failure reason.
enum class BatchFailReason : std::uint8_t {
	none = 0,
	timeout = 1,
	network = 2,
	nxdomain = 3,
	truncated = 4,
};
struct EndpointBatch {
	std::vector<Endpoint> eps;
	std::uint32_t min_ttl{std::numeric_limits<std::uint32_t>::max()};
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
	std::string_view hostname,
	std::uint16_t port,
	std::uint16_t qid,
	codec::QType qtype,
	AddressFamily fam,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	auto wire = codec::encode_query(qid, hostname, qtype, edns);
	auto wire_ptr = std::make_shared<std::vector<std::uint8_t>>(wire); // copy for TCP fallback
	auto expected_qname = lowercase_ascii(hostname);
	bool needs_tcp_fallback = false;
	try {
		auto udp_task = udp_single_query(ring, ns, std::move(wire), qid, expected_qname, qtype, timeout);
		auto const msg = co_await std::move(udp_task);
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
		auto const msg2 = co_await std::move(tcp_task);
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
[[nodiscard]] std::uint16_t random_dns_query_id() {
	std::uint16_t id{};
	std::array<unsigned char, sizeof(id)> bytes{};
	conflux::utils::crypto_random_bytes(bytes);
	std::memcpy(&id, bytes.data(), sizeof(id));
	return id;
}
// Fire A and AAAA queries in parallel (RFC 8305 §3). Connection-attempt
// staggering belongs in the caller's connect loop, not here.
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow(
	SocketTaskRing &ring,
	NameserverEndpoint ns,
	std::string const &hostname,
	std::uint16_t port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	std::uint16_t const qid_a = random_dns_query_id();
	std::uint16_t const qid_aaaa = random_dns_query_id();
	auto v4_task =
		do_v4 ? build_family_flow(ring, ns, hostname, port, qid_a, codec::QType::a, AddressFamily::v4, timeout, edns) :
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
	auto batches = co_await join_all(std::move(v4_task), std::move(v6_task));
	auto v4 = std::move(std::get<0>(batches));
	auto v6 = std::move(std::get<1>(batches));
	if (v4.eps.empty() && v6.eps.empty()) {
		// Both families have no results. Propagate the dominant failure.
		auto const w = (static_cast<std::uint8_t>(v4.fail_reason) >= static_cast<std::uint8_t>(v6.fail_reason)) ?
						   v4.fail_reason :
						   v6.fail_reason;
		if (w == BatchFailReason::truncated) {
			throw DnsError{DnsErrorKind::truncated, "dns: udp truncated and tcp fallback failed"};
		}
		if (w == BatchFailReason::nxdomain) {
			throw DnsError{DnsErrorKind::nxdomain, "dns: name not found"};
		}
	}
	std::vector<Endpoint> all;
	all.reserve(v6.eps.size() + v4.eps.size());
	auto append_all = [&all](std::vector<Endpoint> const &eps) { all.insert(all.end(), eps.begin(), eps.end()); };
	if (prefer == AddressFamily::v4) {
		append_all(v4.eps);
		append_all(v6.eps);
	} else {
		append_all(v6.eps);
		append_all(v4.eps);
	}
	std::uint32_t const min_ttl = std::min(v4.min_ttl, v6.min_ttl);
	ResolveResult r;
	r.endpoints = std::move(all);
	if (min_ttl != std::numeric_limits<std::uint32_t>::max()) {
		r.suggested_ttl = std::chrono::seconds{min_ttl};
	}
	co_return r;
}
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow_with_nameservers(
	SocketTaskRing &ring,
	std::vector<NameserverEndpoint> nameservers,
	size_t index,
	std::string hostname,
	std::uint16_t port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	if (index >= nameservers.size()) {
		throw DnsError{DnsErrorKind::no_servers, "dns: no nameservers configured"};
	}
	auto const ns = nameservers[index];
	auto query_host = hostname;
	auto udp_task = build_native_udp_flow(ring, ns, query_host, port, do_v4, do_v6, prefer, timeout, edns);
	auto result = co_await std::move(udp_task);
	if (!result.endpoints.empty() || index + 1 >= nameservers.size()) {
		co_return std::move(result);
	}
	auto next_task = build_native_udp_flow_with_nameservers(
		ring,
		std::move(nameservers),
		index + 1,
		std::move(hostname),
		port,
		do_v4,
		do_v6,
		prefer,
		timeout,
		edns);
	co_return co_await std::move(next_task);
}
[[nodiscard]] root::Task<ResolveResult> build_native_udp_flow_with_candidates(
	SocketTaskRing &ring,
	std::vector<NameserverEndpoint> nameservers,
	std::vector<std::string> candidates,
	size_t index,
	std::uint16_t port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
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
			std::move(query_nameservers),
			0,
			candidate,
			port,
			do_v4,
			do_v6,
			prefer,
			timeout,
			edns);
		co_return co_await std::move(ns_task);
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
		std::move(nameservers),
		std::move(candidates),
		index + 1,
		port,
		do_v4,
		do_v6,
		prefer,
		timeout,
		edns);
	co_return co_await std::move(next_task);
}
// ─── LRU TTL cache ──────────────────────────────────────────────────────────

struct DnsCacheEntry {
	ResolveResult result;
	std::chrono::steady_clock::time_point expires;
};
class LruDnsCache {
	conflux::support::StringLruMap<DnsCacheEntry> entries_;
	mutable std::mutex mtx_;

public:
	explicit LruDnsCache(
		size_t cap)
		: entries_{std::max<size_t>(cap, 1)} {}
	[[nodiscard]] std::optional<ResolveResult> get(
		std::string_view key) {
		std::scoped_lock const lk{mtx_};
		auto *entry = entries_.find(key);
		if (entry == nullptr) {
			return std::nullopt;
		}
		if (std::chrono::steady_clock::now() >= entry->expires) {
			(void)entries_.erase(key);
			return std::nullopt;
		}
		return entry->result;
	}
	void put(
		std::string_view key,
		ResolveResult result,
		std::chrono::seconds ttl) {
		auto const expires = std::chrono::steady_clock::now() + ttl;
		std::scoped_lock const lk{mtx_};
		(void)entries_.insert_or_assign(key, DnsCacheEntry{std::move(result), expires});
	}
	void invalidate_by_host(
		std::string_view host) {
		std::string prefix;
		prefix.reserve(host.size() + 1);
		prefix.append(host);
		prefix.push_back(':');
		std::scoped_lock const lk{mtx_};
		(void)entries_.erase_if([&](std::string_view key, DnsCacheEntry const &) { return key.starts_with(prefix); });
	}
	void clear() {
		std::scoped_lock const lk{mtx_};
		entries_.clear();
	}
};
[[nodiscard]] std::string make_cache_key(
	std::string_view host,
	std::uint16_t port,
	AddressFamily prefer,
	bool v4,
	bool v6) {
	std::array<char, std::numeric_limits<std::uint16_t>::digits10 + 1> port_buf{};
	auto const [port_end, ec] = std::to_chars(port_buf.data(), port_buf.data() + port_buf.size(), port);
	if (ec != std::errc{}) {
		return {};
	}
	std::string out;
	out.reserve(host.size() + static_cast<std::size_t>(port_end - port_buf.data()) + 5);
	out.append(host);
	out.push_back(':');
	out.append(port_buf.data(), port_end);
	out.push_back(':');
	out.push_back(prefer == AddressFamily::v4 ? '4' : '6');
	out.push_back(v4 ? '4' : '-');
	out.push_back(v6 ? '6' : '-');
	return out;
}
[[nodiscard]] std::chrono::milliseconds effective_native_timeout(
	ResolveOptions const &opts) noexcept {
	if (opts.query_timeout.count() <= 0) {
		return opts.total_timeout;
	}
	if (opts.total_timeout.count() <= 0) {
		return opts.query_timeout;
	}
	return std::min(opts.query_timeout, opts.total_timeout);
}
[[nodiscard]] ResolveOptions apply_resolv_defaults(
	ResolveOptions opts,
	std::chrono::milliseconds resolv_query_timeout) noexcept {
	ResolveOptions const defaults;
	if (opts.query_timeout == defaults.query_timeout && resolv_query_timeout.count() > 0) {
		opts.query_timeout = resolv_query_timeout;
	}
	return opts;
}
[[nodiscard]] std::vector<NameserverEndpoint> nameservers_with_attempts(
	std::vector<NameserverEndpoint> const &base,
	size_t attempts) {
	std::vector<NameserverEndpoint> out;
	if (base.empty()) {
		return out;
	}
	attempts = std::max<size_t>(attempts, 1);
	out.reserve(base.size() * attempts);
	for (size_t attempt = 0; attempt < attempts; ++attempt) {
		out.insert(out.end(), base.begin(), base.end());
	}
	return out;
}
[[nodiscard]] std::vector<std::string> resolve_candidates(
	std::string_view host,
	std::vector<std::string> const &search_domains,
	size_t ndots) {
	std::string normalized{host};
	if (!normalized.empty() && normalized.back() == '.') {
		normalized.pop_back();
		return {std::move(normalized)};
	}
	auto const dot_count = static_cast<size_t>(std::ranges::count(normalized, '.'));
	if (search_domains.empty() || dot_count >= ndots) {
		return {std::move(normalized)};
	}
	std::vector<std::string> out;
	out.reserve(search_domains.size() + 1);
	for (auto const &domain: search_domains) {
		std::string candidate;
		candidate.reserve(normalized.size() + 1 + domain.size());
		candidate.append(normalized);
		candidate.push_back('.');
		candidate.append(domain);
		out.push_back(std::move(candidate));
	}
	out.push_back(std::move(normalized));
	return out;
}
// ─── Resolver::Impl ─────────────────────────────────────────────────────────

struct CoalescedBroadcast {
	std::vector<std::shared_ptr<root::TaskSource<ResolveResult>>> waiters;
};
struct InFlightKey {
	std::string cache_key;
	SocketTaskRing *ring{};
};
struct InFlightKeyHash {
	size_t operator ()(
		InFlightKey const &k) const noexcept {
		size_t const h1 = std::hash<std::string>{}(k.cache_key);
		size_t const h2 = std::hash<void const *>{}(k.ring);
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
	std::unique_ptr<conflux::file_io::FileReader> reader{};
	std::unique_ptr<SocketTaskRing> task_ring{};
	WorkPool *pool{nullptr};
	ResolverOptions opts;
	std::vector<NameserverEndpoint> nameservers;
	std::vector<std::string> search_domains;
	size_t ndots{1};
	std::chrono::milliseconds resolv_query_timeout{0};
	size_t attempts{1};
	std::unordered_map<std::string, std::vector<Endpoint>> hosts_cache;
	std::shared_ptr<LruDnsCache> cache{};
	std::unordered_map<InFlightKey, CoalescedBroadcast, InFlightKeyHash, InFlightKeyEq> in_flight;
	std::mutex in_flight_mutex;
};

[[nodiscard]] root::Task<ResolveResult> ready_resolve_success(
	ResolveResult result) {
	auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
	auto _ = raw_src.try_set_value(root::Success<ResolveResult>{std::move(result)});
	return std::move(task);
}

[[nodiscard]] root::Task<ResolveResult> ready_resolve_error(
	DnsError error) {
	auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
	auto _ = raw_src.try_set_exception(make_exception_ptr(std::move(error)));
	return std::move(task);
}

[[nodiscard]] std::vector<NameserverEndpoint> native_udp_nameservers(
	auto const &impl,
	ResolveOptions const &opts) {
	auto const base_ns = opts.override_nameservers.empty() ? impl->nameservers : opts.override_nameservers;
	return nameservers_with_attempts(base_ns, impl->attempts);
}

[[nodiscard]] root::Task<ResolveResult> make_coalesced_waiter_task(
	root::Task<ResolveResult> waiter_task) {
	auto [out_task, out_raw_src] =
		root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = true});
	auto out_src = std::make_shared<root::TaskSource<ResolveResult>>(std::move(out_raw_src));
	auto weak_out = std::weak_ptr<root::TaskSource<ResolveResult>>{out_src};
	auto _ = out_src->install_cancel_hook([weak_out](root::CancelReason) noexcept {
		try {
			if (auto src = weak_out.lock()) {
				auto _ = src->try_set_exception(
					make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: coalesced query cancelled"}));
			}
		} catch (...) {} // NOLINT(bugprone-empty-catch): cancellation callback is best-effort.
	});
	auto bridge_waiter = [](std::shared_ptr<root::TaskSource<ResolveResult>> out_src,
							root::Task<ResolveResult> waiter) -> root::Task<void> {
		try {
			auto r = co_await std::move(waiter);
			r.from_coalesced = true;
			auto _ = out_src->try_set_value(root::Success<ResolveResult>{std::move(r)});
		} catch (Cancelled const &) {
			auto _ = out_src->try_set_exception(
				make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
		} catch (...) { auto _ = out_src->try_set_exception(std::current_exception()); }
	};
	bridge_waiter(out_src, std::move(waiter_task)).detach();
	return std::move(out_task);
}

struct NativeUdpCoalesceResult {
	std::optional<root::Task<ResolveResult>> task;
	std::optional<DnsError> error;
};

[[nodiscard]] NativeUdpCoalesceResult try_join_or_register_native_dns_query(
	auto const &impl,
	InFlightKey const &inflight_key) {
	std::lock_guard const lock{impl->in_flight_mutex};
	if (impl->in_flight.size() >= impl->opts.max_in_flight_queries) {
		return {
			.error = DnsError{DnsErrorKind::cancelled, "resolve: max in-flight queries exceeded"}
        };
	}
	if (inflight_key.cache_key.empty()) {
		return {};
	}
	if (auto it = impl->in_flight.find(inflight_key); it != impl->in_flight.end()) {
		auto [waiter_task, waiter_raw_src] =
			root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_waiter = std::make_shared<root::TaskSource<ResolveResult>>(std::move(waiter_raw_src));
		it->second.waiters.push_back(shared_waiter);
		return {.task = make_coalesced_waiter_task(std::move(waiter_task))};
	}
	impl->in_flight.emplace(inflight_key, CoalescedBroadcast{});
	return {};
}

[[nodiscard]] std::vector<std::shared_ptr<root::TaskSource<ResolveResult>>> take_native_dns_waiters(
	auto const &impl,
	InFlightKey const &inflight_key) {
	std::vector<std::shared_ptr<root::TaskSource<ResolveResult>>> waiters;
	if (inflight_key.cache_key.empty()) {
		return waiters;
	}
	std::lock_guard const lock{impl->in_flight_mutex};
	if (auto it = impl->in_flight.find(inflight_key); it != impl->in_flight.end()) {
		waiters = std::move(it->second.waiters);
		impl->in_flight.erase(it);
	}
	return waiters;
}

[[nodiscard]] ResolveResult cache_native_dns_result(
	auto const &impl,
	std::string const &cache_key,
	ResolveResult result) {
	try {
		if (impl->cache && !cache_key.empty() && !result.endpoints.empty()) {
			auto const ttl = (result.suggested_ttl.count() > 0) ?
								 std::min(result.suggested_ttl, impl->opts.cache_max_ttl) :
								 impl->opts.cache_max_ttl;
			impl->cache->put(cache_key, result, ttl);
		}
	} catch (...) { ignore_best_effort_dns_failure(); }
	return result;
}

[[nodiscard]] ResolveResult fanout_native_dns_success(
	auto const &impl,
	InFlightKey const &inflight_key,
	ResolveResult result) {
	for (auto const &waiter: take_native_dns_waiters(impl, inflight_key)) {
		auto copy = result;
		copy.from_coalesced = true;
		auto _ = waiter->try_set_value(root::Success<ResolveResult>{std::move(copy)});
	}
	return result;
}

void maybe_cache_native_dns_negative_result(
	auto const &impl,
	InFlightKey const &inflight_key,
	DnsError const &error) {
	if (error.kind == DnsErrorKind::nxdomain && impl->cache && !inflight_key.cache_key.empty()) {
		ResolveResult neg;
		neg.is_negative = true;
		try {
			impl->cache->put(inflight_key.cache_key, neg, impl->opts.cache_negative_ttl);
		} catch (...) { ignore_best_effort_dns_failure(); }
	}
}

void fanout_native_dns_error(
	auto const &impl,
	InFlightKey const &inflight_key,
	std::exception_ptr const &error) {
	for (auto const &waiter: take_native_dns_waiters(impl, inflight_key)) {
		auto _ = waiter->try_set_exception(error);
	}
	try {
		std::rethrow_exception(error);
	} catch (DnsError const &dns_error) {
		maybe_cache_native_dns_negative_result(impl, inflight_key, dns_error);
		throw;
	}
}

void complete_cancelled_native_dns_query(
	std::shared_ptr<root::TaskSource<ResolveResult>> const &out_src,
	auto const &impl,
	InFlightKey const &inflight_key) {
	auto cancelled = make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"});
	for (auto const &waiter: take_native_dns_waiters(impl, inflight_key)) {
		auto _ = waiter->try_set_exception(cancelled);
	}
	auto _ = out_src->try_set_exception(std::move(cancelled));
}

void bridge_native_udp_result(
	std::shared_ptr<root::TaskSource<ResolveResult>> out_src,
	root::Task<ResolveResult> inner,
	auto impl,
	InFlightKey inflight_key) {
	auto bridge = [](std::shared_ptr<root::TaskSource<ResolveResult>> out_src,
					 root::Task<ResolveResult> inner,
					 auto impl,
					 InFlightKey inflight_key) mutable -> root::Task<void> {
		try {
			auto result = co_await std::move(inner);
			result = cache_native_dns_result(impl, inflight_key.cache_key, std::move(result));
			result = fanout_native_dns_success(impl, inflight_key, std::move(result));
			auto _ = out_src->try_set_value(root::Success<ResolveResult>{std::move(result)});
		} catch (Cancelled const &) { complete_cancelled_native_dns_query(out_src, impl, inflight_key); } catch (...) {
			try {
				fanout_native_dns_error(impl, inflight_key, std::current_exception());
			} catch (...) { auto _ = out_src->try_set_exception(std::current_exception()); }
		}
	};
	bridge(std::move(out_src), std::move(inner), std::move(impl), std::move(inflight_key)).detach();
}

Resolver::Resolver(
	::io_uring *ring,
	CompletionTable *completions,
	UserDataFn encode_ud,
	ResolverOptions opts)
	: impl_{std::make_shared<Impl>()} {
	impl_->backend = ResolverBackend::native_udp;
	auto shared_ud = std::make_shared<UserDataFn>(std::move(encode_ud));
	impl_->reader = std::make_unique<conflux::file_io::FileReader>(
		ring,
		completions,
		[shared_ud](std::uint32_t s, std::uint32_t g) -> std::uint64_t { return (*shared_ud)(s, g); });
	impl_->task_ring = std::make_unique<SocketTaskRing>(
		SocketRawRing{ring},
		*completions,
		[shared_ud](std::uint32_t s, std::uint32_t g) -> std::uint64_t { return (*shared_ud)(s, g); });
	impl_->opts = std::move(opts);
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
		impl_->cache = std::make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}
Resolver::Resolver(
	WorkPool &pool,
	ResolverOptions opts)
	: impl_{std::make_shared<Impl>()} {
	impl_->backend = ResolverBackend::nss_thread;
	impl_->pool = &pool;
	impl_->opts = std::move(opts);
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
		impl_->cache = std::make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}
Resolver::~Resolver() = default;
root::Task<ResolveResult> Resolver::resolve_nss_thread(
	SocketTaskRing *external_ring,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts,
	std::string const &cache_key) {
	if (external_ring != nullptr) {
		return ready_resolve_error(
			DnsError{
				DnsErrorKind::not_implemented,
				"resolve: nss_thread resolver does not support caller-provided ring"});
	}

	auto [task, raw_src] = root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<ResolveResult>>(std::move(raw_src));
	bool const ok = impl_->pool->enqueue([shared_src, // NOLINT(bugprone-exception-escape)
										  h = std::string{host},
										  port,
										  allow_v4 = opts.allow_v4,
										  allow_v6 = opts.allow_v6,
										  cache = impl_->cache,
										  cache_key = cache_key,
										  ttl = impl_->opts.cache_max_ttl]() mutable {
		try {
			addrinfo hints{};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_flags = AI_ADDRCONFIG;
			addrinfo *res_raw = nullptr;
			std::string const p = std::to_string(port);
			int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res_raw);
			if (gai != 0 || res_raw == nullptr) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(
					DnsError{DnsErrorKind::nxdomain, std::format("getaddrinfo: {}", ::gai_strerror(gai))}));
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
					DnsError{DnsErrorKind::nxdomain, std::format("no usable addresses for '{}'", h)}));
				return;
			}
			if (cache && !cache_key.empty() && !result.endpoints.empty()) {
				cache->put(cache_key, result, ttl);
			}
			auto _ = shared_src->try_set_value(root::Success<ResolveResult>{std::move(result)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	if (!ok) {
		auto _ = shared_src->try_set_exception(
			make_exception_ptr(DnsError{DnsErrorKind::cancelled, "nss_thread: work pool not accepting jobs"}));
	}
	return std::move(task);
}

root::Task<ResolveResult> Resolver::resolve_native_udp(
	SocketTaskRing *external_ring,
	SocketTaskRing *task_ring,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts,
	std::string const &cache_key) {
	InFlightKey const inflight_key{cache_key, external_ring};

	auto const ns_list = native_udp_nameservers(impl_, opts);

	if (ns_list.empty()) {
		return ready_resolve_error(DnsError{DnsErrorKind::no_servers, "resolve: no nameservers configured"});
	}

	if (task_ring == nullptr) {
		return ready_resolve_error(DnsError{DnsErrorKind::no_ring, "resolve: no ring available"});
	}

	auto coalesced = try_join_or_register_native_dns_query(impl_, inflight_key);
	if (coalesced.error) {
		return ready_resolve_error(std::move(*coalesced.error));
	}
	if (coalesced.task) {
		return std::move(*coalesced.task);
	}
	auto const timeout = effective_native_timeout(opts);
	codec::Edns0Options const edns{.udp_size = impl_->opts.edns0_udp_size};
	bool const do_v4 = opts.allow_v4;
	bool const do_v6 = opts.allow_v6;
	auto const candidates = resolve_candidates(host, impl_->search_domains, impl_->ndots);

	auto inner_task = build_native_udp_flow_with_candidates(
		*task_ring,
		ns_list,
		candidates,
		0,
		port,
		do_v4,
		do_v6,
		opts.prefer,
		timeout,
		edns);
	auto inner_control = inner_task.control();
	auto [out_task, out_raw_src] =
		root::make_task_source<ResolveResult>(root::SubmitOptions{.enable_cancellation = true});
	auto out_src = std::make_shared<root::TaskSource<ResolveResult>>(std::move(out_raw_src));
	auto weak_out = std::weak_ptr<root::TaskSource<ResolveResult>>{out_src};
	auto _ = out_src->install_cancel_hook([inner_control, weak_out](root::CancelReason) mutable noexcept {
		(void)inner_control.request_cancel();
		if (auto src = weak_out.lock()) {
			(void)src->try_set_exception(make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"}));
		}
	});
	bridge_native_udp_result(out_src, std::move(inner_task), impl_, inflight_key);
	return std::move(out_task);
}

root::Task<ResolveResult> Resolver::resolve_flow(
	SocketTaskRing *external_ring,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &per_opts) {
	auto const effective_opts = apply_resolv_defaults(per_opts, impl_->resolv_query_timeout);
	if (auto ep = try_parse_ip_literal(host, port); ep.has_value()) {
		ResolveResult r;
		r.endpoints.push_back(*ep);
		return ready_resolve_success(std::move(r));
	}

	if (!is_valid_hostname(host)) {
		return ready_resolve_error(
			DnsError{DnsErrorKind::invalid_hostname, std::format("invalid hostname '{}'", host)});
	}

	// /etc/hosts lookup
	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
		if (auto hosts_result = hosts_lookup_result(impl_->hosts_cache, host, port, effective_opts); hosts_result) {
			return ready_resolve_success(std::move(*hosts_result));
		}
	}

	std::string const cache_key =
		effective_opts.bypass_cache ?
			std::string{} :
			make_cache_key(host, port, effective_opts.prefer, effective_opts.allow_v4, effective_opts.allow_v6);

	// LRU cache lookup
	if (impl_->cache && !cache_key.empty()) {
		if (auto hit = impl_->cache->get(cache_key); hit.has_value()) {
			if (hit->is_negative) {
				return ready_resolve_error(DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"});
			}
			hit->from_cache = true;
			return ready_resolve_success(std::move(*hit));
		}
	}

	if (impl_->backend == ResolverBackend::nss_thread) {
		return resolve_nss_thread(external_ring, host, port, effective_opts, cache_key);
	}

	SocketTaskRing *task_ring = (external_ring != nullptr) ? external_ring : impl_->task_ring.get();
	return resolve_native_udp(external_ring, task_ring, host, port, effective_opts, cache_key);
}
conflux::work::root::Task<ResolveResult> Resolver::resolve(
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts) {
	return resolve_flow(nullptr, host, port, opts);
}
conflux::work::root::Task<ResolveResult> Resolver::resolve(
	SocketTaskRing &ring,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts) {
	return resolve_flow(&ring, host, port, opts);
}
namespace {

struct BlockingDnsRingBase {
	::io_uring ring{};
	CompletionTable ct;
	bool initialized{false};
	~BlockingDnsRingBase() noexcept {
		if (initialized) {
			::io_uring_queue_exit(&ring);
		}
	}
};

[[nodiscard]] std::expected<ResolveResult, DnsError> resolve_blocking_nss_result(
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &effective_opts) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;
	addrinfo *res_raw = nullptr;
	std::string const h{host};
	std::string const p = std::to_string(port);
	auto const t0 = std::chrono::steady_clock::now();
	int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res_raw);
	auto const elapsed = std::chrono::steady_clock::now() - t0;
	if (gai != 0 || res_raw == nullptr) {
		return std::unexpected{
			DnsError{DnsErrorKind::nxdomain, std::format("getaddrinfo: {}", ::gai_strerror(gai))}
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
		return std::unexpected{
			DnsError{DnsErrorKind::nxdomain, std::format("no usable addresses for '{}'", host)}
        };
	}
	return result;
}

[[nodiscard]] std::optional<DnsError> init_blocking_dns_ring(
	BlockingDnsRingBase &ring_base) {
	if (::io_uring_queue_init(32, &ring_base.ring, 0) < 0) {
		return DnsError{DnsErrorKind::no_ring, "resolve_blocking: io_uring_queue_init failed"};
	}
	ring_base.initialized = true;
	return std::nullopt;
}

[[nodiscard]] std::string blocking_dns_cache_key(
	auto const &impl,
	std::string_view candidate,
	std::uint16_t port,
	ResolveOptions const &effective_opts) {
	if (!impl->cache || effective_opts.bypass_cache) {
		return {};
	}
	return make_cache_key(candidate, port, effective_opts.prefer, effective_opts.allow_v4, effective_opts.allow_v6);
}

[[nodiscard]] std::optional<std::expected<ResolveResult, DnsError>> lookup_blocking_dns_cache(
	auto const &impl,
	std::string const &cache_key) {
	if (!impl->cache || cache_key.empty()) {
		return std::nullopt;
	}
	if (auto hit = impl->cache->get(cache_key); hit.has_value()) {
		if (hit->is_negative) {
			return std::unexpected{
				DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"}
            };
		}
		hit->from_cache = true;
		return std::move(*hit);
	}
	return std::nullopt;
}

void cache_blocking_dns_negative_result(
	auto const &impl,
	std::string const &cache_key) {
	if (!impl->cache || cache_key.empty()) {
		return;
	}
	ResolveResult neg;
	neg.is_negative = true;
	try {
		impl->cache->put(cache_key, neg, impl->opts.cache_negative_ttl);
	} catch (...) { ignore_best_effort_dns_failure(); }
}

[[nodiscard]] std::expected<ResolveResult, DnsError> resolve_blocking_native_candidate(
	auto const &impl,
	SocketTaskRing &tmp_str,
	std::vector<NameserverEndpoint> const &ns_list,
	std::string const &candidate,
	std::uint16_t port,
	ResolveOptions const &effective_opts,
	codec::Edns0Options const &edns,
	std::string const &cache_key) {
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
		auto budget = effective_native_timeout(effective_opts) + std::chrono::milliseconds{500};
		auto result = sync_wait_socket_task(tmp_str, std::move(flow), budget);
		if (result.endpoints.empty()) {
			return result;
		}
		return cache_native_dns_result(impl, cache_key, std::move(result));
	} catch (SyncWaitSocketTaskTimeout const &) {
		return std::unexpected{
			DnsError{DnsErrorKind::timeout, "resolve_blocking: pump timeout"}
        };
	} catch (DnsError const &e) {
		if (e.kind == DnsErrorKind::nxdomain) {
			cache_blocking_dns_negative_result(impl, cache_key);
		}
		return std::unexpected{e};
	} catch (std::exception const &e) {
		return std::unexpected{
			DnsError{DnsErrorKind::network, std::format("resolve_blocking: {}", e.what())}
        };
	}
}

[[nodiscard]] std::expected<ResolveResult, DnsError> resolve_blocking_native_udp_result(
	auto const &impl,
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &effective_opts) {
	auto const ns_list = native_udp_nameservers(impl, effective_opts);
	if (ns_list.empty()) {
		return std::unexpected{
			DnsError{DnsErrorKind::no_servers, "resolve_blocking: no nameservers configured"}
        };
	}
	BlockingDnsRingBase ring_base;
	if (auto err = init_blocking_dns_ring(ring_base); err) {
		return std::unexpected{*err};
	}
	SocketTaskRing tmp_str{
		SocketRawRing{&ring_base.ring},
		ring_base.ct,
		[](std::uint32_t slot, std::uint32_t gen) noexcept -> std::uint64_t {
			return (static_cast<std::uint64_t>(gen) << 32U) | slot;
		}};
	codec::Edns0Options const edns{.udp_size = impl->opts.edns0_udp_size};
	std::optional<DnsError> last_nxdomain;
	for (auto const &candidate: resolve_candidates(host, impl->search_domains, impl->ndots)) {
		std::string const cache_key = blocking_dns_cache_key(impl, candidate, port, effective_opts);
		if (auto cached = lookup_blocking_dns_cache(impl, cache_key); cached) {
			if (*cached) {
				return std::move(**cached);
			}
			last_nxdomain = cached->error();
			continue;
		}
		auto result =
			resolve_blocking_native_candidate(impl, tmp_str, ns_list, candidate, port, effective_opts, edns, cache_key);
		if (result) {
			return std::move(*result);
		}
		if (result.error().kind == DnsErrorKind::nxdomain) {
			last_nxdomain = result.error();
			continue;
		}
		return std::unexpected{result.error()};
	}
	return std::unexpected{last_nxdomain.value_or(DnsError{DnsErrorKind::nxdomain, "dns: name not found"})};
}

} // namespace
std::expected<ResolveResult, DnsError> Resolver::resolve_blocking(
	std::string_view host,
	std::uint16_t port,
	ResolveOptions const &opts) {
	if (current_resolver() == this && impl_->backend == ResolverBackend::native_udp) {
		return std::unexpected{
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
		return std::unexpected{
			DnsError{DnsErrorKind::invalid_hostname, std::format("invalid hostname '{}'", host)}
        };
	}

	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
		if (auto hosts_result = hosts_lookup_result(impl_->hosts_cache, host, port, effective_opts); hosts_result) {
			return *std::move(hosts_result);
		}
	}

	if (impl_->backend == ResolverBackend::nss_thread) {
		return resolve_blocking_nss_result(host, port, effective_opts);
	}

	return resolve_blocking_native_udp_result(impl_, host, port, effective_opts);
}
void Resolver::invalidate(
	std::string_view host) {
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
conflux::file_io::FileReader *Resolver::file_reader() const noexcept {
	return impl_->reader.get();
}

} // namespace conflux::net::dns
