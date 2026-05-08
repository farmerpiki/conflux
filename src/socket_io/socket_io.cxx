module;
#include<cstdlib>
#include<cstring>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/mman.h>
#include<sys/socket.h>

export module conflux.socket_io;
import std;
import conflux.types;
// ─── handle types ────────────────────────────────────────────────────────────

export struct OsFd{
int value{-1};
};
export struct DirectSlot{
u32 value{0};
};
export struct SocketHandle{
u32 id{};
bool fixed{};
[[nodiscard]]static constexpr SocketHandle from_os(
int fd)noexcept{
return{.id=static_cast<u32>(fd),.fixed=false};
}
[[nodiscard]]static constexpr SocketHandle from_direct(
u32 slot)noexcept{
return{.id=slot,.fixed=true};
}
[[nodiscard]]constexpr int as_fd()const noexcept{return static_cast<int>(id);}
};
// ─── SocketRawRing ───────────────────────────────────────────────────────────
// Non-owning wrapper around io_uring* for raw SQE submission.
// Does NOT own CompletionTable — raw callers dispatch CQEs themselves.

export class SocketRawRing{
io_uring*ring_;
public:
explicit SocketRawRing(
io_uring*ring)noexcept
:ring_{ring}{}
[[nodiscard]]io_uring*ring()noexcept{return ring_;}
[[nodiscard]]io_uring_sqe*get_sqe()noexcept{
auto*sqe=io_uring_get_sqe(ring_);
if(!sqe){
io_uring_submit(ring_);
sqe=io_uring_get_sqe(ring_);
}
return sqe;
}
[[nodiscard]]unsigned sq_space_left()const noexcept{
return io_uring_sq_space_left(ring_);
}
int submit()noexcept{
return io_uring_submit(ring_);
}
};
// ─── GenerationTable ─────────────────────────────────────────────────────────
// Per-slot generation counters. Rejects stale CQEs from closed/reused sockets.

export class GenerationTable{
V<u32>gens_;
public:
explicit GenerationTable(
u32 capacity)
:gens_(capacity,0){}
void ensure_capacity(
u32 id){
if(id>=gens_.size())
gens_.resize(id+1,0);
}
[[nodiscard]]u32 current(
u32 id)const noexcept{
return id<static_cast<u32>(gens_.size())?gens_[id]:0;
}
u32 advance(
u32 id)noexcept{
ensure_capacity(id);
return++gens_[id];
}
[[nodiscard]]bool alive(
u32 id,
u32 gen)const noexcept{
return id<static_cast<u32>(gens_.size())&&gens_[id]==gen;
}
};
// ─── BufferRing ──────────────────────────────────────────────────────────────
// Owns a kernel buffer ring group. Manages slab allocation and recycling.

export struct BufferRingOptions{
u32 count{4096};
SZ buf_size{8192};
u16 group_id{0};
bool huge_pages{true};
};

