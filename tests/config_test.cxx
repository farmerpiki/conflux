// Plain TU — config parsing/formatting does not need a module test unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http_server_config;

namespace {

class TempIni {
	std::filesystem::path path_;

public:
	explicit TempIni(
		SV body) {
		auto const name = format(
			"conflux-config-test-{}-{}.ini",
			std::chrono::steady_clock::now().time_since_epoch().count(),
			reinterpret_cast<std::uintptr_t>(this));
		path_ = std::filesystem::temp_directory_path() / name;
		std::ofstream out{path_};
		if (!out) {
			throw RE{"failed to create temp ini"};
		}
		out << body;
	}
	~TempIni() { std::error_code ec; std::filesystem::remove(path_, ec); }
	TempIni(TempIni const &) = delete;
	TempIni &operator =(TempIni const &) = delete;
	[[nodiscard]] char const *c_str() const noexcept { return path_.c_str(); }
};

} // namespace

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
	"net.config: ini rejects invalid new unsigned knobs",
	"[net.config]") {
	TempIni ini{R"ini(
[server]
busy_poll_us = -1
)ini"};

	REQUIRE_THROWS_AS((void)config_from_ini(ini.c_str()), RE);
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
	u32 flags = IORING_SETUP_CQE_MIXED
		| IORING_SETUP_NO_SQARRAY
		| IORING_SETUP_SUBMIT_ALL
		| IORING_SETUP_TASKRUN_FLAG
		| IORING_SETUP_DEFER_TASKRUN
		| IORING_SETUP_SINGLE_ISSUER;
	V<u32> stripped;
	while (auto const bit = next_uring_setup_flag_to_strip(flags)) {
		stripped.push_back(*bit);
		flags &= ~*bit;
	}
	CHECK(flags == 0U);
	CHECK(stripped == V<u32>{
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
