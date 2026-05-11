module;
#include<cerrno>
#include<fcntl.h>
#include<linux/fs.h>
#include<linux/openat2.h>
#include<memory>
#include<stdio.h>
#include<sys/random.h>
#include<sys/stat.h>
#include<sys/syscall.h>
#include<unistd.h>

export module conflux.file_io_sync;

import std;
import conflux.types;
export import conflux.uring.completion;
// ───────────────────────────────────────────────────────────────────────
// FileStat — portable stat result for cache revalidation.
// ───────────────────────────────────────────────────────────────────────

export struct FileStat{
u64 size{};
u64 mtime_ns{};
u64 ctime_ns{};
u64 dev{};
u64 ino{};
u32 mode{};
};
// ───────────────────────────────────────────────────────────────────────
// UniqueFd — RAII wrapper for raw POSIX fds (no io_uring coupling).
// ───────────────────────────────────────────────────────────────────────

export class UniqueFd{
int fd_{-1};
public:
constexpr UniqueFd()noexcept=default;
constexpr explicit UniqueFd(int fd)noexcept:fd_{fd}{}
UniqueFd(UniqueFd const&)=delete;
UniqueFd&operator=(UniqueFd const&)=delete;
constexpr UniqueFd(UniqueFd&&o)noexcept:fd_{exchange(o.fd_,-1)}{}
constexpr UniqueFd&operator=(UniqueFd&&o)noexcept{
if(this!=&o){
reset();
fd_=exchange(o.fd_,-1);
}
return*this;
}
~UniqueFd()noexcept{reset();}
void reset()noexcept{
if(fd_>=0){
::close(fd_);
fd_=-1;
}
}
[[nodiscard]]constexpr int fd()const noexcept{return fd_;}
[[nodiscard]]constexpr bool valid()const noexcept{return fd_>=0;}
[[nodiscard]]int release()noexcept{return exchange(fd_,-1);}
[[nodiscard]]constexpr explicit operator bool()const noexcept{return fd_>=0;}
};

// ───────────────────────────────────────────────────────────────────────
// Enums + options
// ───────────────────────────────────────────────────────────────────────

export enum class TempPublishMode:u8{
replace_existing,
create_new
};

export enum class TempDurability:u8{
none,
file,
file_and_directory
};
export struct TempFileOptions{
mode_t mode=0644;
bool prefer_otmpfile=true;
TempDurability durability=TempDurability::file_and_directory;
};
// ───────────────────────────────────────────────────────────────────────
// TemporaryFileSync — owns an open temp fd (unnamed or named).
// ───────────────────────────────────────────────────────────────────────

export class TemporaryFileSync{
UniqueFd fd_{};
bool unnamed_{false};
S staging_name_{};
int parent_fd_{-1};
public:
TemporaryFileSync()noexcept=default;
TemporaryFileSync(UniqueFd fd,bool unnamed,int parent_fd)noexcept
:fd_{move(fd)},unnamed_{unnamed},parent_fd_{parent_fd}{}
TemporaryFileSync(UniqueFd fd,S staging_name,int parent_fd)noexcept
:fd_{move(fd)},unnamed_{false},staging_name_{move(staging_name)},parent_fd_{parent_fd}{}
TemporaryFileSync(TemporaryFileSync&&)noexcept=default;
TemporaryFileSync&operator=(TemporaryFileSync&&)noexcept=default;
~TemporaryFileSync()noexcept{
if(!staging_name_.empty()&&parent_fd_>=0)
::unlinkat(parent_fd_,staging_name_.c_str(),0);
}
[[nodiscard]]int fd()const noexcept{return fd_.fd();}
[[nodiscard]]bool unnamed()const noexcept{return unnamed_;}
[[nodiscard]]SV staging_name()const noexcept{return staging_name_;}
[[nodiscard]]int parent_fd()const noexcept{return parent_fd_;}
UniqueFd take_fd()noexcept{return move(fd_);}
void disarm_staging()noexcept{staging_name_.clear();}
};
using FileIoSyncError=IoError;
// ───────────────────────────────────────────────────────────────────────
// Internals
// ───────────────────────────────────────────────────────────────────────

