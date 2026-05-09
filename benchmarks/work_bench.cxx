#include<liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.work.carrier.coro;

using namespace std::string_view_literals;

namespace ec=conflux::work::carrier;

namespace root=conflux::work::root;
struct OwnerCap{};
struct DriverCap{};
namespace conflux::work::root{
template<>inline constexpr bool enable_address_capability_v<OwnerCap> =true;
template<>inline constexpr bool enable_address_capability_v<DriverCap> =true;
}
namespace{
inline Atom<SZ>sink{};
struct Config{
bool list_only=false;
S filter;
Opt<SZ>iterations_override;
enum class Format:u8{
table,
json
};
Format format=Format::table;
};
struct Stats{
SV name;
SZ iterations{};
u64 total_ns{};
double ns_per_iter{};
double min_ns{};
double p10_ns{};
double mad_ns{};
};
using BenchFn=root::detail::small_move_only_function<SZ()>;
struct Case{
SV name;
SV description;
SZ default_iterations;
SZ reps=10;
BenchFn run;
};
void print_usage(){
println("Usage: conflux_work_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|json]");
}
Config parse_args(
span<char*>args){
Config cfg;
for(SZ i=1;i<args.size();++i){
SV arg=args[i];
if(arg=="--list"){
cfg.list_only=true;
continue;
}
if(arg=="--help"||arg=="-h"){
print_usage();
std::exit(0);
}
if(arg=="--filter"){
if(i+1>=args.size())
throw std::invalid_argument{"--filter requires a value"};
cfg.filter=args[++i];
continue;
}
if(arg=="--iterations"){
if(i+1>=args.size())
throw std::invalid_argument{"--iterations requires a value"};
SZ iters=0;
auto const value=SV{args[++i]};
auto const[ptr,ec]=from_chars(value.data(),value.data()+value.size(),iters);
if(ec!=errc{}||ptr!=value.data()+value.size()||iters==0)
throw std::invalid_argument{"--iterations must be a positive integer"};
cfg.iterations_override=iters;
continue;
}
if(arg=="--format"){
if(i+1>=args.size())
throw std::invalid_argument{"--format requires a value"};
auto const value=SV{args[++i]};
if(value=="table")
cfg.format=Config::Format::table;
else if(value=="json")
cfg.format=Config::Format::json;
else
throw std::invalid_argument{"--format must be table or json"};
continue;
}
if(arg=="--json"){
cfg.format=Config::Format::json;
continue;
}
throw std::invalid_argument{format("unknown argument: {}",arg)};
}
return cfg;
}
bool matches_filter(
Case const&bench,
SV filter){
return filter.empty()||bench.name.contains(filter)||bench.description.contains(filter);
}
SZ warmup_iterations(
SZ iterations){
return std::clamp(iterations/10,SZ{1},SZ{1000});
}
Stats measure_case(
Case const&bench,
SZ iterations){
for(SZ i=0;i<warmup_iterations(iterations);++i)
sink.fetch_add(bench.run(),memory_order_relaxed);

V<double>times;
times.reserve(bench.reps);
for(SZ r=0;r<bench.reps;++r){
auto const t0=chrono::steady_clock::now();
for(SZ i=0;i<iterations;++i)
sink.fetch_add(bench.run(),memory_order_relaxed);
auto const dt=chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now()-t0).count();
times.push_back(static_cast<double>(dt)/static_cast<double>(iterations));
}

ranges::sort(times);
SZ const n=times.size();

auto const min_ns=times[0];
auto const p10_ns=times[static_cast<SZ>(0.1*static_cast<double>(n-1))];

double const med_ns=(n%2==0)?
(times[n/2-1]+times[n/2])/2.0:
times[n/2];

V<double>devs;
devs.reserve(n);
for(auto t:times)
devs.push_back(std::abs(t-med_ns));
ranges::sort(devs);
double const mad_ns=(n%2==0)?
(devs[n/2-1]+devs[n/2])/2.0:
devs[n/2];

auto const total_ns=static_cast<u64>(med_ns*static_cast<double>(iterations));
return Stats{
.name=bench.name,
.iterations=iterations,
.total_ns=total_ns,
.ns_per_iter=med_ns,
.min_ns=min_ns,
.p10_ns=p10_ns,
.mad_ns=mad_ns,
};
}
void print_header(
Config::Format format){
if(format==Config::Format::table)
println("{:48} {:>12} {:>10} {:>10} {:>10} {:>10}","Benchmark","Iterations","med ns","min ns","p10 ns","mad ns");
}
void print_stats(
Stats const&stats,
Config::Format format){
if(format==Config::Format::table)
println("{:48} {:>12} {:>10.1f} {:>10.1f} {:>10.1f} {:>10.1f}",
stats.name,stats.iterations,stats.ns_per_iter,stats.min_ns,stats.p10_ns,stats.mad_ns);
else
println("{{\"config\":\"\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"min\":{:.2f},\"p10\":{:.2f},\"mad\":{:.2f}}}",
stats.name,stats.iterations,stats.total_ns,stats.ns_per_iter,stats.min_ns,stats.p10_ns,stats.mad_ns);
}
// ---------------------------------------------------------------------------
// root: lifecycle — value paths
// ---------------------------------------------------------------------------

