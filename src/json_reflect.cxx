module;
export module conflux.json.reflect;
import std;
import conflux.types;
import conflux.json;
// ---------------------------------------------------------------------------
// Exported: annotation types for reflected structs
// ---------------------------------------------------------------------------

export namespace conflux::json{
struct name_t{
char const*p;
SZ n;
};
consteval name_t name(SV sv){
return{std::define_static_string(sv),sv.size()};
}
struct skip{};
}// namespace conflux::json
// ---------------------------------------------------------------------------
// Reflection helpers (consteval — require -freflection)
// ---------------------------------------------------------------------------

namespace detail{
template<std::meta::info Mem>
consteval bool reflect_has_skip(){
return!std::meta::annotations_of_with_type(Mem,^^conflux::json::skip).empty();
}
template<std::meta::info Mem>
consteval bool reflect_has_name(){
return!std::meta::annotations_of_with_type(Mem,^^conflux::json::name_t).empty();
}
template<std::meta::info Mem>
consteval conflux::json::name_t reflect_get_name_ann(){
return std::meta::extract<conflux::json::name_t>(
std::meta::annotations_of_with_type(Mem,^^conflux::json::name_t)[0]);
}
template<class T>
consteval SZ reflect_member_count(){
return std::meta::nonstatic_data_members_of(
^^T,std::meta::access_context::unchecked())
.size();
}
template<class T,SZ I>
consteval std::meta::info reflect_member_at(){
return std::meta::nonstatic_data_members_of(
^^T,std::meta::access_context::unchecked())[I];
}
template<std::meta::info Mem>
consteval conflux::json::name_t reflect_field_name(){
if constexpr(reflect_has_name<Mem>()){
return reflect_get_name_ann<Mem>();
}else{
auto sv=std::meta::identifier_of(Mem);
return{std::define_static_string(sv),sv.size()};
}
}
template<class T>struct is_opt_refl:std::false_type{};
template<class T>struct is_opt_refl<Opt<T>>:std::true_type{};
// Decode a NodeRef into M, handling non-codec integral/float types.
template<class M>
[[nodiscard]]expected<M,JsonError>decode_reflect_member(NodeRef node){
if constexpr(requires{JsonCodec<M>::decode(node);}){
return JsonCodec<M>::decode(node);
}else if constexpr(std::is_signed_v<M>&&std::integral<M>){
auto r=JsonCodec<i64>::decode(node);
if(!r)return unexpected(move(r).error());
if(*r<static_cast<i64>(NL<M>::min())||*r>static_cast<i64>(NL<M>::max()))
return unexpected(JsonError{
.stage=JsonStage::decode,
.code=JsonIssueCode::number_out_of_range,
.message=format("value out of i64 range for {}",std::meta::display_string_of(^^M))});
return static_cast<M>(*r);
}else if constexpr(std::is_unsigned_v<M>&&std::integral<M>){
auto r=JsonCodec<u64>::decode(node);
if(!r)return unexpected(move(r).error());
if(*r>static_cast<u64>(NL<M>::max()))
return unexpected(JsonError{
.stage=JsonStage::decode,
.code=JsonIssueCode::number_out_of_range,
.message=format("value out of u64 range for {}",std::meta::display_string_of(^^M))});
return static_cast<M>(*r);
}else if constexpr(std::floating_point<M>){
auto r=JsonCodec<double>::decode(node);
if(!r)return unexpected(move(r).error());
return static_cast<M>(*r);
}else{
static_assert(false,"no decode support for reflected member type");
}
}
// Encode M into ObjectBuilder, handling non-codec integral/float types.
template<class M>
[[nodiscard]]expected<void,JsonError>encode_reflect_member(
ObjectBuilder&obj,SV name,M const&value){
if constexpr(requires{
JsonCodec<M>::encode(std::declval<ValueBuilder&>(),std::declval<M const&>());
})
return obj.template insert<M>(name,value);
else if constexpr(std::is_signed_v<M>&&std::integral<M>)
return obj.insert_i64(name,static_cast<i64>(value));
else if constexpr(std::is_unsigned_v<M>&&std::integral<M>)
return obj.insert_u64(name,static_cast<u64>(value));
else if constexpr(std::floating_point<M>)
return obj.insert_f64(name,static_cast<double>(value));
else if constexpr(std::convertible_to<M,SV>)
return obj.insert_string(name,static_cast<SV>(value));
else
static_assert(false,"no encode support for reflected member type");
}
}// namespace detail
// ---------------------------------------------------------------------------
// JsonCodec<T> partial specialization — reflection-derived encode / decode
// ---------------------------------------------------------------------------

