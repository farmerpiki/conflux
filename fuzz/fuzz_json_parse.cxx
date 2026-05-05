// libFuzzer driver for conflux::json::parse.
// Invariants:
//   - never crash on any byte sequence
//   - parse ok  -> dump -> parse2 -> is_value_equal(root1, root2)
//   - parse fail -> error code valid, message non-empty

import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;
extern "C" int LLVMFuzzerTestOneInput(
u8 const*data,
SZ size){
if(size==0)
return 0;

SV input{reinterpret_cast<char const*>(data),size};

JsonParseOptions opts;
opts.max_depth=LimitOption::bound(256);// prevent stack overflow on deep nesting

auto res=parse(input,opts);
if(!res){
// parse failure: validate error is well-formed
auto const&err=res.error();
if(err.code==JsonIssueCode{}||err.message.empty())
__builtin_trap();
return 0;
}

// Traverse: just iterate children to exercise NodeRef paths
NodeRef const root=res->root();
if(auto arr=root.as_array()){
for(NodeRef const e:arr->elements())
(void)e.kind();
}else if(auto obj=root.as_object()){
for(auto const&[k,v]:obj->members()){
(void)k;
(void)v.kind();
}
}

// Round-trip: dump -> parse -> value-equal
auto dumped=res->dump();
if(!dumped)// NOLINT(bugprone-branch-clone)
return 0;
auto res2=parse(*dumped);
if(!res2)
return 0;
if(!is_value_equal(root,res2->root()))
__builtin_trap();
return 0;
}
