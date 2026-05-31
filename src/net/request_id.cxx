export module conflux.net.request_id;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;
export struct RequestIdOptions {
	// Header to read/write the request ID on.
	std::string header{"X-Request-ID"};

	// If true and the client sends the header, echo it through unchanged.
	// If false, always generate a fresh ID regardless.
	bool trust_incoming{true};
};
namespace request_id_detail {

// Generate a UUID v4 (random) as a hex std::string without dashes.
// Uses /dev/urandom — no dependency on OpenSSL.
std::string generate_uuid() {
	std::array<unsigned char, 16> bytes{};
	random_bytes(bytes);

	// Set UUID v4 version and std::variant bits.
	bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
	bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // std::variant 1

	// Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(36);
	for (std::size_t i = 0; i < 16; ++i) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			out += '-';
		}
		out += kHex[bytes[i] >> 4];
		out += kHex[bytes[i] & 0xF];
	}
	return out;
}

} // namespace request_id_detail
// Middleware factory: stamp X-Request-ID (or configured header) on every
// request/response. Echoes an existing header from the client when
// trust_incoming is true; generates a UUID v4 otherwise.
export conflux::http::Router::Middleware request_id_middleware(
	RequestIdOptions opts = {}) {
	// Lowercase header name for lookup in req.headers (keys are lowercased).
	std::string lower_header = ascii_lower(opts.header);

	return [opts = std::move(opts), lower_header = std::move(lower_header)](
			   RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		std::string id;
		if (opts.trust_incoming) {
			auto existing = req.headers[lower_header];
			if (!existing.empty()) {
				id = std::string{existing};
			}
		}
		if (id.empty()) {
			id = request_id_detail::generate_uuid();
		}

		// Inject the ID into the request so downstream handlers can read it.
		auto enriched = req.to_owned();
		enriched.headers[lower_header] = id;

		auto resp = next(enriched);
		resp.headers[opts.header] = id;
		return resp;
	};
}
