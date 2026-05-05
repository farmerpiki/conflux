// conflux.db basic example.
//
// Connects to PostgreSQL via PG_CONNINFO, runs a SELECT, prints rows.
// Uses file_io's block_on helper to drive a single-thread io_uring.
#include<liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db;

using namespace conflux::db;
namespace{
constexpr u64 pack_ud(
u32 slot,
u32 gen)noexcept{
return(static_cast<u64>(gen)<<32U)|slot;
}
}// namespace
int main(){
char const*raw=std::getenv("PG_CONNINFO");
if(raw==nullptr||*raw=='\0'){
println(cerr,"set PG_CONNINFO, e.g. host=/var/run/postgresql user=postgres dbname=postgres");
return 2;
}

::io_uring ring{};
if(::io_uring_queue_init(64,&ring,0)<0){
println(cerr,"io_uring_queue_init failed");
return 1;
}
CompletionTable ct;
FileReader reader{&ring,&ct,[](u32 s,u32 g)noexcept{return pack_ud(s,g);}};
CurrentFileReaderScope const scope{&reader};

try{
auto conn=block_on(reader,Connection::connect({.conninfo=raw}));
println("connected — backend pid {}, server {}",conn->backend_pid(),conn->server_version());

Params p;
p.add(i64{3});
auto rs=
block_on(reader,conn->query("SELECT i, 'row #' || i AS label FROM generate_series(1,$1) AS i",move(p)));
println("rows: {} cols: {}",rs.rows(),rs.cols());
for(auto row:rs)
println("  {} = {}",row.as<i64>(0),row.as<SV>(1));
conn->close();
}catch(exception const&e){
println(cerr,"error: {}",e.what());
::io_uring_queue_exit(&ring);
return 1;
}

::io_uring_queue_exit(&ring);
}
