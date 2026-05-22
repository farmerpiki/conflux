/*Standalone binary:triggers specific buffer_slices_from_cqe assert paths.
 *A SIGABRT handler converts abort()to _exit(42)so the parent can detect
 *that the assert fired(exit code 42)vs normal exit(0)vs other crash(-1).
 *argv[1]selects the probe:
 *desync—ID at head_pos doesn't match CQE buf_id(test 6)
 *neg_res_buf_flag—res<0 with IORING_CQE_F_BUFFER set(test 12)
 */
#include <csignal>
#include <cstdlib>
#include <liburing.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
namespace {

conflux::uring::CqeFlags recv_flags_for(
	std::uint16_t buf_id) noexcept {
	return cqe_buffer_flags(conflux::uring::BufId{buf_id});
}
struct Rig {
	conflux::uring::Ring uring;
	BufferRing ring;
	Rig()
:uring{[]{
auto r=conflux::uring::Ring::init(32,{});
if(!r){::_exit(2);
}
return std::move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=8,.buf_size=64,.group_id=0,.huge_pages=false,.mode=BufferRingMode::classic_one_cqe_per_buffer},conflux::uring::detect_caps(uring.ref())}{}
};

} // namespace
int main(
	int argc,
	char *argv[]) {
	// Convert SIGABRT (raised by abort() inside assert()) to exit(42).
	// Uses _exit to bypass C++ destructors and avoid re-raising.
	::signal(SIGABRT, [](int) { ::_exit(42); });

	if (argc < 2) {
		return 1;
	}
	std::string_view probe{argv[1]};

	if (probe == "desync") {
		// ring_id_at(head_pos=0)==0, but we pass buf_id=5 → ID-match assert fires.
		Rig rig{};
		(void)buffer_slices_from_cqe(rig.ring, 64, recv_flags_for(5), false);
		return 0; // unreachable when assert fires
	}
	if (probe == "neg_res_buf_flag") {
		// res=-12 (ENOMEM) with IORING_CQE_F_BUFFER set → assert(!cqe_has_buffer) fires.
		Rig rig{};
		(void)buffer_slices_from_cqe(rig.ring, -12, recv_flags_for(0), false);
		return 0; // unreachable when assert fires
	}
	return 1; // unknown probe
}
