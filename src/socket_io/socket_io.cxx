module;
#include<cstdlib>
#include<cstring>
#include<fcntl.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/mman.h>
#include<sys/socket.h>

struct io_uring;
struct io_uring_sqe;
struct __kernel_timespec;

export module conflux.socket_io;
import std;
import conflux.types;
import conflux.uring;
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
[[nodiscard]]constexpr bool is_direct()const noexcept{return fixed;}
[[nodiscard]]constexpr bool is_os_fd()const noexcept{return!fixed;}
[[nodiscard]]constexpr int sqe_fd_value()const noexcept{return static_cast<int>(id);}
[[nodiscard]]constexpr conflux::uring::Fd sqe_fd()const noexcept{
return conflux::uring::Fd{sqe_fd_value()};
}
[[nodiscard]]constexpr conflux::uring::DirectSlot direct_slot()const noexcept{
return conflux::uring::DirectSlot{id};
}
[[nodiscard]]constexpr conflux::uring::SqeFlags sqe_fd_flags()const noexcept{
return fixed?conflux::uring::sqe_flags::fixed_file:conflux::uring::SqeFlags{};
}
[[nodiscard]]constexpr int as_fd()const noexcept{return sqe_fd_value();}
};
// ─── SocketRawRing ───────────────────────────────────────────────────────────
// Non-owning wrapper around io_uring* for raw SQE submission.
// Does NOT own CompletionTable — raw callers dispatch CQEs themselves.

