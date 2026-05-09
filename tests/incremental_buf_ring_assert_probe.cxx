/*Standalone binary:triggers buffer_slice_from_incremental_cqe assert paths.
*argv[1]selects the probe:
*inc_neg_res—res<0 with IORING_CQE_F_BUFFER set→assert(res>0)fires
*/
#include<csignal>
#include<cstdlib>
#include<liburing.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
namespace{
struct Rig{
conflux::uring::Ring uring;
BufferRing ring;
Rig()
:uring{[]{
auto r=conflux::uring::Ring::init(32,{});
if(!r)::_exit(2);
return move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=8,.buf_size=64,.group_id=1,.huge_pages=false,.mode=BufferRingMode::incremental}}{}
};
u32 inc_flags(u16 buf_id,bool buf_more)noexcept{
u32 f=IORING_CQE_F_BUFFER|(static_cast<u32>(buf_id)<<IORING_CQE_BUFFER_SHIFT);
if(buf_more)f|=IORING_CQE_F_BUF_MORE;
return f;
}
}// namespace
int main(int argc,char*argv[]){
::signal(SIGABRT,[](int){::_exit(42);});
if(argc<2)return 1;
SV probe{argv[1]};
if(probe=="inc_neg_res"){
Rig rig{};
auto _=buffer_slice_from_incremental_cqe(rig.ring,-12,inc_flags(0,false));
return 0;
}
return 1;
}
