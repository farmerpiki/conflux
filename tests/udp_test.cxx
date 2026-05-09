// Plain TU — not a module unit.
#include<arpa/inet.h>
#include<liburing.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>

#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.file_io;
import conflux.work;
import conflux.socket_io.coro;

using namespace std;
namespace{
constexpr uint64_t pack_ud(
uint32_t slot,
uint32_t gen)noexcept{
return(static_cast<uint64_t>(gen)<<32U)|slot;
}
struct RingFixture{
::io_uring ring{};
CompletionTable completions{};
FileReader reader;
bool ring_ok{false};
RingFixture()
:reader{&ring,&completions,[](uint32_t slot,uint32_t gen)noexcept{return pack_ud(slot,gen);}}{}
static unique_ptr<RingFixture>make(
unsigned entries=64){
auto fx=make_unique<RingFixture>();
if(::io_uring_queue_init(entries,&fx->ring,0)<0)
return{};
fx->ring_ok=true;
return fx;
}
~RingFixture(){
if(ring_ok)
::io_uring_queue_exit(&ring);
}
RingFixture(RingFixture const&)=delete;
RingFixture&operator=(RingFixture const&)=delete;
RingFixture(RingFixture&&)=delete;
RingFixture&operator=(RingFixture&&)=delete;
template<typename T>
T run(
conflux::work::root::Task<T>task,
chrono::milliseconds budget=chrono::seconds{5}){
return block_on(reader,move(task),budget);
}
};
unique_ptr<RingFixture>require_ring_fixture(
unsigned entries=64){
auto fx=RingFixture::make(entries);
INFO("conflux requires a host that permits io_uring_queue_init");
REQUIRE(fx!=nullptr);
return fx;
}
uint16_t get_local_port(
int fd)noexcept{
sockaddr_storage ss{};
socklen_t len=sizeof(ss);
if(::getsockname(fd,reinterpret_cast<sockaddr*>(&ss),&len)<0)
return 0;
if(ss.ss_family==AF_INET)
return ntohs(reinterpret_cast<sockaddr_in const*>(&ss)->sin_port);
if(ss.ss_family==AF_INET6)
return ntohs(reinterpret_cast<sockaddr_in6 const*>(&ss)->sin6_port);
return 0;
}
sockaddr_storage to_storage(
sockaddr_in const&sa)noexcept{
sockaddr_storage ss{};
memcpy(&ss,&sa,sizeof(sa));
return ss;
}
}// namespace
// ---------------------------------------------------------------------------
// UdpSocket — RAII / ephemeral / family / valid
// ---------------------------------------------------------------------------

TEST_CASE(
"udp: ephemeral binds to non-zero local port",
"[udp]"){
auto fx=require_ring_fixture();
auto sock=UdpSocket::ephemeral(fx->reader,AF_INET);
CHECK(sock.valid());
CHECK(sock.family()==AF_INET);
CHECK(sock.raw_fd()>=0);
CHECK(get_local_port(sock.raw_fd())>0);
}
TEST_CASE(
"udp: ephemeral works for AF_INET6",
"[udp]"){
auto fx=require_ring_fixture();
auto sock=UdpSocket::ephemeral(fx->reader,AF_INET6);
CHECK(sock.valid());
CHECK(sock.family()==AF_INET6);
CHECK(get_local_port(sock.raw_fd())>0);
}
TEST_CASE(
"udp: UdpSocket move-only — source becomes empty",
"[udp]"){
auto fx=require_ring_fixture();
auto src=UdpSocket::ephemeral(fx->reader,AF_INET);
int const fd=src.raw_fd();
REQUIRE(fd>=0);
auto dst=move(src);
CHECK_FALSE(src.valid());
CHECK(dst.raw_fd()==fd);
}
// ---------------------------------------------------------------------------
// send_to + recv_from — loopback round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"udp: send and recv on loopback (AF_INET)",
"[udp][uring]"){
auto fx=require_ring_fixture();

auto recv_sock=UdpSocket::ephemeral(fx->reader,AF_INET);
auto send_sock=UdpSocket::ephemeral(fx->reader,AF_INET);

uint16_t const recv_port=get_local_port(recv_sock.raw_fd());
REQUIRE(recv_port>0);

A<uint8_t,5>payload{'h','e','l','l','o'};
sockaddr_in dest{};
dest.sin_family=AF_INET;
dest.sin_port=htons(recv_port);
dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
socklen_t const dest_len=sizeof(dest);

// Send first; kernel buffers the datagram.
auto const bytes_sent=fx->run(send_sock.send_to(
span<uint8_t const>{payload.data(),payload.size()},
to_storage(dest),dest_len));
CHECK(bytes_sent==payload.size());

// Recv now — packet is already in the kernel buffer.
A<uint8_t,256>rx_buf{};
auto const rx=fx->run(recv_sock.recv_from(span<uint8_t>{rx_buf.data(),rx_buf.size()}));

REQUIRE(rx.bytes==payload.size());
CHECK(memcmp(rx_buf.data(),payload.data(),rx.bytes)==0);
CHECK(rx.from_len>=sizeof(::sockaddr_in));
auto const&from=*reinterpret_cast<::sockaddr_in const*>(&rx.from);
CHECK(from.sin_family==AF_INET);
CHECK(from.sin_addr.s_addr==htonl(INADDR_LOOPBACK));
CHECK(ntohs(from.sin_port)==get_local_port(send_sock.raw_fd()));
}
// ---------------------------------------------------------------------------
// recv_from with timeout — fires on idle socket
// ---------------------------------------------------------------------------

TEST_CASE(
"udp: recv_from with timeout fires FileIoError on idle socket",
"[udp][uring]"){
auto fx=require_ring_fixture();

auto sock=UdpSocket::ephemeral(fx->reader,AF_INET);

A<uint8_t,256>rx_buf{};
int err_code=0;
bool got_value=false;
try{
fx->run(
sock.recv_from(
span<uint8_t>{rx_buf.data(),rx_buf.size()},
chrono::milliseconds{50}),
chrono::seconds{2});
got_value=true;
}catch(FileIoError const&e){err_code=e.code().value();}catch(...){
}

CHECK_FALSE(got_value);
CHECK(err_code==ETIMEDOUT);
}
// ---------------------------------------------------------------------------
// recv_from with timeout — succeeds when data arrives in time
// ---------------------------------------------------------------------------

TEST_CASE(
"udp: recv_from with timeout receives packet that arrives before timeout",
"[udp][uring]"){
auto fx=require_ring_fixture();

auto recv_sock=UdpSocket::ephemeral(fx->reader,AF_INET);
auto send_sock=UdpSocket::ephemeral(fx->reader,AF_INET);

uint16_t const recv_port=get_local_port(recv_sock.raw_fd());

A<uint8_t,4>payload{0xDE,0xAD,0xBE,0xEF};
sockaddr_in dest{};
dest.sin_family=AF_INET;
dest.sin_port=htons(recv_port);
dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
socklen_t const dest_len=sizeof(dest);

// Send first; kernel buffers the datagram.
fx->run(send_sock.send_to(
span<uint8_t const>{payload.data(),payload.size()},
to_storage(dest),dest_len));

A<uint8_t,256>rx_buf{};
auto const rx=fx->run(
recv_sock.recv_from(
span<uint8_t>{rx_buf.data(),rx_buf.size()},
chrono::milliseconds{2000}),
chrono::seconds{3});

REQUIRE(rx.bytes==payload.size());
CHECK(memcmp(rx_buf.data(),payload.data(),rx.bytes)==0);
}