namespace{
Atom<u64>g_staging_counter{0};
inline S make_staging_name()noexcept{
auto const pid=static_cast<u32>(::getpid());
auto const seq=g_staging_counter.fetch_add(1,memory_order_relaxed);
u32 rnd{};
::getrandom(&rnd,sizeof(rnd),0);
return format(".conflux.tmp.{}.{}.{:08x}",pid,seq,rnd);
}
inline expected<void,FileIoSyncError>do_fsync(int fd,TempDurability d)noexcept{
if(d<TempDurability::file)
return{};
int const rc=::fdatasync(fd);
if(rc<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: fdatasync"}};
return{};
}
inline expected<void,FileIoSyncError>do_dir_fsync(int dir_fd,TempDurability d)noexcept{
if(d<TempDurability::file_and_directory)
return{};
int const rc=::fsync(dir_fd);
if(rc<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: dir fsync"}};
return{};
}
constexpr bool is_otmpfile_unsupported_errno(int e)noexcept{
return e==EOPNOTSUPP||e==EISDIR||e==EINVAL||e==ENOSYS||e==EPERM;
}
inline expected<void,FileIoSyncError>link_unnamed_fd(
int tmp_fd,
int parent_fd,
SV staging_name)noexcept{
// AT_EMPTY_PATH — requires CAP_DAC_READ_SEARCH on most kernels
int rc=static_cast<int>(::syscall(SYS_linkat,tmp_fd,"",parent_fd,staging_name.data(),AT_EMPTY_PATH));
if(rc==0)
return{};
int const e1=errno;
if(e1!=EPERM&&e1!=EINVAL&&e1!=ENOENT)
return unexpected{FileIoSyncError{e1,"file_io_sync: linkat AT_EMPTY_PATH"}};

// /proc/self/fd fallback
auto proc=format("/proc/self/fd/{}",tmp_fd);
rc=::linkat(AT_FDCWD,proc.c_str(),parent_fd,staging_name.data(),AT_SYMLINK_FOLLOW);
if(rc==0)
return{};
return unexpected{FileIoSyncError{errno,"file_io_sync: linkat /proc/self/fd"}};
}
inline int openat2_sync(int dir_fd,char const*path,u64 flags,u64 mode,u64 resolve)noexcept{
open_how how{};
how.flags=flags;
how.mode=mode;
how.resolve=resolve;
return static_cast<int>(::syscall(SYS_openat2,dir_fd,path,&how,sizeof(how)));
}
inline expected<UniqueFd,FileIoSyncError>open_parent_dir_contained(
int root_fd,
SV relative_dir)noexcept{
if(relative_dir.empty()||relative_dir=="."){
int fd=openat2_sync(root_fd,".",
O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,
RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS);
if(fd<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: open parent dir"}};
return UniqueFd{fd};
}
S dir_str{relative_dir};
int fd=openat2_sync(root_fd,dir_str.c_str(),
O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,
RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS);
if(fd<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: open parent dir"}};
return UniqueFd{fd};
}
}// namespace
struct PathParts{
SV parent_dir;
SV basename;
};
inline expected<PathParts,FileIoSyncError>split_contained_path(SV path)noexcept{
if(path.empty())
return unexpected{FileIoSyncError{EINVAL,"file_io_sync: empty path"}};
if(path.starts_with('/'))
return unexpected{FileIoSyncError{EINVAL,"file_io_sync: absolute path"}};
if(path.contains('\0'))
return unexpected{FileIoSyncError{EINVAL,"file_io_sync: NUL in path"}};

// reject pure . or ..
if(path=="."||path=="..")
return unexpected{FileIoSyncError{EINVAL,"file_io_sync: invalid path component"}};

// reject path components that are ..
SV remaining=path;
while(!remaining.empty()){
auto const slash=remaining.find('/');
auto const component=remaining.substr(0,slash);
if(component==".."||component.empty()){
if(component=="..")
return unexpected{FileIoSyncError{EINVAL,"file_io_sync: .. in path"}};
}
if(slash==SV::npos)
break;
remaining=remaining.substr(slash+1);
}

auto const last_slash=path.rfind('/');
if(last_slash==SV::npos)
return PathParts{.parent_dir=".",.basename=path};
return PathParts{
.parent_dir=path.substr(0,last_slash),
.basename=path.substr(last_slash+1)};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: open_tmpfile_sync
// ───────────────────────────────────────────────────────────────────────

export expected<TemporaryFileSync,FileIoSyncError>open_tmpfile_sync(
int parent_dir_fd,
TempFileOptions opts={})noexcept{
if(opts.prefer_otmpfile){
int fd=::openat(parent_dir_fd,".",O_TMPFILE|O_WRONLY|O_CLOEXEC,opts.mode);
if(fd>=0)
return TemporaryFileSync{UniqueFd{fd},true,parent_dir_fd};
if(!is_otmpfile_unsupported_errno(errno))
return unexpected{FileIoSyncError{errno,"file_io_sync: O_TMPFILE"}};
}
// named-temp fallback
auto staging=make_staging_name();
int fd=::openat(parent_dir_fd,staging.c_str(),
O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,opts.mode);
if(fd<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: named temp create"}};
return TemporaryFileSync{UniqueFd{fd},move(staging),parent_dir_fd};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: write_all_fd
// ───────────────────────────────────────────────────────────────────────

export expected<void,FileIoSyncError>write_all_fd(
int fd,
span<byte const>bytes)noexcept{
SZ off=0;
while(off<bytes.size()){
auto const n=::write(fd,bytes.data()+off,bytes.size()-off);
if(n<0){
if(errno==EINTR)
continue;
return unexpected{FileIoSyncError{errno,"file_io_sync: write"}};
}
off+=static_cast<SZ>(n);
}
return{};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: publish_tmpfile_sync
// ───────────────────────────────────────────────────────────────────────

export expected<void,FileIoSyncError>publish_tmpfile_sync(
TemporaryFileSync&&tmp,
int parent_dir_fd,
SV final_name,
TempPublishMode mode=TempPublishMode::replace_existing,
TempDurability durability=TempDurability::file_and_directory)noexcept{
if(auto r=do_fsync(tmp.fd(),durability);!r)
return r;

auto staging=S{tmp.staging_name()};
bool need_unlink_staging=false;

if(tmp.unnamed()){
staging=make_staging_name();
auto r=link_unnamed_fd(tmp.fd(),parent_dir_fd,staging);
if(!r){
// O_TMPFILE linkat failed entirely — fall back to error
return unexpected{r.error()};
}
need_unlink_staging=true;
tmp.disarm_staging();
}else{
need_unlink_staging=true;
tmp.disarm_staging();
}

S final_str{final_name};
if(mode==TempPublishMode::replace_existing){
int const rc=::renameat(parent_dir_fd,staging.c_str(),parent_dir_fd,final_str.c_str());
if(rc<0){
if(need_unlink_staging)
::unlinkat(parent_dir_fd,staging.c_str(),0);
return unexpected{FileIoSyncError{errno,"file_io_sync: renameat"}};
}
}else{
// create_new — use renameat2 RENAME_NOREPLACE
int const rc=static_cast<int>(::syscall(SYS_renameat2,
parent_dir_fd,staging.c_str(),
parent_dir_fd,final_str.c_str(),
RENAME_NOREPLACE));
if(rc<0){
int const e=errno;
if(need_unlink_staging)
::unlinkat(parent_dir_fd,staging.c_str(),0);
if(e==ENOSYS)
return unexpected{FileIoSyncError{ENOTSUP,"file_io_sync: RENAME_NOREPLACE unsupported"}};
return unexpected{FileIoSyncError{e,"file_io_sync: renameat2 RENAME_NOREPLACE"}};
}
}

auto r=do_dir_fsync(parent_dir_fd,durability);
// fd cleanup happens via TemporaryFileSync destructor
return r;
}
// ───────────────────────────────────────────────────────────────────────
// High-level: write_file_atomic_at_sync
// ───────────────────────────────────────────────────────────────────────

export expected<void,FileIoSyncError>write_file_atomic_at_sync(
int root_fd,
SV contained_relative_path,
span<byte const>bytes,
TempFileOptions opts={},
TempPublishMode mode=TempPublishMode::replace_existing)noexcept{
auto parts=split_contained_path(contained_relative_path);
if(!parts)
return unexpected{parts.error()};

auto parent=open_parent_dir_contained(root_fd,parts->parent_dir);
if(!parent)
return unexpected{parent.error()};

auto durability=opts.durability;
auto tmp=open_tmpfile_sync(parent->fd(),opts);
if(!tmp)
return unexpected{tmp.error()};

auto wr=write_all_fd(tmp->fd(),bytes);
if(!wr)
return unexpected{wr.error()};

return publish_tmpfile_sync(move(*tmp),parent->fd(),parts->basename,mode,durability);
}
// ───────────────────────────────────────────────────────────────────────
// High-level: write_text_file_atomic_at_sync
// ───────────────────────────────────────────────────────────────────────

export expected<void,FileIoSyncError>write_text_file_atomic_at_sync(
int root_fd,
SV contained_relative_path,
SV text,
TempFileOptions opts={},
TempPublishMode mode=TempPublishMode::replace_existing)noexcept{
return write_file_atomic_at_sync(root_fd,contained_relative_path,
as_bytes(span{text.data(),text.size()}),opts,mode);
}
// ───────────────────────────────────────────────────────────────────────
// fstat_sync / stat_at_sync — populate FileStat from kernel statx.
// ───────────────────────────────────────────────────────────────────────

export expected<FileStat,FileIoSyncError>fstat_sync(int fd)noexcept{
struct statx stx{};
int const rc=::statx(fd,"",AT_EMPTY_PATH,
STATX_BASIC_STATS|STATX_MTIME|STATX_CTIME,&stx);
if(rc<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: statx"}};
return FileStat{
.size=stx.stx_size,
.mtime_ns=static_cast<u64>(stx.stx_mtime.tv_sec)*1000000000ULL+stx.stx_mtime.tv_nsec,
.ctime_ns=static_cast<u64>(stx.stx_ctime.tv_sec)*1000000000ULL+stx.stx_ctime.tv_nsec,
.dev=static_cast<u64>(stx.stx_dev_major)<<32U|stx.stx_dev_minor,
.ino=stx.stx_ino,
.mode=stx.stx_mode};
}
export expected<FileStat,FileIoSyncError>stat_at_sync(
int dir_fd,
SV path)noexcept{
S p{path};
struct statx stx{};
int const rc=::statx(dir_fd,p.c_str(),0,
STATX_BASIC_STATS|STATX_MTIME|STATX_CTIME,&stx);
if(rc<0)
return unexpected{FileIoSyncError{errno,"file_io_sync: statx"}};
return FileStat{
.size=stx.stx_size,
.mtime_ns=static_cast<u64>(stx.stx_mtime.tv_sec)*1000000000ULL+stx.stx_mtime.tv_nsec,
.ctime_ns=static_cast<u64>(stx.stx_ctime.tv_sec)*1000000000ULL+stx.stx_ctime.tv_nsec,
.dev=static_cast<u64>(stx.stx_dev_major)<<32U|stx.stx_dev_minor,
.ino=stx.stx_ino,
.mode=stx.stx_mode};
}
