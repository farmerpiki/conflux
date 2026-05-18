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
	Config const &c) {
	std::uint32_t f = 0;
	if (c.single_issuer) {
		f |= IORING_SETUP_SINGLE_ISSUER;
	}
	if (c.defer_taskrun) {
		f |= IORING_SETUP_DEFER_TASKRUN;
	}
	if (c.sqpoll) {
		f |= IORING_SETUP_SQPOLL;
	}
	if (c.coop_taskrun) {
		f |= IORING_SETUP_COOP_TASKRUN;
	}
	if (c.taskrun_flag) {
		f |= IORING_SETUP_TASKRUN_FLAG;
	}
	if (c.submit_all) {
		f |= IORING_SETUP_SUBMIT_ALL;
	}
	if (c.no_sqarray) {
		f |= IORING_SETUP_NO_SQARRAY;
	}
	if (c.cqe_mixed) {
		f |= IORING_SETUP_CQE_MIXED;
	}
	return f;
}

export [[nodiscard]] std::optional<std::uint32_t> next_uring_setup_flag_to_strip(
	std::uint32_t flags) {
	auto const stripped = conflux::uring::next_setup_flag_to_strip(conflux::uring::SetupFlags{flags});
	if (!stripped) {
		return std::nullopt;
	}
	return stripped->raw();
}

export [[nodiscard]] std::uint32_t wq_fd_for_ring(
	Config const &c,
	unsigned i,
	int parent_ring_fd) {
	if (!c.attach_wq || i == 0 || parent_ring_fd < 0) {
		return 0;
	}
	return static_cast<std::uint32_t>(parent_ring_fd);
}

export [[nodiscard]] std::string setup_flags_str(std::uint32_t flags) {
	return conflux::uring::setup_flags_str(conflux::uring::SetupFlags{flags});
}

export [[nodiscard]] std::string flags_str(
	Config const &c) {
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