export class RecvBuffer;
export class BufferRing{
struct SlabDeleter{
void operator()(byte*p)const noexcept{::free(p);}
};
io_uring_buf_ring*ring_{nullptr};
io_uring*uring_{nullptr};
UPD<byte[],SlabDeleter>slab_;
SZ buf_size_{};
u32 count_{};
u16 group_id_{};
u32 mask_{};
public:
BufferRing(
io_uring*uring,
BufferRingOptions opts)
:uring_{uring},buf_size_{opts.buf_size},count_{opts.count},group_id_{opts.group_id}{
SZ const slab_sz=static_cast<SZ>(count_)*buf_size_;
SZ const aligned_sz=(slab_sz+4095)&~SZ{4095};
auto*raw=static_cast<byte*>(::aligned_alloc(4096,aligned_sz));
if(!raw)throw std::bad_alloc{};
slab_.reset(raw);
if(opts.huge_pages){
::madvise(raw,slab_sz,MADV_HUGEPAGE);
::madvise(raw,slab_sz,MADV_DONTFORK);
}
int ret=0;
ring_=io_uring_setup_buf_ring(uring_,static_cast<unsigned>(count_),group_id_,0,&ret);
if(!ring_){
slab_.reset();
throw RE{format("io_uring_setup_buf_ring failed: {}",ret)};
}
mask_=static_cast<u32>(io_uring_buf_ring_mask(static_cast<u32>(count_)));
for(u32 i=0;i<count_;++i)
io_uring_buf_ring_add(ring_,raw+i*buf_size_,
static_cast<unsigned>(buf_size_),static_cast<u16>(i),static_cast<int>(mask_),static_cast<int>(i));
io_uring_buf_ring_advance(ring_,static_cast<int>(count_));
}
~BufferRing(){
if(ring_)
io_uring_free_buf_ring(uring_,ring_,static_cast<unsigned>(count_),group_id_);
}
BufferRing(BufferRing const&)=delete;
BufferRing&operator=(BufferRing const&)=delete;
BufferRing(BufferRing&&)=delete;
BufferRing&operator=(BufferRing&&)=delete;
[[nodiscard]]span<byte const>buffer_view(
u16 id,
SZ len)const noexcept{
return{slab_.get()+static_cast<SZ>(id)*buf_size_,min(len,buf_size_)};
}
[[nodiscard]]span<byte>buffer_mut(
u16 id)noexcept{
return{slab_.get()+static_cast<SZ>(id)*buf_size_,buf_size_};
}
void recycle(
u16 id)noexcept{
io_uring_buf_ring_add(ring_,slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<unsigned>(buf_size_),id,static_cast<int>(mask_),0);
io_uring_buf_ring_advance(ring_,1);
}
void recycle_batch(
span<u16 const>ids)noexcept{
for(auto id:ids)
io_uring_buf_ring_add(ring_,slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<unsigned>(buf_size_),id,static_cast<int>(mask_),0);
io_uring_buf_ring_advance(ring_,static_cast<int>(ids.size()));
}
[[nodiscard]]RecvBuffer lease(u16 id,SZ len)noexcept;
[[nodiscard]]u16 group_id()const noexcept{return group_id_;}
[[nodiscard]]SZ buf_size()const noexcept{return buf_size_;}
[[nodiscard]]u32 count()const noexcept{return count_;}
};
// ─── RecvBuffer ──────────────────────────────────────────────────────────────
// RAII lease on a single buffer ring slot. Auto-recycles unless detached.

export class RecvBuffer{
BufferRing*ring_{nullptr};
u16 id_{};
SZ len_{};
bool armed_{true};
public:
RecvBuffer(
BufferRing*ring,
u16 id,
SZ len)noexcept
:ring_{ring},id_{id},len_{len}{}
RecvBuffer(RecvBuffer const&)=delete;
RecvBuffer&operator=(RecvBuffer const&)=delete;
RecvBuffer(
RecvBuffer&&o)noexcept
:ring_{exchange(o.ring_,nullptr)},id_{o.id_},len_{o.len_},armed_{exchange(o.armed_,false)}{}
RecvBuffer&operator=(
RecvBuffer&&o)noexcept{
if(this!=&o){
if(ring_&&armed_)ring_->recycle(id_);
ring_=exchange(o.ring_,nullptr);
id_=o.id_;
len_=o.len_;
armed_=exchange(o.armed_,false);
}
return*this;
}
~RecvBuffer(){
if(ring_&&armed_)ring_->recycle(id_);
}
[[nodiscard]]span<byte const>view()const noexcept{
return ring_?ring_->buffer_view(id_,len_):span<byte const>{};
}
[[nodiscard]]u16 id()const noexcept{return id_;}
[[nodiscard]]SZ size()const noexcept{return len_;}
void release()noexcept{
if(ring_&&armed_){
ring_->recycle(id_);
armed_=false;
}
}
void detach()noexcept{armed_=false;}
};
inline RecvBuffer BufferRing::lease(
u16 id,
SZ len)noexcept{
return RecvBuffer{this,id,len};
}
// ─── DirectFdTable ───────────────────────────────────────────────────────────
// Registers a sparse fixed-file table with io_uring.
// accept_direct auto-allocates slots; close_direct frees them.

