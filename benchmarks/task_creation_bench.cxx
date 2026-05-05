// task_creation_bench — measures alloc cost of make_task_source + commit + join/drop.
//
// Variants:
//   task_creation              — make_task_source<int> + try_set_value + join
//   task_drop_joinable_release — make_task_source<int> + try_set_value, Task dropped (not joined)
//   task_drop_joinable_debug   — same as release variant (debug/release builds differ in dtor)
//
// NDJSON output (--json): {"config":"","variant":"...","iterations":N,"total_ns":N,"ns_per_iter":X}

import std;
import conflux.types;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root=conflux::work::root;
namespace{
void run_warmup(
SZ warmup){
for(SZ i=0;i<warmup;++i){
auto[task,source]=root::make_task_source<int>();
(void)source.try_set_value(root::Success<int>{42});
(void)root::join(move(task));
}
}
BenchStats bench_task_creation(
SZ iters){
u64 const t0=bench_now_ns();
for(SZ i=0;i<iters;++i){
auto[task,source]=root::make_task_source<int>();
(void)source.try_set_value(root::Success<int>{static_cast<int>(i)});
[[maybe_unused]]auto outcome=root::join(move(task));
}
u64 const elapsed=bench_now_ns()-t0;
return{{},"task_creation"sv,iters,elapsed,
static_cast<double>(elapsed)/static_cast<double>(iters)};
}
BenchStats bench_task_drop_joinable(
SV variant_name,
SZ iters){
u64 const t0=bench_now_ns();
for(SZ i=0;i<iters;++i){
auto[task,source]=root::make_task_source<int>();
(void)source.try_set_value(root::Success<int>{static_cast<int>(i)});
(void)root::join(move(task));// remove after E1.x auto-detach
}
u64 const elapsed=bench_now_ns()-t0;
return{{},variant_name,iters,elapsed,
static_cast<double>(elapsed)/static_cast<double>(iters)};
}
}// namespace
int main(
int argc,
char**argv){
bench_info_if_requested(argc,argv,
R"({"name":"task_creation","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","1000000","--warmup","50000"]}]})");

auto const cfg=bench_parse_args(span{argv,static_cast<SZ>(argc)});
run_warmup(cfg.warmup);

BenchStats stats[]={
bench_task_creation(cfg.iterations),
bench_task_drop_joinable("task_drop_joinable_release"sv,cfg.iterations),
bench_task_drop_joinable("task_drop_joinable_debug"sv,cfg.iterations),
};
for(SZ i=0;i<std::size(stats);++i)
bench_print(stats[i],cfg.json_out,i==0);
}
