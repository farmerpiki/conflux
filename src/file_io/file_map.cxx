module;
#include<cerrno>
#include<fcntl.h>
#include<linux/openat2.h>
#include<memory>
#include<sys/mman.h>
#include<sys/syscall.h>
#include<unistd.h>

export module conflux.file_map;

import std;
import conflux.types;
export import conflux.file_io_sync;
struct MappedRegionEntry{
void const*ptr{};
u64 mmap_size{};
u64 file_size{};
~MappedRegionEntry()noexcept{
if(ptr!=nullptr)
::munmap(const_cast<void*>(ptr),static_cast<SZ>(mmap_size));
}
MappedRegionEntry()noexcept=default;
MappedRegionEntry(void const*p,u64 msz,u64 fsz)noexcept:ptr{p},mmap_size{msz},file_size{fsz}{}
MappedRegionEntry(MappedRegionEntry const&)=delete;
MappedRegionEntry&operator=(MappedRegionEntry const&)=delete;
MappedRegionEntry(MappedRegionEntry&&)=delete;
MappedRegionEntry&operator=(MappedRegionEntry&&)=delete;
};
export class MappedFileLease{
SP<MappedRegionEntry>region_{};
public:
MappedFileLease()noexcept=default;
explicit MappedFileLease(SP<MappedRegionEntry>r)noexcept:region_{move(r)}{}
[[nodiscard]]span<byte const>bytes()const noexcept{
if(!region_||region_->ptr==nullptr)
return{};
return{static_cast<byte const*>(region_->ptr),static_cast<SZ>(region_->file_size)};
}
[[nodiscard]]bool empty()const noexcept{return!region_||region_->ptr==nullptr||region_->file_size==0;}
[[nodiscard]]u64 size()const noexcept{return region_?region_->file_size:0;}
};
export struct MappedBody{
MappedFileLease lease;
u64 offset{};
u64 size{};
[[nodiscard]]span<byte const>window()const noexcept{
auto const full=lease.bytes();
if(offset>=full.size())
return{};
auto const avail=full.size()-static_cast<SZ>(offset);
auto const len=min(static_cast<SZ>(size),avail);
return full.subspan(static_cast<SZ>(offset),len);
}
};
using FileMapError=IoError;
export expected<MappedFileLease,FileMapError>map_fd_readonly_sync(
int fd,
FileStat const&st)noexcept{
if(st.size==0)
return MappedFileLease{};
auto*ptr=::mmap(nullptr,static_cast<SZ>(st.size),PROT_READ,MAP_SHARED,fd,0);
if(ptr==MAP_FAILED)
return unexpected{FileMapError{errno,"file_map: mmap"}};
return MappedFileLease{make_shared<MappedRegionEntry>(ptr,st.size,st.size)};
}
export expected<MappedFileLease,FileMapError>map_file_readonly_sync(
int dir_fd,
SV relative,
SZ max_bytes=NL<SZ>::max())noexcept{
open_how how{};
how.flags=O_RDONLY|O_CLOEXEC;
how.resolve=RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS;
S rel{relative};
int const fd=static_cast<int>(::syscall(SYS_openat2,dir_fd,rel.c_str(),&how,sizeof(how)));
if(fd<0)
return unexpected{FileMapError{errno,"file_map: openat2"}};
UniqueFd guard{fd};

auto st=fstat_sync(fd);
if(!st)
return unexpected{FileMapError{st.error().code().value(),"file_map: fstat"}};
if(st->size>max_bytes)
return unexpected{FileMapError{EFBIG,"file_map: file exceeds max_bytes"}};
return map_fd_readonly_sync(fd,*st);
}
