// task_chain_composition_bench — measures N-step Chain<T> .then/.map pipeline.
//
// Config JSON: { "chain_steps": N }  for N in {1, 4, 16, 64}
// Variants:
//   chain_into_task  — N-step chain ending with into_ready_task
//
// CSV output (--json): config,variant,iterations,total_ns,ns_per_iter

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;

import bench_common;

using namespace std::string_view_literals;
namespace root=conflux::work::root;
namespace carrier=conflux::work::carrier;
namespace{
carrier::Chain<int>apply_steps(
carrier::Chain<int>chain,
SZ n){
for(SZ i=0;i<n;++i)
chain=carrier::map(move(chain),[](int v){return v+1;});
return chain;
}
void run_once(
SZ steps){
auto[task,source]=root::make_task_source<int>();
(void)source.try_set_value(root::Success<int>{0});
auto chain=carrier::from_task(move(task));
chain=apply_steps(move(chain),steps);
auto result_task=carrier::into_ready_task(move(chain));
[[maybe_unused]]auto outcome=root::join(move(result_task));
}
}// namespace
int main(
int argc,
char**argv){
bench_info_if_requested(argc,argv,
R"({"name":"task_chain_composition","parser":"standard","configs":[{"name":"steps_1","extra":{"chain_steps":1},"args":["--steps","1","--config-name","steps_1","--iterations","500000","--warmup","20000"]},{"name":"steps_4","extra":{"chain_steps":4},"args":["--steps","4","--config-name","steps_4","--iterations","500000","--warmup","20000"]},{"name":"steps_16","extra":{"chain_steps":16},"args":["--steps","16","--config-name","steps_16","--iterations","500000","--warmup","20000"]},{"name":"steps_64","extra":{"chain_steps":64},"args":["--steps","64","--config-name","steps_64","--iterations","500000","--warmup","20000"]}]})");

auto cfg=bench_parse_args(span{argv,static_cast<SZ>(argc)});
SZ chain_steps=4;
for(SZ i=1;i<static_cast<SZ>(argc);++i){
SV a=argv[i];
if(a=="--steps"&&i+1<static_cast<SZ>(argc)){
chain_steps=bench_parse_sz(argv[++i]);
if(cfg.config_name.empty())
cfg.config_name=format("steps_{}",chain_steps);
}
}

for(SZ i=0;i<cfg.warmup;++i)
run_once(chain_steps);

u64 const t0=bench_now_ns();
for(SZ i=0;i<cfg.iterations;++i)
run_once(chain_steps);
u64 const elapsed=bench_now_ns()-t0;

BenchStats s{cfg.config_name,"chain_into_task"sv,cfg.iterations,elapsed,
static_cast<double>(elapsed)/static_cast<double>(cfg.iterations)};
bench_print(s,cfg.json_out,true);
}