export class SocketRawRing{
conflux::uring::RingRef ring_;
public:
explicit SocketRawRing(
io_uring*ring)noexcept
:SocketRawRing(conflux::uring::RingRef{ring}){}
explicit SocketRawRing(
io_uring&ring)noexcept
:SocketRawRing(conflux::uring::RingRef{ring}){}
explicit SocketRawRing(
conflux::uring::RingRef ring)noexcept
:ring_{ring}{}
[[nodiscard]]conflux::uring::RingRef ring()const noexcept{return ring_;}
[[nodiscard]]conflux::uring::Sqe try_get_sqe()const noexcept{
return ring_.try_get_sqe();
}
[[nodiscard]]io_uring_sqe*get_sqe()const noexcept{
auto sqe=try_get_sqe();
return sqe?sqe.raw():nullptr;
}
[[nodiscard]]unsigned sq_space_left()const noexcept{
return ring_.sq_space_left();
}
int submit()const noexcept{
return ring_.submit();
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
conflux::uring::BufRing ring_{};
conflux::uring::RingRef uring_{static_cast<io_uring*>(nullptr)};
UPD<byte[],SlabDeleter>slab_;
SZ buf_size_{};
u32 count_{};
u16 group_id_{};
SZ slab_sz_{};
V<u16>ring_order_;
u32 head_pos_{};
u32 tail_pos_{};
public:
BufferRing(
io_uring*uring,
BufferRingOptions opts)
:BufferRing(conflux::uring::RingRef{uring},opts){}
BufferRing(
conflux::uring::RingRef uring,
BufferRingOptions opts)
:ring_{},uring_{uring},buf_size_{opts.buf_size},count_{opts.count},group_id_{opts.group_id}{
if(count_==0||count_>65536U||(count_&(count_-1))!=0||buf_size_==0||
buf_size_>static_cast<SZ>(NL<u16>::max())||
count_>NL<SZ>::max()/buf_size_)
throw RE{"BufferRing invalid options"};
slab_sz_=static_cast<SZ>(count_)*buf_size_;
SZ const aligned_sz=(slab_sz_+4095)&~SZ{4095};
if(aligned_sz<slab_sz_)
throw RE{"BufferRing allocation overflow"};
auto*raw=static_cast<byte*>(::aligned_alloc(4096,aligned_sz));
if(!raw)throw std::bad_alloc{};
slab_.reset(raw);
if(opts.huge_pages){
::madvise(raw,slab_sz_,MADV_HUGEPAGE);
::madvise(raw,slab_sz_,MADV_DONTFORK);
}
auto built=conflux::uring::BufRing::setup(uring_,static_cast<unsigned>(count_),conflux::uring::BufGroupId{group_id_});
if(!built){
slab_.reset();
throw RE{format("io_uring_setup_buf_ring failed: {}",built.error())};
}
ring_=move(*built);
for(u32 i=0;i<count_;++i)
ring_.add(raw+i*buf_size_,static_cast<u32>(buf_size_),conflux::uring::BufId{static_cast<u16>(i)},static_cast<int>(i));
ring_.advance(static_cast<int>(count_));
ring_order_.resize(count_);
for(u32 i=0;i<count_;++i)
ring_order_[i]=static_cast<u16>(i);
head_pos_=0;
tail_pos_=count_;
}
~BufferRing(){
}
BufferRing(BufferRing const&)=delete;
BufferRing&operator=(BufferRing const&)=delete;
BufferRing(BufferRing&&)=delete;
BufferRing&operator=(BufferRing&&)=delete;
[[nodiscard]]span<byte const>buffer_view_checked(
u16 id,
SZ len)const noexcept{
if(id>=count_)return{};
return{slab_.get()+static_cast<SZ>(id)*buf_size_,min(len,buf_size_)};
}
[[nodiscard]]span<byte const>buffer_view_unchecked(
u16 id,
SZ len)const noexcept{
return{slab_.get()+static_cast<SZ>(id)*buf_size_,min(len,buf_size_)};
}
[[nodiscard]]span<byte const>buffer_view(
u16 id,
SZ len)const noexcept{
return buffer_view_checked(id,len);
}
[[nodiscard]]span<byte>buffer_mut_checked(
u16 id)noexcept{
if(id>=count_)return{};
return{slab_.get()+static_cast<SZ>(id)*buf_size_,buf_size_};
}
[[nodiscard]]span<byte>buffer_mut_unchecked(
u16 id)noexcept{
return{slab_.get()+static_cast<SZ>(id)*buf_size_,buf_size_};
}
[[nodiscard]]span<byte>buffer_mut(
u16 id)noexcept{
return buffer_mut_checked(id);
}
void recycle(
u16 id)noexcept{
ring_order_[tail_pos_%count_]=id;
ring_.add(slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),conflux::uring::BufId{id},0);
ring_.advance(1);
++tail_pos_;
}
void recycle_batch(
span<u16 const>ids)noexcept{
u32 i=0;
for(auto id:ids){
ring_order_[(tail_pos_+i)%count_]=id;
ring_.add(slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),conflux::uring::BufId{id},static_cast<int>(i));
++i;
}
ring_.advance(static_cast<int>(ids.size()));
tail_pos_+=static_cast<u32>(ids.size());
}
void recycle_range(
u32 start_pos,
u32 cnt)noexcept{
for(u32 i=0;i<cnt;++i){
u16 const id=ring_order_[(start_pos+i)%count_];
ring_order_[(tail_pos_+i)%count_]=id;
ring_.add(slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),conflux::uring::BufId{id},static_cast<int>(i));
}
ring_.advance(static_cast<int>(cnt));
tail_pos_+=cnt;
}
u32 consume(
u32 cnt)noexcept{
u32 const old=head_pos_;
head_pos_+=cnt;
return old;
}
[[nodiscard]]u16 ring_id_at(
u32 pos)const noexcept{
return ring_order_[pos%count_];
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
return ring_?ring_->buffer_view_checked(id_,len_):span<byte const>{};
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
// ─── RecvSlice / RecvSlices ──────────────────────────────────────────────────
// Zero-allocation view over one or more buffer-ring slots from a single CQE.
// No auto-recycle — caller must call recycle_all() or detach().

export struct RecvSlice{
u16 id;
span<byte const>bytes;
};
export class RecvSlices{
BufferRing*ring_{};
u32 start_pos_{};
u32 count_{};
SZ total_{};
bool detached_{false};
public:
RecvSlices()noexcept=default;
RecvSlices(BufferRing*ring,u32 start,u32 cnt,SZ total)noexcept
:ring_{ring},start_pos_{start},count_{cnt},total_{total}{}
RecvSlices(RecvSlices const&)=delete;
RecvSlices&operator=(RecvSlices const&)=delete;
RecvSlices(RecvSlices&&o)noexcept
:ring_{exchange(o.ring_,nullptr)},start_pos_{o.start_pos_},
count_{o.count_},total_{o.total_},detached_{o.detached_}{}
RecvSlices&operator=(RecvSlices&&o)noexcept{
if(this!=&o){
ring_=exchange(o.ring_,nullptr);
start_pos_=o.start_pos_;
count_=o.count_;
total_=o.total_;
detached_=o.detached_;
}
return*this;
}
[[nodiscard]]bool valid()const noexcept{return ring_!=nullptr&&count_>0;}
[[nodiscard]]SZ total_size()const noexcept{return total_;}
[[nodiscard]]u32 count()const noexcept{return count_;}
struct iterator{
RecvSlices const*slices_;
u32 idx_;
[[nodiscard]]RecvSlice operator*()const noexcept{
u16 const id=slices_->ring_->ring_id_at(slices_->start_pos_+idx_);
SZ const off=static_cast<SZ>(idx_)*slices_->ring_->buf_size();
SZ const len=(idx_+1<slices_->count_)?slices_->ring_->buf_size():slices_->total_-off;
return{id,slices_->ring_->buffer_view_unchecked(id,len)};
}
iterator&operator++()noexcept{
++idx_;
return*this;
}
bool operator==(iterator const&o)const noexcept{return idx_==o.idx_;}
bool operator!=(iterator const&o)const noexcept{return idx_!=o.idx_;}
};
[[nodiscard]]iterator begin()const noexcept{return{this,0};}
[[nodiscard]]iterator end()const noexcept{return{this,count_};}
void recycle_all()noexcept{
if(!ring_||detached_)return;
ring_->recycle_range(start_pos_,count_);
ring_=nullptr;
}
void detach()noexcept{detached_=true;}
};
export[[nodiscard]]RecvSlices buffer_slices_from_cqe(
BufferRing&ring,
conflux::uring::Cqe cqe,
bool bundle)noexcept{
if(cqe.res<=0)return{};
SZ const total=static_cast<SZ>(cqe.res);
u32 const cnt=bundle?static_cast<u32>((total+ring.buf_size()-1)/ring.buf_size()):1u;
u32 const start=ring.consume(cnt);
return RecvSlices{&ring,start,cnt,total};
}
// ─── DirectFdTable ───────────────────────────────────────────────────────────
// Registers a sparse fixed-file table with io_uring.
// accept_direct auto-allocates slots; close_direct frees them.

export class DirectFdTable{
conflux::uring::RingRef ring_;
u32 capacity_{};
int err_{0};
bool registered_{false};
public:
DirectFdTable(
io_uring*ring,
u32 max_slots)
:DirectFdTable(conflux::uring::RingRef{ring},max_slots){}
DirectFdTable(
conflux::uring::RingRef ring,
u32 max_slots)
:ring_{ring},capacity_{max_slots}{
err_=ring_.register_files_sparse(capacity_);
if(err_==0)
registered_=true;
}
~DirectFdTable(){
if(registered_)
ring_.unregister_files();
}
DirectFdTable(DirectFdTable const&)=delete;
DirectFdTable&operator=(DirectFdTable const&)=delete;
DirectFdTable(DirectFdTable&&)=delete;
DirectFdTable&operator=(DirectFdTable&&)=delete;
[[nodiscard]]bool install(
u32 slot,
int fd){
if(!registered_||slot>=capacity_)
return false;
return ring_.register_files_update(slot,span<int const>{&fd,1})==1;
}
[[nodiscard]]bool registered()const noexcept{return registered_;}
[[nodiscard]]int error()const noexcept{return err_;}
[[nodiscard]]u32 capacity()const noexcept{return capacity_;}
};
// ─── CQE helpers ─────────────────────────────────────────────────────────────

export[[nodiscard]]inline u16 cqe_buffer_id(
u32 cqe_flags)noexcept{
return conflux::uring::cqe_flags::buf_id(conflux::uring::CqeFlags{cqe_flags}).v;
}
export[[nodiscard]]inline bool cqe_has_more(
u32 cqe_flags)noexcept{
return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::more);
}
export[[nodiscard]]inline bool cqe_has_buffer(
u32 cqe_flags)noexcept{
return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::buffer);
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
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
if(direct)
sqe.prep_multishot_accept_direct(listen.sqe_fd(),addr,addrlen,0);
else
sqe.prep_multishot_accept(listen.sqe_fd(),addr,addrlen,0);
sqe.add_flags(listen.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── raw submission: recv ────────────────────────────────────────────────────

export bool submit_recv_multishot(
SocketRawRing&ring,
SocketHandle handle,
BufferRing&bufs,
u64 user_data,
bool bundle=false){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_recv_multishot(handle.sqe_fd(),nullptr,0,conflux::uring::MsgFlags{});
sqe.buf_group(conflux::uring::BufGroupId{bufs.group_id()});
if(bundle)
sqe.ioprio(conflux::uring::ioprio_flags::recvsend_bundle);
sqe.add_flags(conflux::uring::sqe_flags::buffer_select);
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
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
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_send(handle.sqe_fd(),data,len,conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_writev_borrowed(
SocketRawRing&ring,
SocketHandle handle,
iovec const*iov,
unsigned nr_vecs,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_writev(handle.sqe_fd(),iov,nr_vecs,0);
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
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
auto shutdown_sqe=ring.try_get_sqe();
if(!shutdown_sqe)return false;
auto close_sqe=ring.try_get_sqe();
if(!close_sqe)return false;
shutdown_sqe.prep_shutdown(handle.sqe_fd(),SHUT_WR);
shutdown_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
shutdown_sqe.add_flags(handle.sqe_fd_flags());
shutdown_sqe.user_data(conflux::uring::UserData{shutdown_ud});
if(handle.fixed)
close_sqe.prep_close_direct(handle.direct_slot());
else
close_sqe.prep_close(handle.sqe_fd());
close_sqe.user_data(conflux::uring::UserData{close_ud});
return true;
}
// ─── raw submission: close ───────────────────────────────────────────────────

export bool submit_close(
SocketRawRing&ring,
SocketHandle handle,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
if(handle.fixed)
sqe.prep_close_direct(handle.direct_slot());
else
sqe.prep_close(handle.sqe_fd());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export struct SocketCloseOptions{
bool shutdown_write{true};
bool skip_shutdown_success_cqe{true};
bool allow_async_shutdown_for_os_fd{false};
};
export[[nodiscard]]bool submit_close_fast(
SocketRawRing&ring,
SocketHandle handle,
u64 shutdown_ud,
u64 close_ud,
SocketCloseOptions opts)noexcept{
bool const needs_shutdown=handle.fixed?opts.shutdown_write:opts.allow_async_shutdown_for_os_fd;
unsigned const needed=1U+(needs_shutdown?1U:0U);
if(ring.sq_space_left()<needed)
return false;
if(needs_shutdown){
auto shut_sqe=ring.try_get_sqe();
if(!shut_sqe)return false;
auto close_sqe=ring.try_get_sqe();
if(!close_sqe)return false;
shut_sqe.prep_shutdown(handle.sqe_fd(),SHUT_WR);
shut_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
shut_sqe.add_flags(handle.sqe_fd_flags());
if(opts.skip_shutdown_success_cqe)
shut_sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
shut_sqe.user_data(conflux::uring::UserData{shutdown_ud});
if(handle.fixed)
close_sqe.prep_close_direct(handle.direct_slot());
else
close_sqe.prep_close(handle.sqe_fd());
close_sqe.user_data(conflux::uring::UserData{close_ud});
}else{
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
if(handle.fixed)
sqe.prep_close_direct(handle.direct_slot());
else
sqe.prep_close(handle.sqe_fd());
sqe.user_data(conflux::uring::UserData{close_ud});
}
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
if(!handle.fixed)
return false;
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_cmd_sock(conflux::uring::uring_cmd_op::setsockopt,
handle.sqe_fd(),level,optname,
const_cast<void*>(optval),static_cast<int>(optlen));
sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export struct DirectTcpAcceptSetup{
bool tcp_quickack_once{false};
bool skip_sockopt_success_cqes{true};
};
namespace{
static int k_socket_opt_on=1;
}
export[[nodiscard]]bool submit_direct_tcp_accept_setup_recv(
SocketRawRing&ring,
SocketHandle direct_socket,
BufferRing&buffers,
u64 quickack_ud,
u64 recv_ud,
DirectTcpAcceptSetup opts)noexcept{
if(!direct_socket.is_direct())
return false;
unsigned needed=1U+(opts.tcp_quickack_once?1U:0U);
if(ring.sq_space_left()<needed)
return false;
if(opts.tcp_quickack_once){
auto quickack_sqe=ring.try_get_sqe();
if(!quickack_sqe)
return false;
quickack_sqe.prep_cmd_sock(conflux::uring::uring_cmd_op::setsockopt,
direct_socket.sqe_fd(),IPPROTO_TCP,TCP_QUICKACK,
&k_socket_opt_on,static_cast<int>(sizeof(k_socket_opt_on)));
quickack_sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
quickack_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
if(opts.skip_sockopt_success_cqes)
quickack_sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
quickack_sqe.user_data(conflux::uring::UserData{quickack_ud});
}
auto recv_sqe=ring.try_get_sqe();
if(!recv_sqe)
return false;
recv_sqe.prep_recv_multishot(direct_socket.sqe_fd(),nullptr,0,conflux::uring::MsgFlags{});
recv_sqe.buf_group(conflux::uring::BufGroupId{buffers.group_id()});
recv_sqe.add_flags(conflux::uring::sqe_flags::buffer_select);
recv_sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
recv_sqe.user_data(conflux::uring::UserData{recv_ud});
return true;
}
// ─── raw submission: cancel ──────────────────────────────────────────────────

export bool submit_cancel_fd(
SocketRawRing&ring,
SocketHandle handle,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_cancel_fd(handle.sqe_fd(),
handle.fixed?conflux::uring::cancel_flags::fd_fixed:conflux::uring::CancelFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_cancel_by_ud(
SocketRawRing&ring,
u64 target_ud,
u64 cancel_ud){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_cancel64(conflux::uring::UserData{target_ud},conflux::uring::CancelFlags{});
sqe.user_data(conflux::uring::UserData{cancel_ud});
return true;
}
// ─── raw submission: timeout ─────────────────────────────────────────────────

export bool submit_timeout(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_timeout(ts,0,conflux::uring::TimeoutFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_link_timeout(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_link_timeout(ts,conflux::uring::TimeoutFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── raw submission: socket creation ─────────────────────────────────────────

export bool submit_socket(
SocketRawRing&ring,
int domain,
int type,
int protocol,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_socket(domain,type,protocol,0);
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_socket_direct(
SocketRawRing&ring,
int domain,
int type,
int protocol,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_socket_direct_alloc(domain,type,protocol,0);
sqe.user_data(conflux::uring::UserData{user_data});
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
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_connect(handle.sqe_fd(),addr,addrlen);
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
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
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_recv(handle.sqe_fd(),buf,len,conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── raw submission: sendmsg / recvmsg (UDP) ─────────────────────────────────

export bool submit_sendmsg_borrowed(
SocketRawRing&ring,
SocketHandle handle,
msghdr const*msg,
u64 user_data,
unsigned flags=0){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_sendmsg(handle.sqe_fd(),msg,conflux::uring::MsgFlags{flags});
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_recvmsg_borrowed(
SocketRawRing&ring,
SocketHandle handle,
msghdr*msg,
u64 user_data,
unsigned flags=0){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_recvmsg(handle.sqe_fd(),msg,conflux::uring::MsgFlags{flags});
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── raw submission: linked recvmsg + timeout (UDP pattern) ──────────────────
// Requires 2 SQE slots. recvmsg is IO_LINK'd to a link_timeout.
// On timeout, recvmsg CQE arrives with res=-ECANCELED.
// Returns false if SQ has fewer than 2 slots.

export[[nodiscard]]bool submit_recvmsg_timeout_borrowed(
SocketRawRing&ring,
SocketHandle handle,
msghdr*msg,
__kernel_timespec*ts,
u64 recv_ud,
u64 timeout_ud,
unsigned recv_flags=0){
if(ring.sq_space_left()<2)
return false;
auto recv_sqe=ring.try_get_sqe();
if(!recv_sqe)return false;
auto timeout_sqe=ring.try_get_sqe();
if(!timeout_sqe)return false;
recv_sqe.prep_recvmsg(handle.sqe_fd(),msg,conflux::uring::MsgFlags{recv_flags});
recv_sqe.add_flags(conflux::uring::sqe_flags::io_link);
recv_sqe.add_flags(handle.sqe_fd_flags());
recv_sqe.user_data(conflux::uring::UserData{recv_ud});
timeout_sqe.prep_link_timeout(ts,conflux::uring::TimeoutFlags{});
timeout_sqe.user_data(conflux::uring::UserData{timeout_ud});
return true;
}
// ─── raw submission: fixed fd install ────────────────────────────────────────

export bool submit_fixed_fd_install(
SocketRawRing&ring,
u32 direct_slot,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)return false;
sqe.prep_fixed_fd_install(conflux::uring::DirectSlot{direct_slot},conflux::uring::InstallFdFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
