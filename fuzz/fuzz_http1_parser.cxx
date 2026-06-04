// libFuzzer driver for conflux::http1::parse_request.
// Invariants:
//   Ok    → method/target/version are slices of input; header_end_offset within input
//   all outputs (method, target, version, header names/values) must be slices of input
//   must not crash on any byte sequence

import std;
import conflux.types;
import conflux.net.http1_parser;
import conflux.net.config;

using namespace std;
using namespace conflux::http;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size < 1) {
		return 0;
	}

	ParserLimits limits;
	limits.max_request_line_size = 1U << ((data[0] & 0x0FU) + 4U); // 16..512k
	limits.max_header_line_size = 1U << ((data[0] >> 4U) + 4U); // 16..2M
	limits.max_headers = 64;
	limits.max_header_block_size = 16U * 1024U;

	std::string_view input{reinterpret_cast<char const *>(data + 1), size - 1};

	conflux::http1::ParsedRequest out;
	auto const st = conflux::http1::parse_request(input, limits, out);
	if (st != conflux::http1::ParseStatus::Ok) {
		return 0;
	}

	auto const *base = input.data();
	auto const *end = base + input.size();
	auto within = [&](std::string_view sv) {
		if (sv.empty()) {
			return true;
		}
		return sv.data() >= base && sv.data() + sv.size() <= end;
	};

	if (!within(out.method) || !within(out.target) || !within(out.version)) {
		__builtin_trap();
	}
	if (out.header_end_offset + 4 > input.size()) {
		__builtin_trap();
	}
	for (auto const &[n, v]: out.headers) {
		if (!within(n) || !within(v)) {
			__builtin_trap();
		}
		if (n.empty()) {
			__builtin_trap();
		}
	}
	return 0;
}
