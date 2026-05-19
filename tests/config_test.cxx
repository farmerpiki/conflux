// Plain TU — config parsing/formatting does not need a module test unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.net.config;
import conflux.net.http_server_config;

namespace {

class TempIni {
	std::filesystem::path path_;

public:
	explicit TempIni(
		std::string_view body) {
		auto const name = std::format(
			"conflux-config-test-{}-{}.ini",
			std::chrono::steady_clock::now().time_since_epoch().count(),
			reinterpret_cast<std::uintptr_t>(this));
		path_ = std::filesystem::temp_directory_path() / name;
		std::ofstream out{path_};
		if (!out) {
			throw std::runtime_error{"failed to create temp ini"};
		}
		out << body;
	}
	~TempIni() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	TempIni(TempIni const &) = delete;
	TempIni &operator =(TempIni const &) = delete;
	[[nodiscard]] char const *c_str() const noexcept { return path_.c_str(); }
};

} // namespace

TEST_CASE(
	"net.config: HTTP presets expose safe and explicit speed profiles",
	"[net.config]") {
	auto public_cfg = Config::public_server();
	CHECK(public_cfg.max_body_size == kConfigDefaultMaxBodySize);
	CHECK(public_cfg.request_timeout_ms == kConfigDefaultRequestTimeoutMs);
	CHECK(public_cfg.tls_sniff_timeout_ms == kConfigDefaultTlsSniffTimeoutMs);
	CHECK(public_cfg.parser_limits.max_headers == kConfigDefaultMaxHeaders);

	auto dev = Config::development();
	CHECK(dev.slow_handler_diagnostics);
	CHECK(dev.startup_banner);

	auto bench = Config::benchmark();
	CHECK_FALSE(bench.startup_banner);
	CHECK(bench.request_timeout_ms == 0);
	CHECK(bench.tls_sniff_timeout_ms == 0);

	auto unsafe = Config::unsafe_max_speed();
	CHECK(unsafe.max_body_size > public_cfg.max_body_size);
	CHECK(unsafe.parser_limits.max_headers > public_cfg.parser_limits.max_headers);
	CHECK(unsafe.send_fixed_buffers);
	CHECK(unsafe.send_zc == "on");
}

TEST_CASE(
	"net.config: ini parses perf/isolation knobs",
	"[net.config]") {
	TempIni ini{R"ini(
[server]
port = 0
busy_poll_us = 75
ring_core = 4
worker_core_base = 12
startup_banner = false

[io_uring]
prefer_busy_poll = yes
direct_accept = no
cmd_sock_setsockopt = false
auto_recv_arm_policy = true
cqe_mixed = true
submit_all = true
)ini"};

	auto cfg = config_from_ini(ini.c_str());

	CHECK(cfg.port == 0);
	CHECK(cfg.busy_poll_us == 75);
	CHECK(cfg.ring_core == 4);
	CHECK(cfg.worker_core_base == 12);
	CHECK_FALSE(cfg.startup_banner);
	CHECK(cfg.prefer_busy_poll);
	CHECK_FALSE(cfg.direct_accept);
	CHECK_FALSE(cfg.cmd_sock_setsockopt);
	CHECK(cfg.auto_recv_arm_policy);
	CHECK(cfg.cqe_mixed);
	CHECK(cfg.submit_all);
}

TEST_CASE(
	"net.config: ini parses HTTP limits and timeout hardening knobs",
	"[net.config]") {
	TempIni ini{R"ini(
[server]
max_body_size = 4096
request_timeout_ms = 12000
tls_sniff_timeout_ms = 3000
max_request_line_size = 2048
max_header_line_size = 1024
max_headers = 32
max_header_block_size = 8192
max_chunks = 128

[http3]
max_body_size = 8192
)ini"};

	auto cfg = config_from_ini(ini.c_str());

	CHECK(cfg.max_body_size == 4096);
	CHECK(cfg.request_timeout_ms == 12000);
	CHECK(cfg.tls_sniff_timeout_ms == 3000);
	CHECK(cfg.parser_limits.max_request_line_size == 2048);
	CHECK(cfg.parser_limits.max_header_line_size == 1024);
	CHECK(cfg.parser_limits.max_headers == 32);
	CHECK(cfg.parser_limits.max_header_block_size == 8192);
	CHECK(cfg.parser_limits.max_chunks == 128);
	CHECK(cfg.http3.max_body_size == 8192);
}

TEST_CASE(
	"net.config: checked ini loader returns expected errors",
	"[net.config]") {
	TempIni ok{R"ini(
[server]
port = 8088
startup_banner = false
)ini"};
	auto cfg = try_config_from_ini(ok.c_str());
	REQUIRE(cfg.has_value());
	CHECK(cfg->port == 8088);
	CHECK_FALSE(cfg->startup_banner);

	auto checked = config_from_ini_checked(ok.c_str());
	REQUIRE(checked.has_value());
	CHECK(checked->port == cfg->port);

	TempIni bad{R"ini(
[server]
port = nope
)ini"};
	auto invalid = try_config_from_ini(bad.c_str());
	REQUIRE_FALSE(invalid.has_value());
	CHECK(invalid.error().find("invalid integer") != std::string::npos);
}

TEST_CASE(
	"net.config: ini rejects invalid new unsigned knobs",
	"[net.config]") {
	TempIni ini{R"ini(
[server]
busy_poll_us = -1
)ini"};

	REQUIRE_THROWS_AS((void)config_from_ini(ini.c_str()), std::runtime_error);
}

