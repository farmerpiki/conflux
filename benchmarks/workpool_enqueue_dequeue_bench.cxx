// workpool_enqueue_dequeue_bench — measures WorkPool enqueue/dequeue throughput.
//
// Config JSON: { "threads": N }  for N in {1, 4, 16, nproc}
// Variants:
//   single_thread — single-producer, single-worker, no cross-thread contention
//   contended     — N producer threads, WorkPool workers; N from --threads
//
// CSV output (--json): config,variant,iterations,total_ns,ns_per_iter

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;

import bench_common;

using namespace std::string_view_literals;
namespace root=conflux::work::root;
namespace{
BenchStats bench_single_thread(
SV cfg_name,
SZ iters,
SZ warmup){
WorkPool pool{WorkPoolOptions{.threads=1}};
auto do_iters=[&](SZ n){
for(SZ i=0;i<n;++i){
auto[task,source]=root::make_task_source<int>();
pool.enqueue([s=move(source)]()mutable{(void)s.try_set_value(root::Success<int>{0});});
[[maybe_unused]]auto outcome=root::join(move(task));
}
};
do_iters(warmup);
u64 const t0=bench_now_ns();
do_iters(iters);
u64 const elapsed=bench_now_ns()-t0;
double const ns_pi=static_cast<double>(elapsed)/static_cast<double>(iters);
return{cfg_name,"single_thread"sv,iters,elapsed,ns_pi,1e9/ns_pi};
}
BenchStats bench_contended(
SV cfg_name,
SZ threads,
SZ iters,
SZ warmup){
SZ const worker_count=max(SZ{1},threads);
WorkPool pool{WorkPoolOptions{.threads=worker_count}};
SZ const per_thread=iters/threads;
auto do_wave=[&](SZ n_per){
V<thread>producers;
producers.reserve(threads);
for(SZ t=0;t<threads;++t)
producers.emplace_back([&pool,n_per]{
for(SZ i=0;i<n_per;++i){
auto[task,source]=root::make_task_source<int>();
pool.enqueue([s=move(source)]()mutable{(void)s.try_set_value(root::Success<int>{0});});
(void)root::join(move(task));
}
});
for(auto&th:producers)
th.join();
};
do_wave(warmup/threads+1);
u64 const t0=bench_now_ns();
do_wave(per_thread);
u64 const elapsed=bench_now_ns()-t0;
SZ const total_iters=per_thread*threads;
double const ns_pi=static_cast<double>(elapsed)/static_cast<double>(total_iters);
return{cfg_name,"contended"sv,total_iters,elapsed,ns_pi,1e9/ns_pi};
}
}// namespace
int main(
int argc,
char**argv){
if(argc>=2&&SV{argv[1]}=="--bench-info"){
auto const hw=thread::hardware_concurrency();
V<unsigned>ts={1u,4u,16u};
if(hw!=1u&&hw!=4u&&hw!=16u)
ts.push_back(hw);
std::sort(ts.begin(),ts.end());
S cfgs;
for(SZ i=0;i<ts.size();++i){
if(i>0)
cfgs+=',';
cfgs+=format(
"{{\"name\":\"threads_{0}\",\"extra\":{{\"threads\":{0}}},\"args\":[\"--threads\",\"{0}\"," "\"--config-name\",\"threads_{0}\",\"--iterations\",\"5000\",\"--warmup\",\"500\"]}}",
ts[i]);
}
println("{{\"name\":\"workpool_enqueue_dequeue\",\"parser\":\"strip1\",\"configs\":[{}]}}",cfgs);
return 0;
}

auto cfg=bench_parse_args(span{argv,static_cast<SZ>(argc)});
SZ threads=1;
for(SZ i=1;i<static_cast<SZ>(argc);++i){
SV a=argv[i];
if(a=="--threads"&&i+1<static_cast<SZ>(argc)){
threads=bench_parse_sz(argv[++i]);
if(cfg.config_name.empty())
cfg.config_name=format("threads_{}",threads);
}
}

BenchStats stats[]={
bench_single_thread(cfg.config_name,cfg.iterations,cfg.warmup),
bench_contended(cfg.config_name,threads,cfg.iterations,cfg.warmup),
};
for(SZ i=0;i<std::size(stats);++i)
bench_print(stats[i],cfg.json_out,i==0);
}
