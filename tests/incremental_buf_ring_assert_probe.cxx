/*Standalone binary:triggers buffer_slice_from_incremental_cqe assert paths.
 *argv[1]selects the probe:
 *inc_neg_res—res<0 with IORING_CQE_F_BUFFER set→assert(res>0)fires
 *inc_no_buf_flag—res>0,no IORING_CQE_F_BUFFER→assert(cqe_has_buffer)fires
 *inc_wrong_mode—classicring+incremental flags→assert(mode==incremental)fires
 *inc_bad_id—buf_id>=count→assert(id<ring.count())fires
 *inc_len_overflow—res>buf_size→assert(res<=buf_size-off)fires
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
ring{uring.ref(),BufferRingOptions{.count=8,.buf_size=64,.group_id=1,.huge_pages=false,.mode=BufferRingMode::incremental},conflux::uring::detect_caps(uring.ref())}{}
};
conflux::uring::CqeFlags inc_flags(
	std::uint16_t buf_id,
	bool buf_more) noexcept {
	return cqe_buffer_flags(conflux::uring::BufId{buf_id}, buf_more);
}

} // namespace
int main(
	int argc,
	char *argv[]) {
	::signal(SIGABRT, [](int) { ::_exit(42); });
	if (argc < 2) {
		return 1;
	}
	std::string_view probe{argv[1]};
	// inc_wrong_mode: classic ring — no incremental cap required.
	if (probe == "inc_wrong_mode") {
		auto r = conflux::uring::Ring::init(32, {});
		if (!r) {
			return 1;
		}
		conflux::uring::IoUringCaps caps{};
		BufferRing classic{
			r->ref(),
			BufferRingOptions{
							  .count = 8,
							  .buf_size = 64,
							  .group_id = 1,
							  .huge_pages = false,
							  .mode = BufferRingMode::classic_one_cqe_per_buffer},
			caps
        };
		auto _ = buffer_slice_from_incremental_cqe(classic, 8, inc_flags(0, false));
		return 0;
	}
	// Remaining probes need IOU_PBUF_RING_INC.
	{
		auto r = conflux::uring::Ring::init(32, {});
		if (!r) {
			return 1;
		}
		if (!conflux::uring::detect_caps(r->ref()).feat_pbuf_ring_inc) {
			return 1;
		}
	}
	if (probe == "inc_neg_res") {
		Rig rig{};
		auto _ = buffer_slice_from_incremental_cqe(rig.ring, -12, inc_flags(0, false));
		return 0;
	}
	if (probe == "inc_no_buf_flag") {
		Rig rig{};
		auto _ = buffer_slice_from_incremental_cqe(rig.ring, 8, conflux::uring::CqeFlags{});
		return 0;
	}
	if (probe == "inc_bad_id") {
		Rig rig{}; // count=8; id=9 exceeds it
		auto _ = buffer_slice_from_incremental_cqe(rig.ring, 8, inc_flags(9, false));
		return 0;
	}
	if (probe == "inc_len_overflow") {
		Rig rig{}; // buf_size=64; res=65 overflows
		auto _ = buffer_slice_from_incremental_cqe(rig.ring, 65, inc_flags(0, false));
		return 0;
	}
	return 1;
}