export class DirectFdTable{
io_uring*ring_;
u32 capacity_{};
bool registered_{false};
public:
DirectFdTable(
io_uring*ring,
u32 max_slots)
:ring_{ring},capacity_{max_slots}{
if(io_uring_register_files_sparse(ring_,static_cast<unsigned>(capacity_))==0)
registered_=true;
}
~DirectFdTable(){
if(registered_)
io_uring_unregister_files(ring_);
}
DirectFdTable(DirectFdTable const&)=delete;
DirectFdTable&operator=(DirectFdTable const&)=delete;
DirectFdTable(DirectFdTable&&)=delete;
DirectFdTable&operator=(DirectFdTable&&)=delete;
[[nodiscard]]bool install(
u32 slot,
int fd){
return io_uring_register_files_update(ring_,slot,&fd,1)==1;
}
[[nodiscard]]bool registered()const noexcept{return registered_;}
[[nodiscard]]u32 capacity()const noexcept{return capacity_;}
};
// ─── CQE helpers ─────────────────────────────────────────────────────────────

export[[nodiscard]]inline u16 cqe_buffer_id(
u32 cqe_flags)noexcept{
return static_cast<u16>(cqe_flags>>IORING_CQE_BUFFER_SHIFT);
}
export[[nodiscard]]inline bool cqe_has_more(
u32 cqe_flags)noexcept{
return(cqe_flags&IORING_CQE_F_MORE)!=0;
}
export[[nodiscard]]inline bool cqe_has_buffer(
u32 cqe_flags)noexcept{
return(cqe_flags&IORING_CQE_F_BUFFER)!=0;
}
// ─── raw submission: accept ──────────────────────────────────────────────────
// All borrowed data (buffers, iovecs) must remain valid until CQE completion.

