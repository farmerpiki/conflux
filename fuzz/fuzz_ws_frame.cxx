// libFuzzer driver for ws_detail::parse_frame_header.
// Invariants:
//   Ok → header_size ≤ buf.size(); 0 ≤ header_size ≤ 14; payload_len fits uint64
//   Incomplete/ProtocolError/ControlTooLarge → must not crash, must not read past buf

import std;
import conflux.types;
import conflux.net.router;

using namespace std;
extern "C" int LLVMFuzzerTestOneInput(
u8 const*data,
SZ size){
ws_detail::FrameHeader hdr{};
auto const st=ws_detail::parse_frame_header(as_bytes(span{data,size}),hdr);
if(st==ws_detail::FrameParseStatus::Ok){
if(hdr.header_size>size)
__builtin_trap();
if(hdr.header_size>14)
__builtin_trap();
if(hdr.header_size<6)
__builtin_trap();
}
return 0;
}