TEST_CASE(
	"net.http_server_config: setup flag helpers expose exact active flag names",
	"[net.config]") {
	Config cfg{};
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = false;
	cfg.taskrun_flag = true;
	cfg.submit_all = true;
	cfg.no_sqarray = true;
	cfg.cqe_mixed = true;

	auto const flags = build_uring_flags(cfg);
	CHECK((flags & IORING_SETUP_SINGLE_ISSUER) != 0U);
	CHECK((flags & IORING_SETUP_DEFER_TASKRUN) != 0U);
	CHECK((flags & IORING_SETUP_TASKRUN_FLAG) != 0U);
	CHECK((flags & IORING_SETUP_SUBMIT_ALL) != 0U);
	CHECK((flags & IORING_SETUP_NO_SQARRAY) != 0U);
	CHECK((flags & IORING_SETUP_CQE_MIXED) != 0U);
	CHECK((flags & IORING_SETUP_COOP_TASKRUN) == 0U);

	auto text = setup_flags_str(flags);
	CHECK(text.find("SINGLE_ISSUER") != std::string::npos);
	CHECK(text.find("DEFER_TASKRUN") != std::string::npos);
	CHECK(text.find("TASKRUN_FLAG") != std::string::npos);
	CHECK(text.find("SUBMIT_ALL") != std::string::npos);
	CHECK(text.find("NO_SQARRAY") != std::string::npos);
	CHECK(text.find("CQE_MIXED") != std::string::npos);
	CHECK(setup_flags_str(0) == "none");
}

TEST_CASE(
	"net.http_server_config: EINVAL fallback strips setup flags in proposal order",
	"[net.config]") {
	std::uint32_t flags = IORING_SETUP_CQE_MIXED
						| IORING_SETUP_NO_SQARRAY
						| IORING_SETUP_SUBMIT_ALL
						| IORING_SETUP_TASKRUN_FLAG
						| IORING_SETUP_DEFER_TASKRUN
						| IORING_SETUP_SINGLE_ISSUER;
	std::vector<std::uint32_t> stripped;
	while (auto const bit = next_uring_setup_flag_to_strip(flags)) {
		stripped.push_back(*bit);
		flags &= ~*bit;
	}
	CHECK(flags == 0U);
	CHECK(
		stripped
		== std::vector<std::uint32_t>{
			IORING_SETUP_CQE_MIXED,
			IORING_SETUP_NO_SQARRAY,
			IORING_SETUP_SUBMIT_ALL,
			IORING_SETUP_TASKRUN_FLAG,
			IORING_SETUP_DEFER_TASKRUN,
			IORING_SETUP_SINGLE_ISSUER,
		});
	CHECK_FALSE(next_uring_setup_flag_to_strip(0).has_value());
}

TEST_CASE(
	"uring: setup flag fallback helpers expose exact requested active stripped text",
	"[uring]") {
	auto flags = conflux::uring::setup_flags::cqe_mixed
			   | conflux::uring::setup_flags::no_sqarray
			   | conflux::uring::setup_flags::submit_all
			   | conflux::uring::setup_flags::taskrun_flag
			   | conflux::uring::setup_flags::defer_taskrun
			   | conflux::uring::setup_flags::single_issuer;
	std::vector<std::uint32_t> stripped;
	while (auto const bit = conflux::uring::next_setup_flag_to_strip(flags)) {
		stripped.push_back(bit->raw());
		flags &= ~*bit;
	}
	CHECK(flags.raw() == 0U);
	CHECK(
		stripped
		== std::vector<std::uint32_t>{
			IORING_SETUP_CQE_MIXED,
			IORING_SETUP_NO_SQARRAY,
			IORING_SETUP_SUBMIT_ALL,
			IORING_SETUP_TASKRUN_FLAG,
			IORING_SETUP_DEFER_TASKRUN,
			IORING_SETUP_SINGLE_ISSUER,
		});
	CHECK(conflux::uring::setup_flags_str(conflux::uring::SetupFlags{}) == "none");
	CHECK(conflux::uring::setup_flags_str(conflux::uring::setup_flags::cqe_mixed) == "CQE_MIXED");
}

TEST_CASE(
	"net.http_server_config: public config flag text reports disabled compatibility knobs",
	"[net.config]") {
	Config cfg{};
	cfg.auto_recv_arm_policy = true;
	cfg.direct_accept = false;
	cfg.cmd_sock_setsockopt = false;

	auto text = flags_str(cfg);
	CHECK(text.find("AUTO_RECV_ARM") != std::string::npos);
	CHECK(text.find("DIRECT_ACCEPT_OFF") != std::string::npos);
	CHECK(text.find("CMD_SOCK_SOCKOPTS_OFF") != std::string::npos);
	CHECK(flags_str(Config{}) != "none");
}

TEST_CASE(
	"net.http_server_config: shared io-wq fd attaches only child rings",
	"[net.config]") {
	Config cfg{};
	cfg.attach_wq = true;

	CHECK(wq_fd_for_ring(cfg, 0, 77) == 0U);
	CHECK(wq_fd_for_ring(cfg, 1, 77) == 77U);
	CHECK(wq_fd_for_ring(cfg, 2, -1) == 0U);

	cfg.attach_wq = false;
	CHECK(wq_fd_for_ring(cfg, 1, 77) == 0U);
}
