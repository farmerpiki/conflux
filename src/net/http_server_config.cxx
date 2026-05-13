module;
#include <liburing.h>

export module conflux.net.http_server_config;

import std;
import conflux.types;
import conflux.net.config;

namespace {

template<typename Fn>
S format_flag_list(
	char sep,
	Fn &&fn) {
	S s;
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

export [[gnu::pure]] u32 build_uring_flags(
	Config const &c) {
	u32 f = 0;
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

export [[nodiscard]] u32 wq_fd_for_ring(
	Config const &c,
	unsigned i,
	int parent_ring_fd) {
	if (!c.attach_wq || i == 0 || parent_ring_fd < 0) {
		return 0;
	}
	return static_cast<u32>(parent_ring_fd);
}

export [[nodiscard]] S setup_flags_str(u32 flags) {
	return format_flag_list(',', [&](auto app) {
		if ((flags & IORING_SETUP_SINGLE_ISSUER) != 0u) {
			app("SINGLE_ISSUER");
		}
		if ((flags & IORING_SETUP_DEFER_TASKRUN) != 0u) {
			app("DEFER_TASKRUN");
		}
		if ((flags & IORING_SETUP_SQPOLL) != 0u) {
			app("SQPOLL");
		}
		if ((flags & IORING_SETUP_IOPOLL) != 0u) {
			app("IOPOLL");
		}
		if ((flags & IORING_SETUP_COOP_TASKRUN) != 0u) {
			app("COOP_TASKRUN");
		}
		if ((flags & IORING_SETUP_TASKRUN_FLAG) != 0u) {
			app("TASKRUN_FLAG");
		}
		if ((flags & IORING_SETUP_SUBMIT_ALL) != 0u) {
			app("SUBMIT_ALL");
		}
		if ((flags & IORING_SETUP_ATTACH_WQ) != 0u) {
			app("ATTACH_WQ");
		}
		if ((flags & IORING_SETUP_NO_SQARRAY) != 0u) {
			app("NO_SQARRAY");
		}
		if ((flags & IORING_SETUP_CQE_MIXED) != 0u) {
			app("CQE_MIXED");
		}
	});
}

export [[nodiscard]] S flags_str(
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
