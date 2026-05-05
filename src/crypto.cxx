module;
#if defined(CONFLUX_CRYPTO_USE_AESNI)
extern "C"{
int conflux_aes_gcm_encrypt_aesni(
unsigned char const*key,
unsigned char const*iv,
unsigned char const*pt,
__SIZE_TYPE__ pt_len,
unsigned char const*aad,
__SIZE_TYPE__ aad_len,
unsigned char*out);
int conflux_aes_gcm_decrypt_aesni(
unsigned char const*key,
unsigned char const*iv,
unsigned char const*ct_tag,
__SIZE_TYPE__ ct_tag_len,
unsigned char const*aad,
__SIZE_TYPE__ aad_len,
unsigned char*out);
}
#endif
#if defined(CONFLUX_STDSIMD)
extern "C"{
int conflux_constant_time_eq_stdsimd(unsigned char const*a,unsigned char const*b,__SIZE_TYPE__ n);
}
#endif

export module conflux.crypto;
import std;
import conflux.types;
import std.compat;
namespace{
constexpr SV kB64Alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr SV kB64UrlAlphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr A<i8,256>make_b64_table(
SV alphabet){
A<i8,256>t{};
t.fill(-1);
for(SZ i=0;i<64;++i)
t[static_cast<unsigned char>(alphabet[i])]=static_cast<i8>(i);
return t;
}
constexpr auto kB64Table=make_b64_table(kB64Alphabet);
constexpr auto kB64UrlTable=make_b64_table(kB64UrlAlphabet);
S b64_encode_impl(
span<unsigned char const>in,
SV alphabet,
bool padding){
S out;
out.reserve(((in.size()+2)/3)*4);
for(SZ i=0;i<in.size();i+=3){
unsigned int v=static_cast<unsigned int>(in[i])<<16U;
if(i+1<in.size())
v|=static_cast<unsigned int>(in[i+1])<<8U;
if(i+2<in.size())
v|=static_cast<unsigned int>(in[i+2]);
out+=alphabet[(v>>18U)&0x3FU];
out+=alphabet[(v>>12U)&0x3FU];
if(i+1<in.size())
out+=alphabet[(v>>6U)&0x3FU];
else if(padding)
out+='=';
if(i+2<in.size())
out+=alphabet[v&0x3FU];
else if(padding)
out+='=';
}
return out;
}
S b64_decode_impl(
SV encoded,
span<i8 const,256>table){
S out;
out.reserve(((encoded.size()*3)/4)+1);
int bits=0;
int val=0;
for(char const raw:encoded){
auto c=static_cast<unsigned char>(raw);
if(c=='=')
break;
i8 const d=table[c];
if(d<0)
return{};
val=(val<<6)|d;
bits+=6;
if(bits>=8){
bits-=8;
out+=static_cast<char>((val>>bits)&0xFF);
}
}
return out;
}
}// namespace
export[[gnu::always_inline]]inline span<unsigned char const>to_unsigned_span(
SV s)noexcept{
return{
reinterpret_cast<unsigned char const*>(s.data()),
s.size()};// NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
export S base64_encode(
span<unsigned char const>in){
return b64_encode_impl(in,kB64Alphabet,true);
}
export S base64_decode(
SV encoded){
return b64_decode_impl(encoded,kB64Table);
}
export S base64url_encode(
span<unsigned char const>in){
return b64_encode_impl(in,kB64UrlAlphabet,false);
}
export S base64url_decode(
SV encoded){
return b64_decode_impl(encoded,kB64UrlTable);
}
// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4)
// ---------------------------------------------------------------------------

export A<unsigned char,20>sha1(
span<unsigned char const>msg){
A<u32,5>h{0x67452301U,0xEFCDAB89U,0x98BADCFEU,0x10325476U,0xC3D2E1F0U};

V<unsigned char>padded;
padded.reserve(msg.size()+72);
padded.insert(padded.end(),msg.begin(),msg.end());
padded.push_back(0x80U);
while((padded.size()%64)!=56)
padded.push_back(0);
u64 const bit_len=msg.size()*8ULL;
for(int s=56;s>=0;s-=8)
padded.push_back(static_cast<unsigned char>((bit_len>>s)&0xFF));

auto rot32=[](u32 v,unsigned n)->u32{return(v<<n)|(v>>(32-n));};

for(SZ blk=0;blk<padded.size();blk+=64){
A<u32,80>w{};
for(int i=0;i<16;++i){
auto b=span{padded}.subspan(blk+(static_cast<SZ>(i)*4),4);
w[static_cast<SZ>(i)]=(static_cast<u32>(b[0])<<24)|(static_cast<u32>(b[1])<<16)|(static_cast<u32>(b[2])<<8)|static_cast<u32>(b[3]);
}
for(SZ i=16;i<80;++i)
w[i]=rot32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
auto[a,b,c,d,e]=h;
for(SZ i=0;i<80;++i){
u32 f{};
u32 k{};
if(i<20){
f=(b&c)|(~b&d);
k=0x5A827999U;
}else if(i<40){
f=b^c^d;
k=0x6ED9EBA1U;
}else if(i<60){
f=(b&c)|(b&d)|(c&d);
k=0x8F1BBCDCU;
}else{
f=b^c^d;
k=0xCA62C1D6U;
}
u32 const tmp=rot32(a,5)+f+e+k+w[i];
e=d;
d=c;
c=rot32(b,30);
b=a;
a=tmp;
}
h[0]+=a;
h[1]+=b;
h[2]+=c;
h[3]+=d;
h[4]+=e;
}

A<unsigned char,20>out{};
for(SZ i=0;i<5;++i){
out[(i*4)+0]=static_cast<unsigned char>(h[i]>>24);
out[(i*4)+1]=static_cast<unsigned char>(h[i]>>16);
out[(i*4)+2]=static_cast<unsigned char>(h[i]>>8);
out[(i*4)+3]=static_cast<unsigned char>(h[i]);
}
return out;
}
// ---------------------------------------------------------------------------
// HMAC-SHA1 (RFC 2104)
// ---------------------------------------------------------------------------

export A<unsigned char,20>hmac_sha1(
span<unsigned char const>key,
span<unsigned char const>msg){
A<unsigned char,64>k_pad{};
if(key.size()>64){
auto kh=sha1(key);
ranges::copy(kh,k_pad.begin());
}else{
ranges::copy(key,k_pad.begin());
}

V<unsigned char>inner_buf(64+msg.size());
for(SZ i=0;i<64;++i)
inner_buf[i]=static_cast<unsigned char>(k_pad[i]^0x36U);
ranges::copy(msg,inner_buf.begin()+64);
auto inner=sha1(inner_buf);

A<unsigned char,84>outer_buf{};
for(SZ i=0;i<64;++i)
outer_buf[i]=static_cast<unsigned char>(k_pad[i]^0x5CU);
ranges::copy(inner,outer_buf.begin()+64);
return sha1(span{outer_buf.data(),84});
}
// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

export A<unsigned char,32>sha256(
span<unsigned char const>msg){
static constexpr A<u32,64>K{
0x428a2f98U,
0x71374491U,
0xb5c0fbcfU,
0xe9b5dba5U,
0x3956c25bU,
0x59f111f1U,
0x923f82a4U,
0xab1c5ed5U,
0xd807aa98U,
0x12835b01U,
0x243185beU,
0x550c7dc3U,
0x72be5d74U,
0x80deb1feU,
0x9bdc06a7U,
0xc19bf174U,
0xe49b69c1U,
0xefbe4786U,
0x0fc19dc6U,
0x240ca1ccU,
0x2de92c6fU,
0x4a7484aaU,
0x5cb0a9dcU,
0x76f988daU,
0x983e5152U,
0xa831c66dU,
0xb00327c8U,
0xbf597fc7U,
0xc6e00bf3U,
0xd5a79147U,
0x06ca6351U,
0x14292967U,
0x27b70a85U,
0x2e1b2138U,
0x4d2c6dfcU,
0x53380d13U,
0x650a7354U,
0x766a0abbU,
0x81c2c92eU,
0x92722c85U,
0xa2bfe8a1U,
0xa81a664bU,
0xc24b8b70U,
0xc76c51a3U,
0xd192e819U,
0xd6990624U,
0xf40e3585U,
0x106aa070U,
0x19a4c116U,
0x1e376c08U,
0x2748774cU,
0x34b0bcb5U,
0x391c0cb3U,
0x4ed8aa4aU,
0x5b9cca4fU,
0x682e6ff3U,
0x748f82eeU,
0x78a5636fU,
0x84c87814U,
0x8cc70208U,
0x90befffaU,
0xa4506cebU,
0xbef9a3f7U,
0xc67178f2U,
};

A<u32,8>h{
0x6a09e667U,
0xbb67ae85U,
0x3c6ef372U,
0xa54ff53aU,
0x510e527fU,
0x9b05688cU,
0x1f83d9abU,
0x5be0cd19U,
};

V<unsigned char>padded;
padded.reserve(msg.size()+72);
padded.insert(padded.end(),msg.begin(),msg.end());
padded.push_back(0x80U);
while((padded.size()%64)!=56)
padded.push_back(0);
u64 const bit_len=msg.size()*8ULL;
for(int s=56;s>=0;s-=8)
padded.push_back(static_cast<unsigned char>((bit_len>>s)&0xFF));

auto rotr=[](u32 v,unsigned n)->u32{return(v>>n)|(v<<(32-n));};
auto ch=[](u32 e,u32 f,u32 g){return(e&f)^(~e&g);};
auto maj=[](u32 a,u32 b,u32 c){return(a&b)^(a&c)^(b&c);};

for(SZ blk=0;blk<padded.size();blk+=64){
A<u32,64>w{};
for(SZ i=0;i<16;++i){
auto b=span{padded}.subspan(blk+(i*4),4);
w[i]=(static_cast<u32>(b[0])<<24)|(static_cast<u32>(b[1])<<16)|(static_cast<u32>(b[2])<<8)|static_cast<u32>(b[3]);
}
for(SZ i=16;i<64;++i){
auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
auto s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
w[i]=w[i-16]+s0+w[i-7]+s1;
}
auto[a,b,c,d,e,f,g,hh]=h;
for(SZ i=0;i<64;++i){
auto S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
auto S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
auto temp1=hh+S1+ch(e,f,g)+K[i]+w[i];
auto temp2=S0+maj(a,b,c);
hh=g;
g=f;
f=e;
e=d+temp1;
d=c;
c=b;
b=a;
a=temp1+temp2;
}
h[0]+=a;
h[1]+=b;
h[2]+=c;
h[3]+=d;
h[4]+=e;
h[5]+=f;
h[6]+=g;
h[7]+=hh;
}

A<unsigned char,32>out{};
for(SZ i=0;i<32;++i)
out[i]=static_cast<unsigned char>((h[i/4]>>(24-((i%4)*8)))&0xFFU);
return out;
}
// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------

export A<unsigned char,32>hmac_sha256(
span<unsigned char const>key,
span<unsigned char const>msg){
A<unsigned char,64>k_pad{};
if(key.size()>64){
auto kh=sha256(key);
ranges::copy(kh,k_pad.begin());
}else{
ranges::copy(key,k_pad.begin());
}

auto xor_pad=[&](unsigned char mask){
V<unsigned char>buf(64+msg.size());
for(SZ i=0;i<64;++i)
buf[i]=static_cast<unsigned char>(k_pad[i]^mask);
ranges::copy(msg,buf.begin()+64);
return buf;
};

auto inner=sha256(xor_pad(0x36U));
V<unsigned char>outer_input(64+32);
for(SZ i=0;i<64;++i)
outer_input[i]=static_cast<unsigned char>(k_pad[i]^0x5CU);
ranges::copy(inner,outer_input.begin()+64);
return sha256(outer_input);
}
export bool constant_time_eq(
SV a,
SV b){
if(a.size()!=b.size())
return false;
#if defined(CONFLUX_STDSIMD)
return conflux_constant_time_eq_stdsimd(
reinterpret_cast<unsigned char const*>(a.data()),
reinterpret_cast<unsigned char const*>(b.data()),
a.size())!=0;
#else
unsigned char acc=0;
for(SZ i=0;i<a.size();++i)
acc=static_cast<unsigned char>(acc|(static_cast<unsigned char>(a[i])^static_cast<unsigned char>(b[i])));
return acc==0;
#endif
}
// ---------------------------------------------------------------------------
// AES-256-GCM (NIST SP 800-38D)
// ---------------------------------------------------------------------------

namespace{
constexpr A<unsigned char,256>kAesSbox{
0x63,
0x7c,
0x77,
0x7b,
0xf2,
0x6b,
0x6f,
0xc5,
0x30,
0x01,
0x67,
0x2b,
0xfe,
0xd7,
0xab,
0x76,
0xca,
0x82,
0xc9,
0x7d,
0xfa,
0x59,
0x47,
0xf0,
0xad,
0xd4,
0xa2,
0xaf,
0x9c,
0xa4,
0x72,
0xc0,
0xb7,
0xfd,
0x93,
0x26,
0x36,
0x3f,
0xf7,
0xcc,
0x34,
0xa5,
0xe5,
0xf1,
0x71,
0xd8,
0x31,
0x15,
0x04,
0xc7,
0x23,
0xc3,
0x18,
0x96,
0x05,
0x9a,
0x07,
0x12,
0x80,
0xe2,
0xeb,
0x27,
0xb2,
0x75,
0x09,
0x83,
0x2c,
0x1a,
0x1b,
0x6e,
0x5a,
0xa0,
0x52,
0x3b,
0xd6,
0xb3,
0x29,
0xe3,
0x2f,
0x84,
0x53,
0xd1,
0x00,
0xed,
0x20,
0xfc,
0xb1,
0x5b,
0x6a,
0xcb,
0xbe,
0x39,
0x4a,
0x4c,
0x58,
0xcf,
0xd0,
0xef,
0xaa,
0xfb,
0x43,
0x4d,
0x33,
0x85,
0x45,
0xf9,
0x02,
0x7f,
0x50,
0x3c,
0x9f,
0xa8,
0x51,
0xa3,
0x40,
0x8f,
0x92,
0x9d,
0x38,
0xf5,
0xbc,
0xb6,
0xda,
0x21,
0x10,
0xff,
0xf3,
0xd2,
0xcd,
0x0c,
0x13,
0xec,
0x5f,
0x97,
0x44,
0x17,
0xc4,
0xa7,
0x7e,
0x3d,
0x64,
0x5d,
0x19,
0x73,
0x60,
0x81,
0x4f,
0xdc,
0x22,
0x2a,
0x90,
0x88,
0x46,
0xee,
0xb8,
0x14,
0xde,
0x5e,
0x0b,
0xdb,
0xe0,
0x32,
0x3a,
0x0a,
0x49,
0x06,
0x24,
0x5c,
0xc2,
0xd3,
0xac,
0x62,
0x91,
0x95,
0xe4,
0x79,
0xe7,
0xc8,
0x37,
0x6d,
0x8d,
0xd5,
0x4e,
0xa9,
0x6c,
0x56,
0xf4,
0xea,
0x65,
0x7a,
0xae,
0x08,
0xba,
0x78,
0x25,
0x2e,
0x1c,
0xa6,
0xb4,
0xc6,
0xe8,
0xdd,
0x74,
0x1f,
0x4b,
0xbd,
0x8b,
0x8a,
0x70,
0x3e,
0xb5,
0x66,
0x48,
0x03,
0xf6,
0x0e,
0x61,
0x35,
0x57,
0xb9,
0x86,
0xc1,
0x1d,
0x9e,
0xe1,
0xf8,
0x98,
0x11,
0x69,
0xd9,
0x8e,
0x94,
0x9b,
0x1e,
0x87,
0xe9,
0xce,
0x55,
0x28,
0xdf,
0x8c,
0xa1,
0x89,
0x0d,
0xbf,
0xe6,
0x42,
0x68,
0x41,
0x99,
0x2d,
0x0f,
0xb0,
0x54,
0xbb,
0x16,
};

constexpr A<unsigned char,11>kAesRcon{0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
constexpr u32 aes_word(
unsigned char a,
unsigned char b,
unsigned char c,
unsigned char d){
return(static_cast<u32>(a)<<24)|(static_cast<u32>(b)<<16)|(static_cast<u32>(c)<<8)|d;
}
constexpr u32 aes_sub_word(
u32 w){
return aes_word(
kAesSbox[(w>>24)&0xFF],
kAesSbox[(w>>16)&0xFF],
kAesSbox[(w>>8)&0xFF],
kAesSbox[w&0xFF]);
}
constexpr u32 aes_rot_word(
u32 w){
return(w<<8)|(w>>24);
}
struct AesKey256{
A<u32,60>rk{};
};
AesKey256 aes256_expand_key(
span<unsigned char const>key){
AesKey256 ek{};
for(SZ i=0;i<8;++i)
ek.rk[i]=aes_word(key[i*4],key[i*4+1],key[i*4+2],key[i*4+3]);
for(SZ i=8;i<60;++i){
u32 t=ek.rk[i-1];
if(i%8==0)
t=aes_sub_word(aes_rot_word(t))^(static_cast<u32>(kAesRcon[i/8])<<24);
else if(i%8==4)
t=aes_sub_word(t);
ek.rk[i]=ek.rk[i-8]^t;
}
return ek;
}
constexpr unsigned char xtime(
unsigned char x){
return static_cast<unsigned char>((x<<1)^((x>>7)*0x1b));
}
void aes256_encrypt_block(
AesKey256 const&ek,
unsigned char const in[16],
unsigned char out[16]){
A<unsigned char,16>s{};
for(SZ i=0;i<16;++i)
s[i]=static_cast<unsigned char>(in[i]^static_cast<unsigned char>(ek.rk[i/4]>>(24-(i%4)*8)));

for(int round=1;round<=14;++round){
A<unsigned char,16>t{};
for(SZ i=0;i<16;++i)
t[i]=kAesSbox[s[i]];
// ShiftRows
A<unsigned char,16>sr{};
for(SZ col=0;col<4;++col){
sr[col*4+0]=t[col*4+0];
sr[col*4+1]=t[((col+1)%4)*4+1];
sr[col*4+2]=t[((col+2)%4)*4+2];
sr[col*4+3]=t[((col+3)%4)*4+3];
}
if(round<14){
// MixColumns
for(SZ col=0;col<4;++col){
auto a0=sr[col*4+0],a1=sr[col*4+1],a2=sr[col*4+2],a3=sr[col*4+3];
auto x0=xtime(a0),x1=xtime(a1),x2=xtime(a2),x3=xtime(a3);
s[col*4+0]=static_cast<unsigned char>(x0^x1^a1^a2^a3);
s[col*4+1]=static_cast<unsigned char>(a0^x1^x2^a2^a3);
s[col*4+2]=static_cast<unsigned char>(a0^a1^x2^x3^a3);
s[col*4+3]=static_cast<unsigned char>(x0^a0^a1^a2^x3);
}
}else{
ranges::copy(sr,s.begin());
}
// AddRoundKey
for(SZ i=0;i<16;++i)
s[i]=static_cast<unsigned char>(
s[i]^static_cast<unsigned char>(ek.rk[static_cast<SZ>(round)*4+i/4]>>(24-(i%4)*8)));
}
ranges::copy(s,out);
}
// GCM: GF(2^128) multiplication (bit-by-bit, constant-time for equal-length inputs)
void ghash_mult(
unsigned char result[16],
unsigned char const h[16],
unsigned char const x[16]){
A<unsigned char,16>v{};
ranges::copy(span{h,16},v.begin());
A<unsigned char,16>z{};

for(int i=0;i<128;++i){
if(((x[i/8]>>(7-(i%8)))&1)!=0)
for(SZ j=0;j<16;++j)
z[j]=static_cast<unsigned char>(z[j]^v[j]);
unsigned char const carry=v[15]&1;
for(int j=15;j>0;--j)
v[static_cast<SZ>(j)]=
static_cast<unsigned char>((v[static_cast<SZ>(j)]>>1)|(v[static_cast<SZ>(j)-1]<<7));
v[0]>>=1;
if(carry!=0u)
v[0]=static_cast<unsigned char>(v[0]^0xe1);
}
ranges::copy(z,result);
}
void ghash_update(
unsigned char state[16],
unsigned char const h[16],
span<unsigned char const>data){
A<unsigned char,16>block{};
SZ pos=0;
while(pos<data.size()){
SZ const chunk=min(SZ{16},data.size()-pos);
block.fill(0);
ranges::copy(data.subspan(pos,chunk),block.begin());
for(SZ i=0;i<16;++i)
state[i]=static_cast<unsigned char>(state[i]^block[i]);
ghash_mult(state,h,state);
pos+=16;
}
}
void gcm_inc32(
unsigned char ctr[16]){
for(int i=15;i>=12;--i)
if(++ctr[i]!=0)
break;
}
}// namespace
export expected<V<unsigned char>,S>aes_gcm_encrypt(
span<unsigned char const>key,
span<unsigned char const>iv,
span<unsigned char const>plaintext,
span<unsigned char const>aad){
if(key.size()!=32)
return unexpected(S{"aes_gcm_encrypt: key must be 32 bytes"});
if(iv.size()!=12)
return unexpected(S{"aes_gcm_encrypt: iv must be 12 bytes"});

#if defined(CONFLUX_CRYPTO_USE_AESNI)
V<unsigned char>out(plaintext.size()+16);
conflux_aes_gcm_encrypt_aesni(
key.data(),
iv.data(),
plaintext.data(),
plaintext.size(),
aad.data(),
aad.size(),
out.data());
return out;
#else
auto const ek=aes256_expand_key(key);

A<unsigned char,16>h_in{};
A<unsigned char,16>h{};
aes256_encrypt_block(ek,h_in.data(),h.data());

A<unsigned char,16>j0{};
ranges::copy(iv,j0.begin());
j0[15]=1;

A<unsigned char,16>ctr{};
ranges::copy(j0,ctr.begin());

V<unsigned char>ct(plaintext.size());
A<unsigned char,16>keystream{};
for(SZ i=0;i<plaintext.size();i+=16){
gcm_inc32(ctr.data());
aes256_encrypt_block(ek,ctr.data(),keystream.data());
SZ const chunk=min(SZ{16},plaintext.size()-i);
for(SZ j=0;j<chunk;++j)
ct[i+j]=static_cast<unsigned char>(plaintext[i+j]^keystream[j]);
}

A<unsigned char,16>ghash_state{};
if(!aad.empty())
ghash_update(ghash_state.data(),h.data(),aad);
ghash_update(ghash_state.data(),h.data(),ct);

A<unsigned char,16>len_block{};
u64 const aad_bits=aad.size()*8;
u64 const ct_bits=ct.size()*8;
for(int i=0;i<8;++i){
len_block[static_cast<SZ>(i)]=static_cast<unsigned char>(aad_bits>>(56-i*8));
len_block[static_cast<SZ>(i)+8]=static_cast<unsigned char>(ct_bits>>(56-i*8));
}
for(SZ i=0;i<16;++i)
ghash_state[i]=static_cast<unsigned char>(ghash_state[i]^len_block[i]);
ghash_mult(ghash_state.data(),h.data(),ghash_state.data());

A<unsigned char,16>tag{};
aes256_encrypt_block(ek,j0.data(),tag.data());
for(SZ i=0;i<16;++i)
tag[i]=static_cast<unsigned char>(tag[i]^ghash_state[i]);

V<unsigned char>out;
out.reserve(ct.size()+16);
out.insert(out.end(),ct.begin(),ct.end());
out.insert(out.end(),tag.begin(),tag.end());
return out;
#endif
}
export expected<V<unsigned char>,S>aes_gcm_decrypt(
span<unsigned char const>key,
span<unsigned char const>iv,
span<unsigned char const>ciphertext_and_tag,
span<unsigned char const>aad){
if(key.size()!=32)
return unexpected(S{"aes_gcm_decrypt: key must be 32 bytes"});
if(iv.size()!=12)
return unexpected(S{"aes_gcm_decrypt: iv must be 12 bytes"});
if(ciphertext_and_tag.size()<16)
return unexpected(S{"aes_gcm_decrypt: input too short (need at least tag)"});

#if defined(CONFLUX_CRYPTO_USE_AESNI)
SZ const ct_len=ciphertext_and_tag.size()-16;
V<unsigned char>pt(ct_len);
int const rc=conflux_aes_gcm_decrypt_aesni(
key.data(),
iv.data(),
ciphertext_and_tag.data(),
ciphertext_and_tag.size(),
aad.data(),
aad.size(),
pt.data());
if(rc!=0)
return unexpected(S{"aes_gcm_decrypt: authentication failed"});
return pt;
#else
SZ const ct_len=ciphertext_and_tag.size()-16;
auto const ct=ciphertext_and_tag.subspan(0,ct_len);
auto const claimed_tag=ciphertext_and_tag.subspan(ct_len,16);

auto const ek=aes256_expand_key(key);

A<unsigned char,16>h_in{};
A<unsigned char,16>h{};
aes256_encrypt_block(ek,h_in.data(),h.data());

A<unsigned char,16>j0{};
ranges::copy(iv,j0.begin());
j0[15]=1;

// Verify tag before decrypting (authenticate-then-decrypt)
A<unsigned char,16>ghash_state{};
if(!aad.empty())
ghash_update(ghash_state.data(),h.data(),aad);
ghash_update(ghash_state.data(),h.data(),ct);

A<unsigned char,16>len_block{};
u64 const aad_bits=aad.size()*8;
u64 const ct_bits=ct.size()*8;
for(int i=0;i<8;++i){
len_block[static_cast<SZ>(i)]=static_cast<unsigned char>(aad_bits>>(56-i*8));
len_block[static_cast<SZ>(i)+8]=static_cast<unsigned char>(ct_bits>>(56-i*8));
}
for(SZ i=0;i<16;++i)
ghash_state[i]=static_cast<unsigned char>(ghash_state[i]^len_block[i]);
ghash_mult(ghash_state.data(),h.data(),ghash_state.data());

A<unsigned char,16>expected_tag{};
aes256_encrypt_block(ek,j0.data(),expected_tag.data());
for(SZ i=0;i<16;++i)
expected_tag[i]=static_cast<unsigned char>(expected_tag[i]^ghash_state[i]);

// Constant-time tag comparison
unsigned char tag_diff=0;
for(SZ i=0;i<16;++i)
tag_diff=static_cast<unsigned char>(tag_diff|(expected_tag[i]^claimed_tag[i]));
if(tag_diff!=0)
return unexpected(S{"aes_gcm_decrypt: authentication failed"});

// Decrypt CTR
A<unsigned char,16>ctr{};
ranges::copy(j0,ctr.begin());

V<unsigned char>pt(ct_len);
A<unsigned char,16>keystream{};
for(SZ i=0;i<ct_len;i+=16){
gcm_inc32(ctr.data());
aes256_encrypt_block(ek,ctr.data(),keystream.data());
SZ const chunk=min(SZ{16},ct_len-i);
for(SZ j=0;j<chunk;++j)
pt[i+j]=static_cast<unsigned char>(ct[i+j]^keystream[j]);
}
return pt;
#endif
}
