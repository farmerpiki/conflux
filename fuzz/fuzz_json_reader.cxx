import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;
extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size == 0) {
		return 0;
	}

	SV input{reinterpret_cast<char const *>(data), size};

	JsonParseOptions opts{.max_depth = LimitOption::bound(256)};
	JsonReader reader{input, opts};

	for (;;) {
		auto ev_or = reader.next();
		if (!ev_or) {
			if (ev_or.error().message.empty()) {
				__builtin_trap();
			}
			break;
		}
		if (!*ev_or) {
			break;
		}
		using Ev = JsonReader::Event;
		Ev ev = **ev_or;
		switch (ev) {
		case Ev::key:
		case Ev::string_value:
			{
				S decoded;
				auto tok = (ev == Ev::key) ? reader.key_token() : reader.string_token();
				auto _ = tok.append_decoded_to(decoded);
				break;
			}
		case Ev::number_value:
			{
				auto i = reader.number_val().to_i64();
				auto d = reader.number_val().to_f64();
				(void)i;
				(void)d;
				break;
			}
		case Ev::bool_value:
			{
				auto _ = reader.bool_val();
				break;
			}
		default: break;
		}
	}

	JsonStreamReader stream{opts};
	auto drain_stream = [&]() {
		for (;;) {
			auto sev_or = stream.next();
			if (!sev_or) {
				if (sev_or.error().message.empty()) {
					__builtin_trap();
				}
				return;
			}
			if (!*sev_or) {
				return;
			}
			using Ev = JsonStreamReader::Event;
			Ev const sev = **sev_or;
			switch (sev) {
			case Ev::key:
			case Ev::string_value:
				{
					S decoded;
					auto tok = (sev == Ev::key) ? stream.key_token() : stream.string_token();
					auto decoded_or = tok.append_decoded_to(decoded);
					if (!decoded_or && decoded_or.error().message.empty()) {
						__builtin_trap();
					}
					break;
				}
			case Ev::number_value:
				{
					auto i = stream.number_val().to_i64();
					auto d = stream.number_val().to_f64();
					(void)i;
					(void)d;
					break;
				}
			case Ev::bool_value:
				{
					auto b = stream.bool_val();
					(void)b;
					break;
				}
			default: break;
			}
		}
	};
	SZ const stride = 1 + static_cast<SZ>(data[0] & 7U);
	for (SZ off = 0; off < size;) {
		SZ const n = min(stride, size - off);
		auto fed = stream.feed(span<byte const>{reinterpret_cast<byte const *>(data + off), n});
		if (!fed) {
			if (fed.error().message.empty()) {
				__builtin_trap();
			}
			break;
		}
		off += n;
		drain_stream();
	}
	if (auto closed = stream.close(); closed) {
		drain_stream();
	}
	return 0;
}
