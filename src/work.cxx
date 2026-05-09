module;
#include<liburing.h>
#include<linux/futex.h>
#include<pthread.h>
#include<sched.h>
#include<sys/syscall.h>
#include<unistd.h>

export module conflux.work;

import std;
import conflux.types;
export import conflux.work.root;
export struct Cancelled final:RE{
Cancelled()
:RE{"work cancelled"}{}
};
namespace work_detail{
constexpr SZ kNoWorker=NL<SZ>::max();

using Fn=conflux::work::root::detail::small_move_only_function<void()>;
inline int futex_wait_private(
Atom<u32>&word,
u32 expected)noexcept{
auto*addr=reinterpret_cast<u32*>(&word);// NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
return static_cast<int>(::syscall(SYS_futex,addr,FUTEX_WAIT_PRIVATE,expected,nullptr,nullptr,0));
}
inline int futex_wake_private(
Atom<u32>&word,
int count)noexcept{
auto*addr=reinterpret_cast<u32*>(&word);// NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
return static_cast<int>(::syscall(SYS_futex,addr,FUTEX_WAKE_PRIVATE,count,nullptr,nullptr,0));
}
class QueueTarget{
public:
virtual~QueueTarget()=default;
virtual bool enqueue(conflux::work::root::detail::small_move_only_function<void()>job)=0;
};
// Vyukov-style MPMC bounded ring buffer, lock-free. Capacity rounded up to
// next power-of-2 at construction. try_push/try_pop are noexcept because Fn
// move-assign is noexcept.
class MpmcRing{
struct Slot{
Atom<SZ>seq{0};
Fn item{};
};
UP<Slot[]>slots_;
SZ capacity_{};
SZ mask_{};
alignas(64)Atom<SZ>head_{0};
alignas(64)Atom<SZ>tail_{0};
public:
explicit MpmcRing(SZ capacity){
SZ cap=1;
while(cap<capacity)
cap<<=1;
capacity_=cap;
mask_=cap-1;
slots_=make_unique<Slot[]>(cap);
for(SZ i=0;i<cap;++i)
slots_[i].seq.store(i,memory_order_relaxed);
}
MpmcRing(MpmcRing const&)=delete;
MpmcRing&operator=(MpmcRing const&)=delete;
MpmcRing(MpmcRing&&)=delete;
MpmcRing&operator=(MpmcRing&&)=delete;
[[nodiscard]]bool try_push(Fn fn)noexcept{
SZ pos=head_.load(memory_order_relaxed);
for(;;){
Slot&slot=slots_[pos&mask_];
SZ const seq=slot.seq.load(memory_order_acquire);
auto const diff=static_cast<std::intptr_t>(seq)-static_cast<std::intptr_t>(pos);
if(diff==0){
if(head_.compare_exchange_weak(pos,pos+1,memory_order_relaxed)){
slot.item=move(fn);
slot.seq.store(pos+1,memory_order_release);
return true;
}
}else if(diff<0){
return false;
}else{
conflux::work::root::detail::cpu_pause();
pos=head_.load(memory_order_relaxed);
}
}
}
[[nodiscard]]Opt<Fn>try_pop()noexcept{
SZ pos=tail_.load(memory_order_relaxed);
for(;;){
Slot&slot=slots_[pos&mask_];
SZ const seq=slot.seq.load(memory_order_acquire);
auto const diff=static_cast<std::intptr_t>(seq)-static_cast<std::intptr_t>(pos+1);
if(diff==0){
if(tail_.compare_exchange_weak(pos,pos+1,memory_order_relaxed)){
Fn job=move(slot.item);
slot.seq.store(pos+capacity_,memory_order_release);
return Opt<Fn>{move(job)};
}
}else if(diff<0){
return nullopt;
}else{
conflux::work::root::detail::cpu_pause();
pos=tail_.load(memory_order_relaxed);
}
}
}
};
}// namespace work_detail
export struct WorkPoolOptions{
SZ threads=0;
SZ max_inject_queue=4096;
SZ local_queue_capacity=1024;
u32 spin_before_park=256;
int numa_node=-1;
bool pin_workers=false;
S worker_name_prefix="conflux-work";
Fn<void(EP)>raw_exception_sink{};
};
export struct RingLaneOptions{
int ring_fd=-1;
u64 wake_user_data=0x434F4E464C5558ULL;// "CONFLUX"
SZ drain_budget=0;
bool allow_inline_on_owner=true;
};

