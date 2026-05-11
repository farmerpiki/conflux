module;
#include <cstddef> // must precede nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" block in nghttp2.h re-includes c++config.h
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

export module conflux.net.http2;
import std;
import std.compat;
import conflux.types;

// HTTP/2 ALPN protocol identifier.
export inline SV const kH2Alpn = "h2";
// Configure an SSL_CTX to advertise HTTP/2 via ALPN.
// Call this after creating the SSL_CTX in HttpServer when HTTP/2 is desired.
export void http2_configure_alpn(
	SSL_CTX *ctx) {
	// Server-side ALPN callback: prefer h2, fall back to http/1.1.
	// Uses SSL_select_next_proto which handles NPN wire-format length prefixes
	// correctly. nghttp2_select_next_protocol only matches h2/h2-14 and leaves
	// *out undefined on no-overlap, corrupting non-h2 handshakes.
	SSL_CTX_set_alpn_select_cb(
		ctx,
		[](SSL * /*ssl*/,
		   unsigned char const **out,
		   unsigned char *outlen,
		   unsigned char const *in,
		   unsigned int inlen,
		   void * /*arg*/) -> int {
			static constexpr unsigned char kServerProtos[] = "\x02h2\x08http/1.1";
			if (SSL_select_next_proto(
					const_cast<unsigned char **>(out),
					outlen, // NOLINT(cppcoreguidelines-pro-type-const-cast)
					kServerProtos,
					sizeof(kServerProtos) - 1,
					in,
					inlen)
				== OPENSSL_NPN_NEGOTIATED) {
				return SSL_TLSEXT_ERR_OK;
			}
			return SSL_TLSEXT_ERR_NOACK;
		},
		nullptr);
}
// Returns true if the negotiated protocol on this SSL connection is h2.
export bool http2_negotiated(
	SSL const *ssl) {
	unsigned char const *proto{};
	unsigned int proto_len{};
	SSL_get0_alpn_selected(ssl, &proto, &proto_len);
	return proto != nullptr && proto_len == kH2Alpn.size() && memcmp(proto, kH2Alpn.data(), proto_len) == 0;
}
