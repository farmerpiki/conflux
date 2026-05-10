module;
#include<cassert>
#include<cerrno>
#include<cstdlib>
#include<cstring>
#include<fcntl.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/mman.h>
#include<sys/socket.h>
#include<unistd.h>

struct io_uring;
struct io_uring_sqe;
struct __kernel_timespec;

export module conflux.socket_io;
import std;
import conflux.types;
import conflux.uring;
import conflux.uring.completion;
import conflux.uring.handle;
// ─── handle types ────────────────────────────────────────────────────────────

export using OwnedSocketHandle=IoHandle;
export using SocketHandle=RingFd;
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
[[nodiscard]]conflux::uring::Sqe try_get_sqe()const noexcept{return ring_.try_get_sqe();}
[[nodiscard]]io_uring_sqe*get_sqe()const noexcept{
auto sqe=try_get_sqe();
return sqe?sqe.raw():nullptr;
}
[[nodiscard]]unsigned sq_space_left()const noexcept{return ring_.sq_space_left();}
[[nodiscard]]int submit()const noexcept{return ring_.submit();}
[[nodiscard]]bool reserve_sqe_slots(
u32 n)const noexcept{
return sq_space_left()>=n;
}
[[nodiscard]]conflux::uring::Sqe get_reserved_sqe()const noexcept{
auto sqe=try_get_sqe();
assert(sqe);
return sqe;
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

export class IncrementalRecvSlice;

export enum class BufferRingMode:u8{
classic_one_cqe_per_buffer,
recv_bundle,
incremental
};
export struct BufferRingOptions{
u32 count{4096};
SZ buf_size{8192};
u16 group_id{0};
bool huge_pages{true};
BufferRingMode mode{BufferRingMode::classic_one_cqe_per_buffer};
};

export enum class RecvDecodeError:u8{
bad_cqe,
bad_id,
bad_bounds
};
export class RecvBuffer;
export class BufferRing{
struct SlabDeleter{
void operator()(
byte*p)const noexcept{
::free(p);
}
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
BufferRingMode mode_{BufferRingMode::classic_one_cqe_per_buffer};
V<SZ>incremental_offsets_{};
friend class IncrementalRecvSlice;
IncrementalRecvSlice friend buffer_slice_from_incremental_cqe(BufferRing&,int,u32)noexcept;
expected<IncrementalRecvSlice,RecvDecodeError>friend try_buffer_slice_from_incremental_cqe(BufferRing&,int,u32)noexcept;
[[nodiscard]]SZ&incremental_offset_ref(
u16 id)noexcept{
assert(mode_==BufferRingMode::incremental);
assert(id<count_);
return incremental_offsets_[id];
}
[[nodiscard]]span<byte const>buffer_view_at_offset(
u16 id,
SZ offset,
SZ len)const noexcept{
return{slab_.get()+static_cast<SZ>(id)*buf_size_+offset,len};
}
public:
BufferRing(
io_uring*uring,
BufferRingOptions opts,
conflux::uring::IoUringCaps const&caps)
:BufferRing(conflux::uring::RingRef{uring},opts,caps){}
BufferRing(
conflux::uring::RingRef uring,
BufferRingOptions opts,
conflux::uring::IoUringCaps const&caps)
:ring_{},uring_{uring},buf_size_{opts.buf_size},count_{opts.count},group_id_{opts.group_id},mode_{opts.mode}{
if(opts.mode==BufferRingMode::incremental&&!caps.feat_pbuf_ring_inc)
throw RE{"BufferRingMode::incremental requires IORING_FEAT_PBUF_RING_INC (kernel 6.12+)"};
if(count_==0||count_>32768U||(count_&(count_-1))!=0||buf_size_==0||buf_size_>static_cast<SZ>(NL<u16>::max())||count_>NL<SZ>::max()/buf_size_)
throw RE{"BufferRing invalid options"};
static_assert(conflux::uring::buf_ring_flags::has_inc,"IOU_PBUF_RING_INC required");
slab_sz_=static_cast<SZ>(count_)*buf_size_;
SZ const aligned_sz=(slab_sz_+4095)&~SZ{4095};
if(aligned_sz<slab_sz_)
throw RE{"BufferRing allocation overflow"};
auto*raw=static_cast<byte*>(::aligned_alloc(4096,aligned_sz));
if(raw==nullptr)
throw std::bad_alloc{};
slab_.reset(raw);
if(opts.huge_pages){
::madvise(raw,slab_sz_,MADV_HUGEPAGE);
::madvise(raw,slab_sz_,MADV_DONTFORK);
}
unsigned const ring_flags=mode_==BufferRingMode::incremental?conflux::uring::buf_ring_flags::inc:0u;
auto built=conflux::uring::BufRing::setup(
uring_,
static_cast<unsigned>(count_),
conflux::uring::BufGroupId{group_id_},
ring_flags);
if(!built){
slab_.reset();
if(built.error()==EINVAL&&mode_==BufferRingMode::incremental)
throw RE{"io_uring_setup_buf_ring: incremental mode requires kernel 6.12+ (IORING_FEAT_PBUF_RING_INC)"};
throw RE{format("io_uring_setup_buf_ring failed: {}",built.error())};
}
ring_=move(*built);
for(u32 i=0;i<count_;++i)
ring_.add(
raw+i*buf_size_,
static_cast<u32>(buf_size_),
conflux::uring::BufId{static_cast<u16>(i)},
static_cast<int>(i));
ring_.advance(static_cast<int>(count_));
ring_order_.resize(count_);
for(u32 i=0;i<count_;++i)
ring_order_[i]=static_cast<u16>(i);
head_pos_=0;
tail_pos_=count_;
if(mode_==BufferRingMode::incremental)
incremental_offsets_.assign(count_,SZ{0});
}
~BufferRing()=default;
BufferRing(BufferRing const&)=delete;
BufferRing&operator=(BufferRing const&)=delete;
BufferRing(BufferRing&&)=delete;
BufferRing&operator=(BufferRing&&)=delete;
[[nodiscard]]span<byte const>buffer_view_checked(
u16 id,
SZ len)const noexcept{
if(id>=count_)
return{};
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
if(id>=count_)
return{};
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
ring_.add(
slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),
conflux::uring::BufId{id},
0);
ring_.advance(1);
++tail_pos_;
}
void recycle_batch(
span<u16 const>ids)noexcept{
u32 i=0;
for(auto id:ids){
ring_order_[(tail_pos_+i)%count_]=id;
ring_.add(
slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),
conflux::uring::BufId{id},
static_cast<int>(i));
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
ring_.add(
slab_.get()+static_cast<SZ>(id)*buf_size_,
static_cast<u32>(buf_size_),
conflux::uring::BufId{id},
static_cast<int>(i));
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
bool reclaim_incremental_partial(
u16 id)noexcept{
if(mode_!=BufferRingMode::incremental||id>=count_)
return false;
auto&off=incremental_offsets_[id];
if(off==0)
return false;
off=0;
consume(1);
recycle(id);
return true;
}
[[nodiscard]]u16 ring_id_at(
u32 pos)const noexcept{
return ring_order_[pos%count_];
}
[[nodiscard]]BufferRingMode mode()const noexcept{return mode_;}
[[nodiscard]]RecvBuffer lease(u16 id,SZ len)noexcept;
[[nodiscard]]u16 group_id()const noexcept{return group_id_;}
[[nodiscard]]SZ buf_size()const noexcept{return buf_size_;}
[[nodiscard]]u32 count()const noexcept{return count_;}
[[nodiscard]]u32 debug_head_pos()const noexcept{return head_pos_;}
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
if((ring_!=nullptr)&&armed_)
ring_->recycle(id_);
ring_=exchange(o.ring_,nullptr);
id_=o.id_;
len_=o.len_;
armed_=exchange(o.armed_,false);
}
return*this;
}
~RecvBuffer(){
if((ring_!=nullptr)&&armed_)
ring_->recycle(id_);
}
[[nodiscard]]span<byte const>view()const noexcept{
return(ring_!=nullptr)?ring_->buffer_view_checked(id_,len_):span<byte const>{};
}
[[nodiscard]]u16 id()const noexcept{return id_;}
[[nodiscard]]SZ size()const noexcept{return len_;}
void release()noexcept{
if((ring_!=nullptr)&&armed_){
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
RecvSlices(
BufferRing*ring,
u32 start,
u32 cnt,
SZ total)noexcept
:ring_{ring},start_pos_{start},count_{cnt},total_{total}{}
RecvSlices(RecvSlices const&)=delete;
RecvSlices&operator=(RecvSlices const&)=delete;
RecvSlices(
RecvSlices&&o)noexcept
:ring_{exchange(o.ring_,nullptr)},start_pos_{o.start_pos_},count_{o.count_},total_{o.total_},detached_{o.detached_}{}
RecvSlices&operator=(
RecvSlices&&o)noexcept{
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
bool operator==(
iterator const&o)const noexcept{
return idx_==o.idx_;
}
bool operator!=(
iterator const&o)const noexcept{
return idx_!=o.idx_;
}
};
[[nodiscard]]iterator begin()const noexcept{return{this,0};}
[[nodiscard]]iterator end()const noexcept{return{this,count_};}
void recycle_all()noexcept{
if((ring_==nullptr)||detached_)
return;
ring_->recycle_range(start_pos_,count_);
ring_=nullptr;
}
void detach()noexcept{detached_=true;}
};
// ─── IncrementalRecvSlice ────────────────────────────────────────────────────
// One CQE's worth of incremental buffer data. Recycles only on final CQE.

export class IncrementalRecvSlice{
BufferRing*ring_{};
u16 id_{};
SZ offset_{};
SZ len_{};
bool more_{};
bool detached_{false};
public:
IncrementalRecvSlice()noexcept=default;
IncrementalRecvSlice(
BufferRing*ring,
u16 id,
SZ offset,
SZ len,
bool more)noexcept
:ring_{ring},id_{id},offset_{offset},len_{len},more_{more}{}
IncrementalRecvSlice(IncrementalRecvSlice const&)=delete;
IncrementalRecvSlice&operator=(IncrementalRecvSlice const&)=delete;
IncrementalRecvSlice(
IncrementalRecvSlice&&o)noexcept
:ring_{exchange(o.ring_,nullptr)},id_{o.id_},offset_{o.offset_},len_{o.len_},more_{o.more_},detached_{o.detached_}{}
~IncrementalRecvSlice()noexcept{recycle_if_final();}
IncrementalRecvSlice&operator=(
IncrementalRecvSlice&&o)noexcept{
if(this!=&o){
recycle_if_final();
ring_=exchange(o.ring_,nullptr);
id_=o.id_;
offset_=o.offset_;
len_=o.len_;
more_=o.more_;
detached_=o.detached_;
}
return*this;
}
[[nodiscard]]bool valid()const noexcept{return ring_!=nullptr&&len_>0;}
[[nodiscard]]u16 id()const noexcept{return id_;}
[[nodiscard]]SZ offset()const noexcept{return offset_;}
[[nodiscard]]SZ size()const noexcept{return len_;}
[[nodiscard]]bool more()const noexcept{return more_;}
[[nodiscard]]span<byte const>bytes()const noexcept{
if(ring_==nullptr)
return{};
return ring_->buffer_view_at_offset(id_,offset_,len_);
}
void recycle_if_final()noexcept{
if(ring_==nullptr||detached_||more_)
return;
ring_->recycle(id_);
ring_=nullptr;
}
void detach()noexcept{detached_=true;}
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
export[[nodiscard]]inline bool cqe_has_buf_more(
u32 cqe_flags)noexcept{
return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::buf_more);
}
export[[nodiscard]]IncrementalRecvSlice buffer_slice_from_incremental_cqe(
BufferRing&ring,
int res,
u32 flags)noexcept{
assert(res>0);
assert(cqe_has_buffer(flags));
assert(ring.mode()==BufferRingMode::incremental);
u16 const id=cqe_buffer_id(flags);
assert(id<ring.count());
SZ&off=ring.incremental_offset_ref(id);
assert(off<=ring.buf_size());
assert(static_cast<SZ>(res)<=ring.buf_size()-off);
bool const more=cqe_has_buf_more(flags);
IncrementalRecvSlice slice{&ring,id,off,static_cast<SZ>(res),more};
off+=static_cast<SZ>(res);
if(!more){
off=0;
ring.consume(1);
}
return slice;
}
export[[nodiscard]]expected<IncrementalRecvSlice,RecvDecodeError>
try_buffer_slice_from_incremental_cqe(
BufferRing&ring,
int res,
u32 flags)noexcept{
if(res<=0||!cqe_has_buffer(flags)||ring.mode()!=BufferRingMode::incremental)
return unexpected(RecvDecodeError::bad_cqe);
u16 const id=cqe_buffer_id(flags);
if(id>=ring.count())
return unexpected(RecvDecodeError::bad_id);
SZ&off=ring.incremental_offset_ref(id);
SZ const len=static_cast<SZ>(res);
if(off>ring.buf_size()||len>ring.buf_size()-off)
return unexpected(RecvDecodeError::bad_bounds);
bool const more=cqe_has_buf_more(flags);
IncrementalRecvSlice slice{&ring,id,off,len,more};
off+=len;
if(!more){
off=0;
ring.consume(1);
}
return slice;
}
export[[nodiscard]]RecvSlices buffer_slices_from_cqe(
BufferRing&ring,
int res,
u32 flags,
bool bundle)noexcept{
if(res<=0||!cqe_has_buffer(flags)){
assert(!cqe_has_buffer(flags));
return{};
}
SZ const total=static_cast<SZ>(res);
u32 const cnt=bundle?static_cast<u32>((total+ring.buf_size()-1)/ring.buf_size()):1u;
assert(cnt>0);
assert(cnt<=ring.count());
u16 const first_id=cqe_buffer_id(flags);
assert(ring.ring_id_at(ring.debug_head_pos())==first_id);
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
auto _=ring_.unregister_files();
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
// ─── raw submission: accept ──────────────────────────────────────────────────
// All borrowed data (buffers, iovecs) must remain valid until CQE completion.

export bool submit_accept_multishot_borrowed(
SocketRawRing&ring,
SocketHandle listen,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
int accept_flags,
bool direct=true)noexcept{
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
if(direct)
sqe.prep_multishot_accept_direct(listen.sqe_fd(),addr,addrlen,accept_flags);
else
sqe.prep_multishot_accept(listen.sqe_fd(),addr,addrlen,accept_flags);
sqe.add_flags(listen.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_accept_multishot_borrowed(
SocketRawRing&ring,
SocketHandle listen,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
conflux::uring::IoUringCaps const&caps,
int accept_flags,
bool direct)noexcept{
return submit_accept_multishot_borrowed(
ring,
listen,
addr,
addrlen,
user_data,
accept_flags,
direct&&caps.accept_direct_supported);
}
// compat wrappers — zero accept_flags
export bool submit_accept_multishot_borrowed(
SocketRawRing&ring,
SocketHandle listen,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
bool direct=true)noexcept{
return submit_accept_multishot_borrowed(ring,listen,addr,addrlen,user_data,0,direct);
}
export bool submit_accept_multishot_borrowed(
SocketRawRing&ring,
SocketHandle listen,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
conflux::uring::IoUringCaps const&caps,
bool direct)noexcept{
return submit_accept_multishot_borrowed(ring,listen,addr,addrlen,user_data,caps,0,direct);
}
// ─── raw submission: recv ────────────────────────────────────────────────────

export enum class RecvArmPolicy:u8{
default_,
poll_first
};
export[[nodiscard]]RecvArmPolicy resolve_recv_arm_policy(
bool auto_enabled,
bool recv_poll_first,
bool have_last_flags,
u32 last_flags)noexcept{
if(!auto_enabled||!recv_poll_first||!have_last_flags)
return RecvArmPolicy::default_;
bool const nonempty=
conflux::uring::CqeFlags{last_flags}
.any(conflux::uring::cqe_flags::sock_nonempty);
return nonempty?RecvArmPolicy::default_:RecvArmPolicy::poll_first;
}
export bool submit_recv_multishot(
SocketRawRing&ring,
SocketHandle handle,
BufferRing&bufs,
u64 user_data,
bool bundle=false,
RecvArmPolicy arm=RecvArmPolicy::default_){
assert(!(bundle&&bufs.mode()==BufferRingMode::incremental));
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_recv_multishot(handle.sqe_fd(),nullptr,0,conflux::uring::MsgFlags{});
sqe.buf_group(conflux::uring::BufGroupId{bufs.group_id()});
conflux::uring::IoPrioFlags ioprio{};
if(bundle&&bufs.mode()==BufferRingMode::recv_bundle)
ioprio=ioprio|conflux::uring::ioprio_flags::recvsend_bundle;
if(arm==RecvArmPolicy::poll_first)
ioprio=ioprio|conflux::uring::ioprio_flags::recvsend_poll_first;
if(ioprio)
sqe.ioprio(ioprio);
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
if(!sqe)
return false;
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
if(!sqe)
return false;
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
if(!shutdown_sqe)
return false;
auto close_sqe=ring.try_get_sqe();
if(!close_sqe)
return false;
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
if(!sqe)
return false;
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
if(!shut_sqe)
return false;
auto close_sqe=ring.try_get_sqe();
if(!close_sqe)
return false;
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
if(!sqe)
return false;
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

export bool submit_setsockopt_borrowed(
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
if(!sqe)
return false;
sqe.prep_cmd_sock(
conflux::uring::uring_cmd_op::setsockopt,
handle.sqe_fd(),
level,
optname,
const_cast<void*>(optval),
static_cast<int>(optlen));
sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export struct DirectTcpAcceptSetup{
bool tcp_nodelay_once{false};// opt-in; requires caps.cmd_sock_setsockopt
bool tcp_quickack_once{false};// opt-in; requires caps.cmd_sock_setsockopt
bool skip_sockopt_success_cqes{true};
};
namespace{
static int const k_socket_opt_on=1;
}// namespace
export[[nodiscard]]bool submit_direct_tcp_accept_setup_recv(
SocketRawRing&ring,
SocketHandle direct_socket,
BufferRing&buffers,
u64 sockopt_ud,
u64 recv_ud,
DirectTcpAcceptSetup opts)noexcept{
if(!direct_socket.is_direct())
return false;
unsigned const needed=1U+(opts.tcp_nodelay_once?1U:0U)+(opts.tcp_quickack_once?1U:0U);
if(ring.sq_space_left()<needed)
return false;
if(opts.tcp_nodelay_once){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_cmd_sock(
conflux::uring::uring_cmd_op::setsockopt,
direct_socket.sqe_fd(),
IPPROTO_TCP,
TCP_NODELAY,
const_cast<int*>(&k_socket_opt_on),
static_cast<int>(sizeof(k_socket_opt_on)));
sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
if(opts.skip_sockopt_success_cqes)
sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
sqe.user_data(conflux::uring::UserData{sockopt_ud});
}
if(opts.tcp_quickack_once){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_cmd_sock(
conflux::uring::uring_cmd_op::setsockopt,
direct_socket.sqe_fd(),
IPPROTO_TCP,
TCP_QUICKACK,
const_cast<int*>(&k_socket_opt_on),
static_cast<int>(sizeof(k_socket_opt_on)));
sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
if(opts.skip_sockopt_success_cqes)
sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
sqe.user_data(conflux::uring::UserData{sockopt_ud});
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
if(!sqe)
return false;
sqe.prep_cancel_fd(
handle.sqe_fd(),
handle.fixed?conflux::uring::cancel_flags::fd_fixed:conflux::uring::CancelFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_cancel_by_ud(
SocketRawRing&ring,
u64 target_ud,
u64 cancel_ud){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_cancel64(conflux::uring::UserData{target_ud},conflux::uring::CancelFlags{});
sqe.user_data(conflux::uring::UserData{cancel_ud});
return true;
}
export enum class CancelPolicy:u8{
ignore,
cancel_sqe_by_user_data,
cancel_fd,
close_fd
};
// ─── raw submission: timeout ─────────────────────────────────────────────────

export bool submit_timeout_borrowed(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_timeout(ts,0,conflux::uring::TimeoutFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_link_timeout_borrowed(
SocketRawRing&ring,
__kernel_timespec*ts,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
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
if(!sqe)
return false;
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
if(!sqe)
return false;
sqe.prep_socket_direct_alloc(domain,type,protocol,0);
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
export bool submit_socket_direct(
SocketRawRing&ring,
int domain,
int type,
int protocol,
u64 user_data,
conflux::uring::IoUringCaps const&caps){
if(!caps.socket_direct_alloc)
return false;
return submit_socket_direct(ring,domain,type,protocol,user_data);
}
// ─── raw submission: connect ─────────────────────────────────────────────────
// addr must remain valid until CQE. Caller owns lifetime.

export bool submit_connect_borrowed(
SocketRawRing&ring,
SocketHandle handle,
sockaddr const*addr,
socklen_t addrlen,
u64 user_data,
bool link_next=false){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_connect(handle.sqe_fd(),addr,addrlen);
sqe.add_flags(handle.sqe_fd_flags());
if(link_next)
sqe.add_flags(conflux::uring::sqe_flags::io_link);
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
if(!sqe)
return false;
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
if(!sqe)
return false;
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
if(!sqe)
return false;
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
if(!recv_sqe)
return false;
auto timeout_sqe=ring.try_get_sqe();
if(!timeout_sqe)
return false;
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
if(!sqe)
return false;
sqe.prep_fixed_fd_install(conflux::uring::DirectSlot{direct_slot},conflux::uring::InstallFdFlags{});
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── TcpListener ─────────────────────────────────────────────────────────────

export enum class TcpBindAddress:u8{
loopback_v4,
any_v4,
loopback_v6,
any_v6_dual,
any_v6_only
};
export struct TcpListenerOptions{
u16 port{0};
TcpBindAddress bind{TcpBindAddress::any_v6_dual};
bool reuse_addr{true};
bool reuse_port{false};
int backlog{SOMAXCONN};
int accept_flags{SOCK_CLOEXEC|SOCK_NONBLOCK};
};
namespace{
struct FdGuard{
int fd{-1};
~FdGuard()noexcept{
if(fd>=0)
::close(fd);
}
};
}// namespace
export class TcpListener{
int fd_{-1};
u16 port_{};
int accept_flags_{SOCK_CLOEXEC|SOCK_NONBLOCK};
public:
explicit TcpListener(
TcpListenerOptions opts={}){
bool const is_v6=
(opts.bind==TcpBindAddress::loopback_v6||opts.bind==TcpBindAddress::any_v6_dual||opts.bind==TcpBindAddress::any_v6_only);
int const domain=is_v6?AF_INET6:AF_INET;
int const raw=::socket(domain,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
if(raw<0)
throw SE(errno,system_category(),"socket");
FdGuard guard{raw};
int const on=1;
if(opts.reuse_addr){
if(::setsockopt(raw,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on))<0)
throw SE(errno,system_category(),"SO_REUSEADDR");
}
if(opts.reuse_port){
if(::setsockopt(raw,SOL_SOCKET,SO_REUSEPORT,&on,sizeof(on))<0)
throw SE(errno,std::system_category(),"SO_REUSEPORT");
}
if(is_v6){
int const v6only=
(opts.bind==TcpBindAddress::loopback_v6||opts.bind==TcpBindAddress::any_v6_only)?1:0;
if(::setsockopt(raw,IPPROTO_IPV6,IPV6_V6ONLY,&v6only,sizeof(v6only))<0)
throw SE(errno,std::system_category(),"IPV6_V6ONLY");
sockaddr_in6 addr{};
addr.sin6_family=AF_INET6;
addr.sin6_port=htons(opts.port);
addr.sin6_addr=(opts.bind==TcpBindAddress::loopback_v6)?in6addr_loopback:in6addr_any;
if(::bind(raw,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)
throw SE(errno,std::system_category(),"bind");
}else{
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_port=htons(opts.port);
addr.sin_addr.s_addr=
(opts.bind==TcpBindAddress::loopback_v4)?htonl(INADDR_LOOPBACK):htonl(INADDR_ANY);
if(::bind(raw,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)
throw SE(errno,std::system_category(),"bind");
}
if(::listen(raw,opts.backlog)<0)
throw SE(errno,system_category(),"listen");
sockaddr_storage ss{};
socklen_t sslen=sizeof(ss);
if(::getsockname(raw,reinterpret_cast<sockaddr*>(&ss),&sslen)<0)
throw SE(errno,system_category(),"getsockname");
port_=(ss.ss_family==AF_INET6)?ntohs(reinterpret_cast<sockaddr_in6 const*>(&ss)->sin6_port):
ntohs(reinterpret_cast<sockaddr_in const*>(&ss)->sin_port);
accept_flags_=opts.accept_flags;
fd_=exchange(guard.fd,-1);
}
~TcpListener()noexcept{
if(fd_>=0)
::close(fd_);
}
TcpListener(TcpListener const&)=delete;
TcpListener&operator=(TcpListener const&)=delete;
TcpListener(
TcpListener&&o)noexcept
:fd_{exchange(o.fd_,-1)},port_{exchange(o.port_,u16{})},accept_flags_{o.accept_flags_}{}
TcpListener&operator=(
TcpListener&&o)noexcept{
if(this!=&o){
if(fd_>=0)
::close(fd_);
fd_=exchange(o.fd_,-1);
port_=exchange(o.port_,u16{});
accept_flags_=o.accept_flags_;
}
return*this;
}
[[nodiscard]]u16 port()const noexcept{return port_;}
[[nodiscard]]int raw_fd()const noexcept{return fd_;}
[[nodiscard]]SocketHandle handle()const noexcept{return SocketHandle::from_os(fd_);}
[[nodiscard]]bool arm_accept_multishot_borrowed(
SocketRawRing&ring,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
conflux::uring::IoUringCaps const&caps,
bool accept_direct=false)noexcept{
return submit_accept_multishot_borrowed(
ring,
handle(),
addr,
addrlen,
user_data,
caps,
accept_flags_,
accept_direct);
}
[[nodiscard]]bool rearm_accept_multishot_borrowed(
SocketRawRing&ring,
sockaddr*addr,
socklen_t*addrlen,
u64 user_data,
conflux::uring::IoUringCaps const&caps,
bool accept_direct=false)noexcept{
return submit_accept_multishot_borrowed(
ring,
handle(),
addr,
addrlen,
user_data,
caps,
accept_flags_,
accept_direct);
}
};
// ─── raw submission: standalone shutdown ──────────────────────────────────────

export bool submit_shutdown(
SocketRawRing&ring,
SocketHandle handle,
int how,
u64 user_data){
auto sqe=ring.try_get_sqe();
if(!sqe)
return false;
sqe.prep_shutdown(handle.sqe_fd(),how);
sqe.add_flags(handle.sqe_fd_flags());
sqe.user_data(conflux::uring::UserData{user_data});
return true;
}
// ─── SocketFdMode ─────────────────────────────────────────────────────────────

export enum class SocketFdMode:u8{
os_fd,
direct_if_available,
direct_required
};
// ─── ConnectOptions ───────────────────────────────────────────────────────────

export struct ConnectOptions{
chrono::milliseconds timeout{chrono::seconds{30}};
CancelPolicy cancel{CancelPolicy::cancel_fd};
bool tcp_nodelay{true};
bool tcp_quickack{false};
};
// ─── SocketTaskRing ───────────────────────────────────────────────────────────
// Thin wrapper: SocketRawRing + CompletionTable& + UserDataFn + options.
// Does NOT own the ring — ring lifetime is managed by the HTTP server.
// Does NOT own CompletionTable — owned by the caller.

export class SocketTaskRing;// forward declare before RingOpFn alias

export using RingOpFn=Fn<void(SocketTaskRing&)>;
export struct SocketTaskRingOptions{
SocketFdMode fd_mode{SocketFdMode::os_fd};// P1 safe default; direct_* is explicit opt-in until P1-04
conflux::uring::IoUringCaps const*caps{};
// Must enqueue fn on ring-owner thread and return true, or return false.
// Must NOT invoke fn inline from an arbitrary cancelling thread.
// If null: ring is treated as single-threaded; submit_on_owner asserts caller==owner
// and calls fn inline. Cross-thread cancel callers MUST provide submit_on_ring_owner.
Fn<bool(RingOpFn)>submit_on_ring_owner{};
};
export class SocketTaskRing{
SocketRawRing raw_;
CompletionTable*completions_{};
UserDataFn encode_ud_{};
SocketTaskRingOptions opts_{};
thread::id owner_thread_{std::this_thread::get_id()};
public:
SocketTaskRing(
SocketRawRing raw,
CompletionTable&completions,
UserDataFn encode_ud,
SocketTaskRingOptions opts={})noexcept
:raw_{raw},completions_{&completions},encode_ud_{std::move(encode_ud)},opts_{std::move(opts)}{}
SocketTaskRing(SocketTaskRing const&)=delete;
SocketTaskRing&operator=(SocketTaskRing const&)=delete;
SocketTaskRing(SocketTaskRing&&)=delete;
SocketTaskRing&operator=(SocketTaskRing&&)=delete;
[[nodiscard]]SocketRawRing&raw()noexcept{return raw_;}
[[nodiscard]]CompletionTable&completions()noexcept{return*completions_;}
[[nodiscard]]SocketTaskRingOptions const&opts()const noexcept{return opts_;}
[[nodiscard]]u64 encode(u32 slot,u32 gen)const{return encode_ud_(slot,gen);}
[[nodiscard]]bool submit_on_owner(RingOpFn fn){
if(opts_.submit_on_ring_owner)
return opts_.submit_on_ring_owner(move(fn));
assert(std::this_thread::get_id()==owner_thread_);
fn(*this);
return true;
}
};
