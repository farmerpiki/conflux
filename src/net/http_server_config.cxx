module;
#include <liburing.h>

export module conflux.net.http_server_config;

import std;
import conflux.types;
import conflux.uring;
import conflux.net.config;

namespace {

template<typename Fn>
std::string format_flag_list(
	char sep,
	Fn &&fn) {
	std::string s;
	auto app = [&](char const *name) {
		if (!s.empty()) {
			s += sep;
		}
		s += name;
	};
	fn(app);
	return s.empty() ? "none" : s;
}

} // namespace

export [[gnu::pure]] std::uint32_t build_uring_flags(
	conflux::http::Config const &c) {
	auto f = conflux::uring::SetupFlags{};
	if (c.single_issuer) {
		f |= conflux::uring::setup_flags::single_issuer;
	}
	if (c.defer_taskrun) {
		f |= conflux::uring::setup_flags::defer_taskrun;
	}
	if (c.sqpoll) {
		f |= conflux::uring::setup_flags::sqpoll;
	}
	if (c.coop_taskrun) {
		f |= conflux::uring::setup_flags::coop_taskrun;
	}
	if (c.taskrun_flag) {
		f |= conflux::uring::setup_flags::taskrun_flag;
	}
	if (c.submit_all) {
		f |= conflux::uring::setup_flags::submit_all;
	}
	if (c.no_sqarray) {
		f |= conflux::uring::setup_flags::no_sqarray;
	}
	if (c.cqe_mixed) {
		f |= conflux::uring::setup_flags::cqe_mixed;
	}
	return f.raw();
}

export [[nodiscard]] std::optional<conflux::uring::SetupFlags> next_uring_setup_flag_to_strip(
	std::uint32_t flags) {
	return conflux::uring::next_setup_flag_to_strip(conflux::uring::SetupFlags{flags});
}

export [[nodiscard]] std::uint32_t wq_fd_for_ring(
	conflux::http::Config const &c,
	unsigned i,
	int parent_ring_fd) {
	if (!c.attach_wq || i == 0 || parent_ring_fd < 0) {
		return 0;
	}
	return static_cast<std::uint32_t>(parent_ring_fd);
}

export [[nodiscard]] std::string setup_flags_str(
	std::uint32_t flags) {
	return conflux::uring::setup_flags_str(conflux::uring::SetupFlags{flags});
}

export [[nodiscard]] std::string flags_str(
	conflux::http::Config const &c) {
	return format_flag_list('|', [&](auto app) {
		if (c.single_issuer) {
			app("SINGLE_ISSUER");
		}
		if (c.defer_taskrun) {
			app("DEFER_TASKRUN");
		}
		if (c.sqpoll) {
			app("SQPOLL");
		}
		if (c.coop_taskrun) {
			app("COOP_TASKRUN");
		}
		if (c.taskrun_flag) {
			app("TASKRUN_FLAG");
		}
		if (c.submit_all) {
			app("SUBMIT_ALL");
		}
		if (c.attach_wq) {
			app("ATTACH_WQ");
		}
		if (c.no_sqarray) {
			app("NO_SQARRAY");
		}
		if (c.cqe_mixed) {
			app("CQE_MIXED");
		}
		if (c.no_mmap) {
			app("NO_MMAP");
		}
		if (c.recv_bundle && CONFLUX_ENABLE_RECV_BUNDLE) {
			app("RECV_BUNDLE");
		}
		if (c.recv_incremental_buf && CONFLUX_ENABLE_RECV_INCREMENTAL_BUF) {
			app("RECV_INCREMENTAL_BUF");
		}
		if (c.send_zc != "off" && CONFLUX_ENABLE_SEND_ZC) {
			app("SEND_ZC");
		}
		if (c.auto_recv_arm_policy) {
			app("AUTO_RECV_ARM");
		}
		if (!c.direct_accept) {
			app("DIRECT_ACCEPT_OFF");
		}
		if (!c.cmd_sock_setsockopt) {
			app("CMD_SOCK_SOCKOPTS_OFF");
		}
	});
}