Case make_root_task_value_case(){
return Case{
.name="root/task_value",
.description="make_task_source + try_set_value + root::value(task)",
.default_iterations=200000,
.run=[]{
auto[task,src]=root::make_task_source<int>();
(void)src.try_set_value(root::Success<int>{42});
return static_cast<SZ>(root::value(move(task)));
}};
}
Case make_root_posted_value_case(){
return Case{
.name="root/posted_value",
.description="make_posted_source + try_set_value + root::value(owner, posted)",
.default_iterations=200000,
.run=[]{
OwnerCap owner{};
auto[posted,src]=root::make_posted_source<int>(owner);
(void)src.try_set_value(root::Success<int>{42});
return static_cast<SZ>(root::value(owner,move(posted)));
}};
}
Case make_root_operation_value_case(){
return Case{
.name="root/operation_value",
.description="make_operation_source + try_set_value + root::value(driver, op)",
.default_iterations=200000,
.run=[]{
DriverCap driver{};
auto[op,src]=root::make_operation_source<int>(driver);
(void)src.try_set_value(root::Success<int>{42});
return static_cast<SZ>(root::value(driver,move(op)));
}};
}
Case make_root_task_cancelled_case(){
return Case{
.name="root/task_cancelled",
.description="make_task_source + try_set_cancelled + root::join (outcome check)",
.default_iterations=200000,
.run=[]{
auto[task,src]=root::make_task_source<int>();
(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
auto out=root::join(move(task));
return static_cast<SZ>(out.is_cancelled()?1:0);
}};
}
// ---------------------------------------------------------------------------
// root: lifecycle — admit/abandon paths
// ---------------------------------------------------------------------------

Case make_root_task_admit_case(
bool enable_cancellation){
auto const name=enable_cancellation?"root/task_admit_cancel_on"sv:"root/task_admit_cancel_off"sv;
auto const desc=enable_cancellation?"make_task_source(cancel=on) + abandon_to(drop)"sv:
"make_task_source(cancel=off) + abandon_to(drop)"sv;
return Case{
.name=name,
.description=desc,
.default_iterations=200000,
.run=[enable_cancellation]{
auto[task,src]=
root::make_task_source<int>(root::SubmitOptions{.enable_cancellation=enable_cancellation});
root::abandon_to(move(task),root::drop_on_abandon{});
(void)src;
return SZ{1};
}};
}
Case make_root_abandon_drop_case(){
return Case{
.name="root/abandon_drop",
.description="make_task_source + try_set_value + abandon_to(drop)",
.default_iterations=200000,
.run=[]{
auto[task,src]=root::make_task_source<int>();
root::abandon_to(move(task),root::drop_on_abandon{});
(void)src.try_set_value(root::Success<int>{1});
return SZ{1};
}};
}
Case make_root_abandon_sink_case(){
struct Sink{
SZ*seen{};
void operator()(
root::Failure const&)const noexcept{}
void operator()(
root::Cancelled const&)const noexcept{
*seen=1;
}
};
return Case{
.name="root/abandon_sink",
.description="make_task_source + abandon_to(custom sink) + try_set_cancelled → sink dispatched",
.default_iterations=200000,
.run=[]{
auto[task,src]=root::make_task_source<int>();
SZ seen=0;
root::abandon_to(move(task),Sink{.seen=&seen});
(void)src.try_set_cancelled(root::work_errc::cancelled_requested);
return seen;
}};
}
// ---------------------------------------------------------------------------
// root: cancellation hooks
// ---------------------------------------------------------------------------

Case make_root_cancel_hook_case(
bool enable_cancellation){
auto const name=enable_cancellation?"root/cancel_hook_enabled"sv:"root/cancel_hook_disabled"sv;
auto const desc=enable_cancellation?"install_cancel_hook + request_cancel (live stop-token)"sv:
"install_cancel_hook + request_cancel (inert stop-token)"sv;
return Case{
.name=name,
.description=desc,
.default_iterations=200000,
.run=[enable_cancellation]{
auto[task,src]=
root::make_task_source<int>(root::SubmitOptions{.enable_cancellation=enable_cancellation});
SZ seen=0;
(void)src.install_cancel_hook([&seen](root::CancelReason reason)noexcept{
if(reason==root::CancelReason::requested)
++seen;
});
auto control=task.control();
SZ score=control.request_cancel()?1U:0U;
score+=seen;
root::abandon_to(move(task),root::drop_on_abandon{});
return score;
}};
}
// ---------------------------------------------------------------------------
// root: callable erasure — small_move_only_function
// ---------------------------------------------------------------------------

Case make_small_fn_inline_case(){
using Fn=root::detail::small_move_only_function<void(root::CancelReason)>;
return Case{
.name="root/small_fn_inline",
.description="small_move_only_function: construct + move + invoke (inline fit)",
.default_iterations=500000,
.run=[]{
SZ seen=0;
Fn fn{[&seen](root::CancelReason r)noexcept{
if(r==root::CancelReason::requested)
++seen;
}};
Fn moved{move(fn)};
moved(root::CancelReason::requested);
return seen;
}};
}
Case make_small_fn_heap_case(){
using Fn=root::detail::small_move_only_function<void(root::CancelReason)>;
struct BigCapture{
A<uintptr_t,5>words{};
};
return Case{
.name="root/small_fn_heap",
.description="small_move_only_function: construct + move + invoke (heap alloc, >32B capture)",
.default_iterations=500000,
.run=[]{
SZ seen=0;
BigCapture cap{};
cap.words[0]=0xC0FFEEU;
Fn fn{[&seen,cap](root::CancelReason r)noexcept{
if(r==root::CancelReason::requested)
seen+=(cap.words[0]&1U)+1U;
}};
Fn moved{move(fn)};
moved(root::CancelReason::requested);
return seen;
}};
}
// ---------------------------------------------------------------------------
// work: WorkPool dispatch
// ---------------------------------------------------------------------------

WorkPoolOptions bench_pool_opts(){
WorkPoolOptions opts;
#ifdef CONFLUX_BENCH_SPIN_BEFORE_PARK
opts.spin_before_park=CONFLUX_BENCH_SPIN_BEFORE_PARK;
#endif
opts.threads=4;
return opts;
}
Case make_pool_single_case(){
auto pool=make_shared<WorkPool>(bench_pool_opts());
return Case{
.name="work/pool_single",
.description="run_on_task(pool, fn) + root::value(task) — single dispatch roundtrip",
.default_iterations=25000,
.run=[pool]{
return static_cast<SZ>(root::value(run_on_task(*pool,[]{return 42;})));
}};
}
Case make_pool_join_all_3_case(){
auto pool=make_shared<WorkPool>(bench_pool_opts());
return Case{
.name="work/pool_join_all_3",
.description="join_all(3 × run_on_task) + root::value — 3-way fan-out",
.default_iterations=30000,
.run=[pool]{
auto[a,b,c]=root::value(join_all(
run_on_task(*pool,[]{return 1;}),
run_on_task(*pool,[]{return 2;}),
run_on_task(*pool,[]{return 3;})));
return static_cast<SZ>(a+b+c);
}};
}
Case make_pool_bursty_case(){
auto pool=make_shared<WorkPool>(bench_pool_opts());
return Case{
.name="work/pool_bursty_8",
.description="burst of 8 tasks after idle gap — exercises park/wake path",
.default_iterations=10000,
.run=[pool]{
// Brief idle to let workers park
for(volatile int i=0;i<200;++i){}
// Burst 8 tasks
auto t0=run_on_task(*pool,[]{return 1;});
auto t1=run_on_task(*pool,[]{return 2;});
auto t2=run_on_task(*pool,[]{return 3;});
auto t3=run_on_task(*pool,[]{return 4;});
auto t4=run_on_task(*pool,[]{return 5;});
auto t5=run_on_task(*pool,[]{return 6;});
auto t6=run_on_task(*pool,[]{return 7;});
auto t7=run_on_task(*pool,[]{return 8;});
auto[a,b,c,d,e,f,g,h]=root::value(join_all(
move(t0),move(t1),move(t2),move(t3),
move(t4),move(t5),move(t6),move(t7)));
return static_cast<SZ>(a+b+c+d+e+f+g+h);
}};
}
// ---------------------------------------------------------------------------
// work: EagerChain coroutine microbench
// ---------------------------------------------------------------------------

ec::EagerChain<int>ec_l1(){co_return 1;}
ec::EagerChain<int>ec_l2(){co_return 1+co_await ec_l1();}
ec::EagerChain<int>ec_l3(){co_return 1+co_await ec_l2();}
ec::EagerChain<int>ec_l4(){co_return 1+co_await ec_l3();}
Case make_eager_chain_flat_int_case(){
return Case{
.name="eager_chain/flat_int",
.description="EagerChain<int>: allocate frame + co_return int (single frame)",
.default_iterations=2000000,
.run=[]{
auto c=[]()->ec::EagerChain<int>{co_return 42;}();
auto out=move(c).chain().release_outcome();
return static_cast<SZ>(move(out).success().value);
}};
}
Case make_eager_chain_flat_void_case(){
return Case{
.name="eager_chain/flat_void",
.description="EagerChain<void>: allocate frame + co_return void (single frame)",
.default_iterations=2000000,
.run=[]{
auto c=[]()->ec::EagerChain<void>{co_return;}();
(void)move(c).chain().release_outcome();
return SZ{1};
}};
}
Case make_eager_chain_nested_4_case(){
return Case{
.name="eager_chain/nested_4",
.description="EagerChain<int> 4-deep: LIFO frame stack, all synchronous",
.default_iterations=500000,
.run=[]{
auto out=ec_l4().chain().release_outcome();
return static_cast<SZ>(move(out).success().value);
}};
}
// ---------------------------------------------------------------------------
// work: RingLane
// ---------------------------------------------------------------------------

Case make_ring_lane_case(){
struct State{
io_uring ring{};
UP<RingLane>lane;
State(){
int const rc=::io_uring_queue_init(8,&ring,0);
if(rc!=0)
throw RE{format("io_uring_queue_init failed: {}",rc)};
lane=make_unique<RingLane>(RingLaneOptions{
.ring_fd=ring.ring_fd,
.wake_user_data=0x57524B42U,
.drain_budget=0,
.allow_inline_on_owner=false,
});
lane->adopt_current_thread();
}
~State(){::io_uring_queue_exit(&ring);}
};
auto state=make_shared<State>();
return Case{
.name="work/ring_lane_roundtrip",
.description="RingLane enqueue from jthread + msg-ring wake + owner drain",
.default_iterations=5000,
.reps=1,
.run=[state]{
Atom<SZ>out{};
jthread producer([&]{
if(!state->lane->enqueue([&out]{out.store(77,memory_order_release);}))
throw RE{"ring lane enqueue failed"};
});
producer.join();
io_uring_cqe*cqe=nullptr;
if(::io_uring_wait_cqe(&state->ring,&cqe)!=0||cqe==nullptr)
throw RE{"io_uring_wait_cqe failed"};
::io_uring_cqe_seen(&state->ring,cqe);
(void)state->lane->drain();
return out.load(memory_order_acquire);
}};
}
V<Case>make_cases(){
V<Case>cases;
// root: lifecycle — value paths (sync join)
cases.push_back(make_root_task_value_case());
cases.push_back(make_root_posted_value_case());
cases.push_back(make_root_operation_value_case());
cases.push_back(make_root_task_cancelled_case());
// root: lifecycle — admit/abandon
cases.push_back(make_root_task_admit_case(false));
cases.push_back(make_root_task_admit_case(true));
cases.push_back(make_root_abandon_drop_case());
cases.push_back(make_root_abandon_sink_case());
// root: cancellation hooks
cases.push_back(make_root_cancel_hook_case(true));
cases.push_back(make_root_cancel_hook_case(false));
// root: callable erasure
cases.push_back(make_small_fn_inline_case());
cases.push_back(make_small_fn_heap_case());
// work: pool dispatch (sync join)
cases.push_back(make_pool_single_case());
cases.push_back(make_pool_join_all_3_case());
cases.push_back(make_pool_bursty_case());
// work: ring lane
try{
cases.push_back(make_ring_lane_case());
}catch(exception const&){}
// work: EagerChain microbench
cases.push_back(make_eager_chain_flat_int_case());
cases.push_back(make_eager_chain_flat_void_case());
cases.push_back(make_eager_chain_nested_4_case());
return cases;
}
}// namespace
int main(
int argc,
char**argv){
if(argc>=2&&SV{argv[1]}=="--bench-info"){
std::print(
"{}\n",
R"({"name":"work","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
return 0;
}
try{
auto const cfg=parse_args({argv,static_cast<SZ>(argc)});
auto cases=make_cases();
if(cfg.list_only){
for(auto const&bench:cases)
println("{:48} {}",bench.name,bench.description);
return 0;
}
print_header(cfg.format);
for(auto const&bench:cases){
if(!matches_filter(bench,cfg.filter))
continue;
auto const iterations=cfg.iterations_override.value_or(bench.default_iterations);
print_stats(measure_case(bench,iterations),cfg.format);
}
println(cerr,"sink={}",sink.load(memory_order_relaxed));
return 0;
}catch(exception const&ex){
println(cerr,"conflux_work_benchmarks: {}",ex.what());
return 1;
}
}