export bool submit_accept_multishot(
SocketRawRing&ring,
SocketHandle listen,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
bool direct=true){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
if(direct)
io_uring_prep_multishot_accept_direct(sqe,listen.as_fd(),addr,addrlen,0);
else
io_uring_prep_multishot_accept(sqe,listen.as_fd(),addr,addrlen,0);
if(listen.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: recv ────────────────────────────────────────────────────

export bool submit_recv_multishot(
SocketRawRing&ring,
SocketHandle handle,
BufferRing&bufs,
u64 user_data,
bool bundle=false){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_recv_multishot(sqe,handle.as_fd(),nullptr,0,0);
sqe->buf_group=bufs.group_id();
if(bundle)
sqe->ioprio|=IORING_RECVSEND_BUNDLE;
unsigned flags=IOSQE_BUFFER_SELECT;
if(handle.fixed)
flags|=IOSQE_FIXED_FILE;
io_uring_sqe_set_flags(sqe,flags);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: send ────────────────────────────────────────────────────

export bool submit_send_borrowed(
SocketRawRing&ring,
SocketHandle handle,
void const*data,
SZ len,
u64 user_data,
int msg_flags=MSG_NOSIGNAL){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_send(sqe,handle.as_fd(),data,len,msg_flags);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
export bool submit_writev_borrowed(
SocketRawRing&ring,
SocketHandle handle,
iovec const*iov,
unsigned nr_vecs,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_writev(sqe,handle.as_fd(),iov,nr_vecs,0);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: shutdown + close ────────────────────────────────────────
// Linked HARDLINK: shutdown(WR) then close. Requires 2 SQE slots.
// Returns false if SQ has fewer than 2 slots (caller should submit and retry).

export[[nodiscard]]bool submit_shutdown_close(
SocketRawRing&ring,
SocketHandle handle,
u64 shutdown_ud,
u64 close_ud){
if(ring.sq_space_left()<2)
return false;
auto*sqe_shut=ring.get_sqe();
auto*sqe_close=ring.get_sqe();
io_uring_prep_shutdown(sqe_shut,handle.as_fd(),SHUT_WR);
unsigned shut_flags=IOSQE_IO_HARDLINK;
if(handle.fixed)
shut_flags|=IOSQE_FIXED_FILE;
io_uring_sqe_set_flags(sqe_shut,shut_flags);
io_uring_sqe_set_data64(sqe_shut,shutdown_ud);
if(handle.fixed)
io_uring_prep_close_direct(sqe_close,handle.id);
else
io_uring_prep_close(sqe_close,handle.as_fd());
io_uring_sqe_set_data64(sqe_close,close_ud);
return true;
}
// ─── raw submission: close ───────────────────────────────────────────────────

export bool submit_close(
SocketRawRing&ring,
SocketHandle handle,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
if(handle.fixed)
io_uring_prep_close_direct(sqe,handle.id);
else
io_uring_prep_close(sqe,handle.as_fd());
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: setsockopt ──────────────────────────────────────────────
// Async socket option via io_uring cmd_sock. Only works with fixed fds.

export bool submit_setsockopt(
SocketRawRing&ring,
SocketHandle handle,
int level,
int optname,
void const*optval,
socklen_t optlen,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_cmd_sock(sqe,SOCKET_URING_OP_SETSOCKOPT,
handle.as_fd(),level,optname,
const_cast<void*>(optval),static_cast<int>(optlen));
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: cancel ──────────────────────────────────────────────────

export bool submit_cancel_fd(
SocketRawRing&ring,
SocketHandle handle,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_cancel_fd(sqe,handle.as_fd(),
handle.fixed?IORING_ASYNC_CANCEL_FD_FIXED:0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
export bool submit_cancel_by_ud(
SocketRawRing&ring,
u64 target_ud,
u64 cancel_ud){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_cancel64(sqe,target_ud,0);
io_uring_sqe_set_data64(sqe,cancel_ud);
return true;
}
// ─── raw submission: timeout ─────────────────────────────────────────────────

export bool submit_timeout(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_timeout(sqe,ts,0,0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
export bool submit_link_timeout(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_link_timeout(sqe,ts,0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: socket creation ─────────────────────────────────────────

export bool submit_socket(
SocketRawRing&ring,
int domain,
int type,
int protocol,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_socket(sqe,domain,type,protocol,0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
export bool submit_socket_direct(
SocketRawRing&ring,
int domain,
int type,
int protocol,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_socket_direct_alloc(sqe,domain,type,protocol,0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: connect ─────────────────────────────────────────────────
// addr must remain valid until CQE. Caller owns lifetime.

export bool submit_connect_borrowed(
SocketRawRing&ring,
SocketHandle handle,
sockaddr const*addr,
socklen_t addrlen,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_connect(sqe,handle.as_fd(),addr,addrlen);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: recv (single-shot, into caller buffer) ──────────────────

export bool submit_recv_borrowed(
SocketRawRing&ring,
SocketHandle handle,
void*buf,
SZ len,
u64 user_data,
int msg_flags=0){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_recv(sqe,handle.as_fd(),buf,len,msg_flags);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: sendmsg / recvmsg (UDP) ─────────────────────────────────

export bool submit_sendmsg_borrowed(
SocketRawRing&ring,
SocketHandle handle,
msghdr const*msg,
u64 user_data,
unsigned flags=0){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_sendmsg(sqe,handle.as_fd(),msg,flags);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
export bool submit_recvmsg_borrowed(
SocketRawRing&ring,
SocketHandle handle,
msghdr*msg,
u64 user_data,
unsigned flags=0){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_recvmsg(sqe,handle.as_fd(),msg,flags);
if(handle.fixed)
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
// ─── raw submission: linked recvmsg + timeout (UDP pattern) ──────────────────
// Requires 2 SQE slots. recvmsg is IO_LINK'd to a link_timeout.
// On timeout, recvmsg CQE arrives with res=-ECANCELED.
// Returns false if SQ has fewer than 2 slots.

export[[nodiscard]]bool submit_recvmsg_with_timeout(
SocketRawRing&ring,
SocketHandle handle,
msghdr*msg,
__kernel_timespec*ts,
u64 recv_ud,
u64 timeout_ud,
unsigned recv_flags=0){
if(ring.sq_space_left()<2)
return false;
auto*sqe_recv=ring.get_sqe();
io_uring_prep_recvmsg(sqe_recv,handle.as_fd(),msg,recv_flags);
unsigned flags=IOSQE_IO_LINK;
if(handle.fixed)
flags|=IOSQE_FIXED_FILE;
io_uring_sqe_set_flags(sqe_recv,flags);
io_uring_sqe_set_data64(sqe_recv,recv_ud);
auto*sqe_to=ring.get_sqe();
io_uring_prep_link_timeout(sqe_to,ts,0);
io_uring_sqe_set_data64(sqe_to,timeout_ud);
return true;
}
// ─── raw submission: fixed fd install ────────────────────────────────────────

export bool submit_fixed_fd_install(
SocketRawRing&ring,
u32 direct_slot,
u64 user_data){
auto*sqe=ring.get_sqe();
if(!sqe)return false;
io_uring_prep_fixed_fd_install(sqe,static_cast<int>(direct_slot),0);
io_uring_sqe_set_data64(sqe,user_data);
return true;
}
