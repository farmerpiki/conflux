#include<cstddef>
#include<experimental/simd>

namespace stdx=std::experimental::parallelism_v2;
using vec_t=stdx::native_simd<unsigned char>;
static constexpr std::size_t W=vec_t::size();

extern "C"{
void conflux_ascii_lower_inplace_stdsimd(
char*p,
std::size_t n)noexcept{
auto*u=reinterpret_cast<unsigned char*>(p);
vec_t const va(static_cast<unsigned char>('A'));
vec_t const vz(static_cast<unsigned char>('Z'));
vec_t const bit(static_cast<unsigned char>(0x20));

std::size_t i=0;
for(;i+W<=n;i+=W){
vec_t v(u+i,stdx::element_aligned);
auto upper=(v>=va)&&(v<=vz);
stdx::where(upper,v)=v|bit;
v.copy_to(u+i,stdx::element_aligned);
}
for(;i<n;++i)
if(u[i]>='A'&&u[i]<='Z')
u[i]|=0x20U;
}
int conflux_constant_time_eq_stdsimd(
unsigned char const*a,
unsigned char const*b,
std::size_t n)noexcept{
vec_t acc(static_cast<unsigned char>(0));
std::size_t i=0;
for(;i+W<=n;i+=W){
vec_t va(a+i,stdx::element_aligned);
vec_t vb(b+i,stdx::element_aligned);
acc|=va^vb;
}
unsigned char tail=0;
for(;i<n;++i)
tail=static_cast<unsigned char>(tail|(a[i]^b[i]));
return stdx::all_of(acc==vec_t(static_cast<unsigned char>(0)))&&tail==0?1:0;
}
std::size_t conflux_url_scan_plain_run_stdsimd(
char const*p,
std::size_t n,
int plus_is_special)noexcept{
auto const*u=reinterpret_cast<unsigned char const*>(p);
vec_t const pct(static_cast<unsigned char>('%'));
vec_t const plus(static_cast<unsigned char>('+'));

std::size_t i=0;
for(;i+W<=n;i+=W){
vec_t v(u+i,stdx::element_aligned);
auto m=(v==pct);
if(plus_is_special)
m=m||(v==plus);
if(stdx::any_of(m))
return i+static_cast<std::size_t>(stdx::find_first_set(m));
}
for(;i<n;++i)
if(p[i]=='%'||(plus_is_special!=0&&p[i]=='+'))
return i;
return n;
}
void conflux_ws_unmask_stdsimd(
unsigned char*data,
std::size_t n,
unsigned char const*mask4)noexcept{
alignas(64)unsigned char repeated[W];
for(std::size_t i=0;i<W;++i)
repeated[i]=mask4[i&3];
vec_t const m(repeated,stdx::element_aligned);

std::size_t i=0;
for(;i+W<=n;i+=W){
vec_t v(data+i,stdx::element_aligned);
(v^m).copy_to(data+i,stdx::element_aligned);
}
for(;i<n;++i)
data[i]^=mask4[i&3];
}
}// extern "C"
