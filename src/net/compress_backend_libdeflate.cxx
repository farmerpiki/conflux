module;
#include<libdeflate.h>

export module conflux.net.compress.backend.libdeflate;
import std;
import conflux.types;
export namespace conflux::compress_backends{
S libdeflate_gzip_compress(
SV input){
struct CompressorHolder{
libdeflate_compressor*ptr{libdeflate_alloc_compressor(6)};
~CompressorHolder(){libdeflate_free_compressor(ptr);}
};
thread_local CompressorHolder compressor{};
if(compressor.ptr==nullptr)
return{};

auto const bound=libdeflate_gzip_compress_bound(compressor.ptr,input.size());
S out(bound,'\0');
auto const produced=libdeflate_gzip_compress(compressor.ptr,input.data(),input.size(),out.data(),out.size());
if(produced==0)
return{};
out.resize(produced);
return out;
}
}// namespace conflux::compress_backends