template<class T>
requires std::is_aggregate_v<T>&&std::default_initializable<T>&&(!requires{JsonMembers<T>::members();})
struct JsonCodec<T>{
static expected<T,JsonError>decode(NodeRef root){
auto obj_res=root.as_object();
if(!obj_res)return unexpected(move(obj_res).error());
auto const&obj=*obj_res;

T result{};
bool ok=true;
JsonError first_err;

constexpr auto N=detail::reflect_member_count<T>();

[&]<SZ...Is>(std::index_sequence<Is...>){
([&]<SZ I>(){
if(!ok)return;
constexpr auto mem=detail::reflect_member_at<T,I>();
if constexpr(detail::reflect_has_skip<mem>())return;

constexpr auto name_info=detail::reflect_field_name<mem>();
SV const field_name{name_info.p,name_info.n};

using M=std::remove_cvref_t<decltype(result.[:mem:])>;
auto node=obj.find_member(field_name);
if(!node){
if constexpr(!detail::is_opt_refl<M>::value){
ok=false;
first_err=JsonError{
.stage=JsonStage::decode,
.code=JsonIssueCode::missing_member,
.member_name=S{field_name},
.message=format("missing member: {}",field_name)};
}
return;
}
auto decoded=detail::decode_reflect_member<M>(*node);
if(!decoded){
ok=false;
first_err=move(decoded).error();
return;
}
result.[:mem:]=move(*decoded);
}.template operator()<Is>(),
...);
}(std::make_index_sequence<N>{});

if(!ok)return unexpected(move(first_err));

// Reject unknown JSON members (default policy: reject)
for(auto const&m:obj.members()){
if(!ok)break;
bool found=false;
[&]<SZ...Is>(std::index_sequence<Is...>){
([&]<SZ I>(){
if(found)return;
constexpr auto mem=detail::reflect_member_at<T,I>();
if constexpr(detail::reflect_has_skip<mem>())return;
constexpr auto ni=detail::reflect_field_name<mem>();
if(SV{ni.p,ni.n}==m.name)found=true;
}.template operator()<Is>(),
...);
}(std::make_index_sequence<N>{});
if(!found){
ok=false;
first_err=JsonError{
.stage=JsonStage::decode,
.code=JsonIssueCode::invalid_value,
.member_name=S{m.name},
.message=format("unknown member: {}",m.name)};
}
}

if(!ok)return unexpected(move(first_err));
return result;
}
static expected<void,JsonError>encode(ValueBuilder&b,T const&value){
auto obj_res=b.begin_object();
if(!obj_res)return unexpected(move(obj_res).error());
auto&obj=*obj_res;

bool ok=true;
JsonError first_err;

constexpr auto N=detail::reflect_member_count<T>();
[&]<SZ...Is>(std::index_sequence<Is...>){
([&]<SZ I>(){
if(!ok)return;
constexpr auto mem=detail::reflect_member_at<T,I>();
if constexpr(detail::reflect_has_skip<mem>())return;

constexpr auto name_info=detail::reflect_field_name<mem>();
SV const field_name{name_info.p,name_info.n};

using M=std::remove_cvref_t<decltype(value.[:mem:])>;
auto res=detail::encode_reflect_member<M>(obj,field_name,value.[:mem:]);
if(!res){
ok=false;
first_err=move(res).error();
}
}.template operator()<Is>(),
...);
}(std::make_index_sequence<N>{});

if(!ok)return unexpected(move(first_err));
move(obj).commit();
return{};
}
static constexpr SV type_name(){return std::meta::display_string_of(^^T);}
};
