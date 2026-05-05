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

JsonParseOptions opts{.max_depth=LimitOption::bound(256)};
JsonReader reader{input,opts};

for(;;){
auto ev_or=reader.next();
if(!ev_or){
if(ev_or.error().message.empty())
__builtin_trap();
break;
}
if(!*ev_or)
break;
using Ev=JsonReader::Event;
Ev ev=**ev_or;
switch(ev){
case Ev::key:
case Ev::string_value:{
S decoded;
auto tok=(ev==Ev::key)?reader.key_token():reader.string_token();
(void)tok.append_decoded_to(decoded);
break;
}
case Ev::number_value:
(void)reader.number_val().to_i64();
(void)reader.number_val().to_f64();
break;
case Ev::bool_value:
(void)reader.bool_val();
break;
default:
break;
}
}
return 0;
}
