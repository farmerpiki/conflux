// libFuzzer driver for decode_chunked.
// Invariants:
//   return > 0 → body populated, size ≤ max_body_size, bytes consumed ≤ input
//   return 0   → incomplete
//   return -1/-2 → malformed / oversize (must not crash, must not touch `body`)

import std;
import conflux.types;
import conflux.net.http_server;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size < 2) {
		return 0;
	}

	// First two bytes pick (max_body_size, max_chunks) caps within sane bounds
	// so the fuzzer explores boundary behaviour without OOMing.
	auto const mb_choice = data[0];
	auto const mc_choice = data[1];
	SZ const max_body = 1U << (mb_choice % 20U); // 1..1M
	SZ const max_chunks = 1U << (mc_choice % 14U); // 1..8192

	SV input{reinterpret_cast<char const *>(data + 2), size - 2};
	S body;
	auto const rc = decode_chunked(input, max_body, max_chunks, body);

	if (rc > 0) {
		if (static_cast<SZ>(rc) > input.size()) {
			__builtin_trap();
		}
		if (body.size() > max_body) {
			__builtin_trap();
		}
	} else if (rc != 0 && rc != -1 && rc != -2) {
		__builtin_trap();
	}
	return 0;
}