export enum class WorkError:u8{
stopped,
queue_full,
wake_failed,
submit_failed,
cancelled,
owner_violation
};
export class WorkPool final:public work_detail::QueueTarget{
struct alignas(
64)Worker{
mutex mtx;
deque<conflux::work::root::detail::small_move_only_function<void()>>local{};
jthread thread{};
};
WorkPoolOptions options_{};
V<UP<Worker>>workers_{};
work_detail::MpmcRing inject_ring_;
Atom<u32>wake_epoch_{0};
// parked_ on a separate cache line: producer loads it after every push;
// isolating prevents wake_epoch_ stores from invalidating this line and
// adding a cache miss to the no-parked-workers fast path.
alignas(64)Atom<int>parked_{0};
Atom<SZ>pending_{0};
atomic_flag accepting_stopped_{};
atomic_flag stopping_{};
mutex admission_mtx_{};

inline static thread_local WorkPool*tls_pool_=nullptr;
inline static thread_local SZ tls_worker_=work_detail::kNoWorker;
[[nodiscard]]bool is_local_worker()const noexcept{
return tls_pool_==this&&tls_worker_!=work_detail::kNoWorker;
}
void wake_one()noexcept{
// P6 candidate (b) — fence-between: release store on wake_epoch_ +
// SC fence forms one half of the SC fence pair with the worker's
// parked_++ + SC fence. Guarantees: if any worker is parked (parked_>0)
// we issue a wake; if none are parked, the futex_wake is elided.
wake_epoch_.fetch_add(1,memory_order_release);
std::atomic_thread_fence(memory_order_seq_cst);
if(parked_.load(memory_order_acquire)>0)
work_detail::futex_wake_private(wake_epoch_,1);
}
void wake_all()noexcept{
// Unconditional: shutdown must guarantee all parked workers exit.
wake_epoch_.fetch_add(1,memory_order_release);
work_detail::futex_wake_private(wake_epoch_,static_cast<int>(workers_.size()));
}
[[nodiscard]]bool push_local(
conflux::work::root::detail::small_move_only_function<void()>job){
auto&worker=*workers_[tls_worker_];
SL const lk{worker.mtx};
if(worker.local.size()>=options_.local_queue_capacity)
return false;
worker.local.push_back(move(job));
pending_.fetch_add(1,memory_order_release);
return true;
}
[[nodiscard]]bool push_inject(
work_detail::Fn job)noexcept{
if(!inject_ring_.try_push(move(job)))
return false;
pending_.fetch_add(1,memory_order_release);
return true;
}
[[nodiscard]]Opt<conflux::work::root::detail::small_move_only_function<void()>>pop_local(
SZ index){
auto&worker=*workers_[index];
SL const lk{worker.mtx};
if(worker.local.empty())
return nullopt;
auto job=move(worker.local.back());
worker.local.pop_back();
return job;
}
[[nodiscard]]Opt<work_detail::Fn>pop_inject()noexcept{
return inject_ring_.try_pop();
}
[[nodiscard]]Opt<conflux::work::root::detail::small_move_only_function<void()>>steal_work(
SZ thief){
for(SZ offset=1;offset<workers_.size();++offset){
SZ const victim_index=(thief+offset)%workers_.size();
auto&victim=*workers_[victim_index];
SL const lk{victim.mtx};
if(victim.local.empty())
continue;
auto job=move(victim.local.front());
victim.local.pop_front();
return job;
}
return nullopt;
}
static void maybe_set_name(
S const&prefix,
SZ index)noexcept{
if(prefix.empty())
return;
auto name=format("{}-{}",prefix,index);
if(name.size()>15)
name.resize(15);
::pthread_setname_np(::pthread_self(),name.c_str());
}
void maybe_pin_worker(
SZ index)noexcept{
if(!options_.pin_workers)
return;
cpu_set_t set;
CPU_ZERO(&set);
unsigned const cpus=max(1U,thread::hardware_concurrency());
CPU_SET(index%cpus,&set);
::pthread_setaffinity_np(::pthread_self(),sizeof(set),&set);
}
void worker_loop(
std::stop_token const&st,
SZ index){
tls_pool_=this;
tls_worker_=index;
maybe_set_name(options_.worker_name_prefix,index);
maybe_pin_worker(index);
while(!st.stop_requested()&&!stopping_.test(memory_order_acquire)){
auto job=pop_local(index);
if(!job)
job=pop_inject();
if(!job)
job=steal_work(index);
if(job){
try{
(*job)();
}catch(...){
if(options_.raw_exception_sink)
try{
options_.raw_exception_sink(current_exception());
}catch(...){}// NOLINT(bugprone-empty-catch)
}
pending_.fetch_sub(1,memory_order_release);
continue;
}
auto const has_pending=[&]{return pending_.load(memory_order_relaxed)>0;};
bool spun=false;
for(u32 s=0;s<options_.spin_before_park&&!spun;++s){
conflux::work::root::detail::cpu_pause();
spun=has_pending();
}
if(!spun){
// P6 candidate (b) park protocol:
// 1. Announce parked before the re-check so a concurrent push
//    that lands after our spin loop sees us parked and wakes us.
parked_.fetch_add(1,memory_order_acq_rel);
// 2. SC fence: parked++ is globally visible before pending_ load,
//    forming the pair with wake_one()'s SC fence.
std::atomic_thread_fence(memory_order_seq_cst);
// 3. Re-check: if a producer pushed between our spin loop and
//    parked++, either pending_ reflects it (→ skip park) or the
//    producer saw parked_>0 and issued a wake (→ futex_wait
//    returns EAGAIN immediately on the stale epoch).
if(pending_.load(memory_order_acquire)>0||stopping_.test(memory_order_acquire)){
parked_.fetch_sub(1,memory_order_acq_rel);
}else{
u32 const epoch=wake_epoch_.load(memory_order_acquire);
work_detail::futex_wait_private(wake_epoch_,epoch);
parked_.fetch_sub(1,memory_order_acq_rel);
}
}
}
tls_pool_=nullptr;
tls_worker_=work_detail::kNoWorker;
}
public:
explicit WorkPool(
WorkPoolOptions options={})
:options_{move(options)},inject_ring_{options_.max_inject_queue}{
if(options_.threads==0)
options_.threads=max(1U,thread::hardware_concurrency());
workers_.reserve(options_.threads);
for(SZ i=0;i<options_.threads;++i)
workers_.push_back(make_unique<Worker>());
for(SZ i=0;i<workers_.size();++i)
workers_[i]->thread=jthread([this,i](std::stop_token const&st){worker_loop(st,i);});
}
~WorkPool()override{
stop();
wait();
}
WorkPool(WorkPool const&)=delete;
WorkPool&operator=(WorkPool const&)=delete;
WorkPool(WorkPool&&)=delete;
WorkPool&operator=(WorkPool&&)=delete;
[[nodiscard]]bool enqueue(
conflux::work::root::detail::small_move_only_function<void()>job)override{
std::unique_lock admission{admission_mtx_};
if(accepting_stopped_.test(memory_order_acquire)||stopping_.test(memory_order_acquire))
return false;
bool const queued=is_local_worker()?push_local(move(job)):push_inject(move(job));
if(!queued)
return false;
admission.unlock();
wake_one();
return true;
}
void stop()noexcept{
SL const admission{admission_mtx_};
accepting_stopped_.test_and_set(memory_order_acq_rel);
if(!stopping_.test_and_set(memory_order_acq_rel)){
for(auto&worker:workers_)
worker->thread.request_stop();
wake_all();
}
}
void drain_and_stop()noexcept{
{
SL const admission{admission_mtx_};
accepting_stopped_.test_and_set(memory_order_acq_rel);
}
while(pending_.load(memory_order_acquire)>0){
wake_all();
std::this_thread::yield();
}
stop();
wait();
}
void wait()noexcept{
for(auto&worker:workers_)
if(worker->thread.joinable())
worker->thread.join();
}
[[nodiscard]]bool stopped()const noexcept{return accepting_stopped_.test(memory_order_acquire);}
};
// io_uring-coupled executor: requires a live ring (ring_fd from RingLaneOptions).
// Each non-inline enqueue into an empty queue issues exactly one
// io_uring_register_sync_msg syscall. Uses raw io_uring_sqe only — no conflux.net
// dependency; callers own the ring and drive drain() from CQE handlers.
export class RingLane final:public work_detail::QueueTarget{
RingLaneOptions options_{};
mutex mtx_{};
deque<conflux::work::root::detail::small_move_only_function<void()>>queue_{};
atomic_flag stopped_{};
atomic_flag wake_pending_{};
thread::id owner_{std::this_thread::get_id()};
[[nodiscard]]bool is_owner_thread()const noexcept{return std::this_thread::get_id()==owner_;}
[[nodiscard]]bool wake_ring()noexcept{
if(options_.ring_fd<0)
return false;
io_uring_sqe sqe{};
io_uring_prep_msg_ring(&sqe,options_.ring_fd,0,options_.wake_user_data,0);
return io_uring_register_sync_msg(&sqe)==0;
}
void run_inline(
conflux::work::root::detail::small_move_only_function<void()>job){
try{
job();
}catch(...){}// NOLINT(bugprone-empty-catch)
}
public:
explicit RingLane(
RingLaneOptions options={})
:options_{move(options)}{}
[[nodiscard]]bool enqueue(
conflux::work::root::detail::small_move_only_function<void()>job)override{
if(stopped_.test(memory_order_acquire))
return false;
if(is_owner_thread()&&options_.allow_inline_on_owner){
run_inline(move(job));
return true;
}
bool need_wake=false;
{
SL const lk{mtx_};
need_wake=queue_.empty();
queue_.push_back(move(job));
if(need_wake&&!wake_pending_.test_and_set(memory_order_acq_rel)){
if(!wake_ring()){
queue_.pop_back();
wake_pending_.clear(memory_order_release);
return false;
}
}
}
return true;
}
void adopt_current_thread()noexcept{owner_=std::this_thread::get_id();}
[[nodiscard]]SZ drain(){
if(!is_owner_thread())
throw conflux::work::root::JoinError{conflux::work::root::JoinError::reason::thread_precondition};
SZ ran=0;
SZ const budget=options_.drain_budget==0?NL<SZ>::max():options_.drain_budget;
while(ran<budget){
conflux::work::root::detail::small_move_only_function<void()>job;
{
SL const lk{mtx_};
if(queue_.empty()){
wake_pending_.clear(memory_order_release);
break;
}
job=move(queue_.front());
queue_.pop_front();
if(queue_.empty())
wake_pending_.clear(memory_order_release);
}
run_inline(move(job));
++ran;
}
if(ran==budget){
SL const lk{mtx_};
if(!queue_.empty()&&!wake_pending_.test_and_set(memory_order_acq_rel))
(void)wake_ring();
}
return ran;
}
void stop()noexcept{stopped_.test_and_set(memory_order_release);}
[[nodiscard]]bool stopped()const noexcept{return stopped_.test(memory_order_acquire);}
[[nodiscard]]bool on_owner_thread()const noexcept{return is_owner_thread();}
[[nodiscard]]int ring_fd()const noexcept{return options_.ring_fd;}
};
namespace conflux::work::root{
template<>
inline constexpr bool enable_address_capability_v<WorkPool> =true;
template<>
inline constexpr bool enable_address_capability_v<RingLane> =true;
}// namespace conflux::work::root
export template<typename Target,typename Fn>
[[nodiscard]]auto run_on_task(
Target&target,
Fn&&fn){
using fn_t=std::decay_t<Fn>;
using T=std::invoke_result_t<fn_t&>;
using namespace conflux::work::root;
auto[task,src]=make_task_source<T>(SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<TaskSource<T>>(move(src));
auto job=[shared_src,fn=fn_t{forward<Fn>(fn)}]()mutable{
try{
if constexpr(std::is_void_v<T>){
fn();
(void)shared_src->try_set_value(Success<T>{});
}else{
(void)shared_src->try_set_value(Success<T>{fn()});
}
}catch(...){(void)shared_src->try_set_exception(current_exception());}
};
if(!target.enqueue(move(job)))
(void)shared_src->try_set_cancelled(work_errc::cancelled_requested);
return move(task);
}
// Synchronous blocking wait for a root::Task<T> — no FileReader required.
// Useful when the task completes on a thread pool (not io_uring).
export template<typename T>
T sync_wait(
conflux::work::root::Task<T>task){
using namespace conflux::work::root;
auto outcome=join(into_join_handle(move(task)));
if(outcome.is_failure())
rethrow_exception(move(outcome).failure().error);
if(outcome.is_cancelled())
throw::Cancelled{};
if constexpr(!std::is_void_v<T>)
return move(outcome).success().value;
}
export namespace conflux::work{
template<typename T>
using Task=root::Task<T>;

template<typename T>
using TaskSource=root::TaskSource<T>;

using TaskControl=root::TaskControl;

template<typename T>
using Outcome=root::Outcome<T>;

using CancelReason=root::CancelReason;

using root::join;// NOLINT(misc-unused-using-decls) — re-export for module consumers
using root::make_task_source;// NOLINT(misc-unused-using-decls)
}// namespace conflux::work
// P9 join_all: single-allocation implementation.
// Two allocs total: make_task_source (output control block) +
// make_shared<JoinState> (slots, handles, and join state in one block).
// Each input handle stored in JoinState — no per-task shared_ptr.
// The ready callback for slot I fires after the control block lock is dropped,
// calls join() (non-blocking at that point) to extract the rvalue outcome,
// then decrements remaining. When remaining reaches 0, commits the result.
// Cancel-cascade: first cancellation calls request_cancel() on all sibling
// controls; shared_from_this keepalive prevents JoinState destruction during
// the cascade; all N slots must complete before commit.
namespace join_all_detail{
template<class T>
using JoinResultT=std::conditional_t<std::is_void_v<T>,std::monostate,T>;
template<typename...Ts>
struct JoinState:std::enable_shared_from_this<JoinState<Ts...>>{
using Result=std::tuple<JoinResultT<Ts>...>;
using Slots=std::tuple<std::optional<JoinResultT<Ts>>...>;

Atom<SZ>remaining{sizeof...(Ts)};
mutex mtx;
EP first_error;
bool any_cancelled=false;
Slots slots;
Tup<conflux::work::root::TaskJoinHandle<Ts>...>handles;
conflux::work::root::TaskSource<Result>src;
JoinState(
conflux::work::root::TaskSource<Result>s,
conflux::work::root::TaskJoinHandle<Ts>...hs)
:handles{move(hs)...},src{move(s)}{}
void cancel_all()noexcept{
auto keepalive=this->shared_from_this();
std::apply([](auto&...hs)noexcept{((void)hs.control().request_cancel(),...);},handles);
}
void commit()noexcept{
using namespace conflux::work::root;
if(any_cancelled){
(void)src.try_set_cancelled(work_errc::cancelled_requested);
return;
}
if(first_error){
(void)src.try_set_exception(move(first_error));
return;
}
try{
auto result=std::apply([](auto&...opts){return Result{move(*opts)...};},slots);
(void)src.try_set_value(conflux::work::root::Success<Result>{move(result)});
}catch(...){(void)src.try_set_exception(current_exception());}
}
template<SZ I>
void on_ready()noexcept{
using namespace conflux::work::root;
using T=std::tuple_element_t<I,std::tuple<Ts...>>;
auto outcome=join(move(std::get<I>(handles)));
bool should_cancel=false;
if(outcome.is_success()){
if constexpr(std::is_void_v<T>)
std::get<I>(slots).emplace();
else
std::get<I>(slots)=move(outcome).success().value;
}else if(outcome.is_failure()){
SL lk{mtx};
if(!first_error)
first_error=move(outcome).failure().error;
}else{
SL lk{mtx};
if(!any_cancelled){
any_cancelled=true;
should_cancel=true;
}
}
if(should_cancel)
cancel_all();
if(remaining.fetch_sub(1,memory_order_acq_rel)==1)
commit();
}
};
}// namespace join_all_detail
export template<typename...Ts>
[[nodiscard]]auto join_all(
conflux::work::root::Task<Ts>...tasks)->conflux::work::root::Task<Tup<std::conditional_t<std::is_void_v<Ts>,std::monostate,Ts>...>>{
using namespace conflux::work::root;
using Result=std::tuple<std::conditional_t<std::is_void_v<Ts>,std::monostate,Ts>...>;

if constexpr(sizeof...(Ts)==0){
auto[t,s]=make_task_source<Result>(SubmitOptions{.enable_cancellation=false});
(void)s.try_set_value(Success<Result>{Result{}});
return move(t);
}

auto[root_task,src]=make_task_source<Result>(SubmitOptions{.enable_cancellation=false});
auto state=
make_shared<join_all_detail::JoinState<Ts...>>(move(src),into_join_handle(move(tasks))...);

[&state]<SZ...Is>(std::index_sequence<Is...>){
auto attach=[&state]<SZ I>()noexcept{
std::get<I>(state->handles).control().set_on_ready_or_run([s=state]()noexcept{
s->template on_ready<I>();
});
};
(attach.template operator()<Is>(),...);
}(std::index_sequence_for<Ts...>{});

return move(root_task);
}
