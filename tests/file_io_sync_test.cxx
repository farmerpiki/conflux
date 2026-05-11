#include<catch2/catch_test_macros.hpp>
#include<fcntl.h>
#include<sys/stat.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.file_io_sync;
namespace{
struct TempDir{
S path{};
int fd{-1};
TempDir()=default;
TempDir(S p,int f)noexcept:path{move(p)},fd{f}{}
TempDir(TempDir const&)=delete;
TempDir&operator=(TempDir const&)=delete;
TempDir(TempDir&&o)noexcept:path{move(o.path)},fd{exchange(o.fd,-1)}{}
TempDir&operator=(TempDir&&)=delete;
~TempDir(){
if(fd>=0)
::close(fd);
if(!path.empty()){
auto cmd=format("rm -rf {}",path);
auto _=::system(cmd.c_str());
}
}
static TempDir create(){
S p="/tmp/conflux_fio_sync_XXXXXX";
auto*r=::mkdtemp(p.data());
REQUIRE(r!=nullptr);
int f=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
REQUIRE(f>=0);
return TempDir{move(p),f};
}
S read_file(SV name)const{
auto full=format("{}/{}",path,name);
int f=::open(full.c_str(),O_RDONLY);
if(f<0)return{};
char buf[4096];
auto n=::read(f,buf,sizeof(buf));
::close(f);
if(n<0)return{};
return S{buf,static_cast<SZ>(n)};
}
bool file_exists(SV name)const{
auto full=format("{}/{}",path,name);
struct stat st{};
return::stat(full.c_str(),&st)==0;
}
void write_file(SV name,SV content)const{
auto full=format("{}/{}",path,name);
int f=::open(full.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
REQUIRE(f>=0);
auto _=::write(f,content.data(),content.size());
::close(f);
}
void mkdir_sub(SV name)const{
auto full=format("{}/{}",path,name);
auto _=::mkdir(full.c_str(),0755);
}
};
}// namespace
TEST_CASE(
"file_io_sync: open_tmpfile_sync creates writable unnamed temp",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto result=open_tmpfile_sync(dir.fd);
REQUIRE(result.has_value());
CHECK(result->fd()>=0);
CHECK(result->unnamed());
}
TEST_CASE(
"file_io_sync: write_file_atomic_at_sync creates new file",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
SV text="hello atomic world";
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"newfile.txt"},text);
REQUIRE(r.has_value());
CHECK(dir.read_file("newfile.txt")==text);
}
TEST_CASE(
"file_io_sync: write_file_atomic_at_sync replaces existing file",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
dir.write_file("target.txt","old content");
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"target.txt"},SV{"new content"});
REQUIRE(r.has_value());
CHECK(dir.read_file("target.txt")=="new content");
}
TEST_CASE(
"file_io_sync: failed publish does not remove existing target",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
dir.write_file("keep.txt","original");
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"keep.txt"},SV{"overwrite"},
TempFileOptions{},TempPublishMode::create_new);
CHECK(!r.has_value());
CHECK(dir.read_file("keep.txt")=="original");
}
TEST_CASE(
"file_io_sync: nested relative path stays below root",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
dir.mkdir_sub("sub");
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"sub/nested.txt"},SV{"deep"});
REQUIRE(r.has_value());
CHECK(dir.read_file("sub/nested.txt")=="deep");
}
TEST_CASE(
"file_io_sync: absolute path rejected",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"/etc/passwd"},SV{"nope"});
CHECK(!r.has_value());
CHECK(r.error().code().value()==EINVAL);
}
TEST_CASE(
"file_io_sync: .. path rejected",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"../escape.txt"},SV{"nope"});
CHECK(!r.has_value());
CHECK(r.error().code().value()==EINVAL);
}
TEST_CASE(
"file_io_sync: named-temp fallback works when O_TMPFILE disabled",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"fallback.txt"},SV{"via named"},
TempFileOptions{.prefer_otmpfile=false});
REQUIRE(r.has_value());
CHECK(dir.read_file("fallback.txt")=="via named");
}
TEST_CASE(
"file_io_sync: file_and_directory durability path runs without error",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"durable.txt"},SV{"synced"},
TempFileOptions{.durability=TempDurability::file_and_directory});
REQUIRE(r.has_value());
CHECK(dir.read_file("durable.txt")=="synced");
}
TEST_CASE(
"file_io_sync: create_new fails if target exists",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
dir.write_file("exists.txt","present");
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"exists.txt"},SV{"replace"},
TempFileOptions{},TempPublishMode::create_new);
CHECK(!r.has_value());
CHECK(dir.file_exists("exists.txt"));
CHECK(dir.read_file("exists.txt")=="present");
}
TEST_CASE(
"file_io_sync: create_new succeeds for new file",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"brand_new.txt"},SV{"fresh"},
TempFileOptions{},TempPublishMode::create_new);
REQUIRE(r.has_value());
CHECK(dir.read_file("brand_new.txt")=="fresh");
}
TEST_CASE(
"file_io_sync: durability none skips fsync",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
auto r=write_text_file_atomic_at_sync(dir.fd,SV{"fast.txt"},SV{"no sync"},
TempFileOptions{.durability=TempDurability::none});
REQUIRE(r.has_value());
CHECK(dir.read_file("fast.txt")=="no sync");
}
TEST_CASE(
"file_io_sync: empty and dot paths rejected",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
CHECK(!write_text_file_atomic_at_sync(dir.fd,SV{""},SV{"data"}).has_value());
CHECK(!write_text_file_atomic_at_sync(dir.fd,SV{"."},SV{"data"}).has_value());
CHECK(!write_text_file_atomic_at_sync(dir.fd,SV{".."},SV{"data"}).has_value());
}
TEST_CASE(
"file_io_sync: binary write_file_atomic_at_sync round-trips bytes",
"[file_io_sync][unit]"){
auto dir=TempDir::create();
A<byte,4>bytes{byte{0xDE},byte{0xAD},byte{0xBE},byte{0xEF}};
auto r=write_file_atomic_at_sync(dir.fd,SV{"binary.bin"},span{bytes});
REQUIRE(r.has_value());
auto content=dir.read_file("binary.bin");
REQUIRE(content.size()==4);
CHECK(static_cast<u8>(content[0])==0xDE);
CHECK(static_cast<u8>(content[1])==0xAD);
CHECK(static_cast<u8>(content[2])==0xBE);
CHECK(static_cast<u8>(content[3])==0xEF);
}
