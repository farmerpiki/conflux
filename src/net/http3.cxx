module;
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef> // before openssl: establishes C++ linkage for __is_constant_evaluated
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

export module conflux.net.http3;
import std;
import conflux.types;
import std.compat;
import conflux.net.config;
import conflux.net.router;

export inline SV const kH3Alpn = "h3";

export S http3_alt_svc_value(
	u16 port,
	u32 max_age_sec) {
	return format(R"(h3=":{}"; ma={})", port, max_age_sec);
}

export bool http3_negotiated(
	SSL const *ssl) {
	unsigned char const *proto{};
	unsigned int proto_len{};
	SSL_get0_alpn_selected(ssl, &proto, &proto_len);
	return proto != nullptr && proto_len == kH3Alpn.size() && memcmp(proto, kH3Alpn.data(), proto_len) == 0;
}

export void http3_configure_alpn(
	SSL_CTX *ctx) {
	SSL_CTX_set_alpn_select_cb(
		ctx,
		[](SSL * /*ssl*/,
		   unsigned char const **out,
		   unsigned char *outlen,
		   unsigned char const *in,
		   unsigned int inlen,
		   void * /*arg*/) -> int {
			static constexpr unsigned char kServerProtos[] = "\x02h3\x02h2\x08http/1.1";
			if (SSL_select_next_proto(
					const_cast<unsigned char **>(out),
					outlen,
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

namespace http3_detail {

constexpr SZ kMaxUdpPayload = 1500;
constexpr SZ kCidLen = 16;

u64 now_ns() {
	auto const t = chrono::steady_clock::now().time_since_epoch();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t).count());
}

struct Http3Stream {
	i64 stream_id{};
	S method;
	S path;
	S authority;
	S scheme;
	V<P<S, S>> headers;
	S body;
	bool request_complete{false};
	bool response_submitted{false};
	HttpResponse response{};
	S response_body_buf;
	S status_str;
	S content_length_str;
	SZ response_body_offset{};
	bool response_eof{false};
};

struct Http3Conn {
	ngtcp2_conn *conn{nullptr};
	nghttp3_conn *h3conn{nullptr};
	SSL *ssl{nullptr};
	ngtcp2_crypto_ossl_ctx *ossl_ctx{nullptr};
	ngtcp2_crypto_conn_ref conn_ref{};
	sockaddr_storage remote_addr{};
	socklen_t remote_addrlen{};
	sockaddr_storage local_addr{};
	socklen_t local_addrlen{};
	A<u8, kCidLen> scid_key{};
	V<A<u8, kCidLen>> cid_keys{};
	UM<i64, UP<Http3Stream>> streams{};
	Router const *router{nullptr};
	void *listener{nullptr};
	bool closing{false};
	bool closed{false};
	bool handshake_done{false};
	A<u8, 16> ip_key{};
	SZ max_body_size{0};
};

void register_cid_on_listener(Http3Conn *c, A<u8, kCidLen> const &key);
void unregister_cid_on_listener(Http3Conn *c, A<u8, kCidLen> const &key);

ngtcp2_conn *crypto_conn_ref_get_conn(
	ngtcp2_crypto_conn_ref *ref) {
	return static_cast<Http3Conn *>(ref->user_data)->conn;
}

void rand_cb(
	u8 *dest,
	SZ destlen,
	ngtcp2_rand_ctx const * /*rand_ctx*/) {
	RAND_bytes(dest, static_cast<int>(destlen));
}

int get_new_connection_id_cb(
	ngtcp2_conn * /*conn*/,
	ngtcp2_cid *cid,
	u8 *token,
	SZ cidlen,
	void *user_data) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	cid->datalen = cidlen;
	if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	A<u8, kCidLen> k{};
	memcpy(k.data(), cid->data, min(cidlen, k.size()));
	c->cid_keys.push_back(k);
	register_cid_on_listener(c, k);
	return 0;
}

int remove_connection_id_cb(
	ngtcp2_conn * /*conn*/,
	ngtcp2_cid const *cid,
	void *user_data) {
	auto *c = static_cast<Http3Conn *>(user_data);
	A<u8, kCidLen> k{};
	memcpy(k.data(), cid->data, min<SZ>(cid->datalen, k.size()));
	unregister_cid_on_listener(c, k);
	return 0;
}

int stream_open_cb(
	ngtcp2_conn * /*conn*/,
	i64 /*stream_id*/,
	void * /*user_data*/) {
	return 0;
}

int acked_stream_data_offset_cb(
	ngtcp2_conn * /*conn*/,
	i64 stream_id,
	u64 /*offset*/,
	u64 datalen,
	void *user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (c->h3conn != nullptr) {
		(void)nghttp3_conn_add_ack_offset(c->h3conn, stream_id, datalen);
	}
	return 0;
}

int extend_max_local_streams_bidi_cb(
	ngtcp2_conn * /*conn*/,
	u64 /*max_streams*/,
	void * /*user_data*/) {
	return 0;
}

int stream_reset_cb(
	ngtcp2_conn * /*conn*/,
	i64 stream_id,
	u64 /*final_size*/,
	u64 /*app_error_code*/,
	void *user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (c->h3conn != nullptr) {
		(void)nghttp3_conn_shutdown_stream_read(c->h3conn, stream_id);
	}
	return 0;
}

int stream_stop_sending_cb(
	ngtcp2_conn * /*conn*/,
	i64 stream_id,
	u64 /*app_error_code*/,
	void *user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (c->h3conn != nullptr) {
		(void)nghttp3_conn_shutdown_stream_read(c->h3conn, stream_id);
	}
	return 0;
}

void dispatch_stream(Http3Conn *c, Http3Stream &s);

int h3_recv_data_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	u8 const *data,
	SZ datalen,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	auto it = c->streams.find(stream_id);
	if (it == c->streams.end()) {
		return 0;
	}
	auto &s = *it->second;
	if (c->max_body_size != 0 && s.body.size() + datalen > c->max_body_size) {
		ngtcp2_conn_shutdown_stream(c->conn, 0, stream_id, NGHTTP3_H3_REQUEST_REJECTED);
		return 0;
	}
	s.body.append(reinterpret_cast<char const *>(data), datalen);
	return 0;
}

int h3_recv_header_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	i32 /*token*/,
	nghttp3_rcbuf *name,
	nghttp3_rcbuf *value,
	u8 /*flags*/,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	auto it = c->streams.find(stream_id);
	if (it == c->streams.end()) {
		return 0;
	}
	nghttp3_vec const nv = nghttp3_rcbuf_get_buf(name);
	nghttp3_vec const vv = nghttp3_rcbuf_get_buf(value);
	SV const n{reinterpret_cast<char const *>(nv.base), nv.len};
	SV const v{reinterpret_cast<char const *>(vv.base), vv.len};
	auto &s = *it->second;
	if (n == ":method") {
		s.method = S{v};
	} else if (n == ":path") {
		s.path = S{v};
	} else if (n == ":authority") {
		s.authority = S{v};
	} else if (n == ":scheme") {
		s.scheme = S{v};
	} else {
		s.headers.emplace_back(S{n}, S{v});
	}
	return 0;
}

int h3_begin_headers_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	if (c->streams.count(stream_id) == 0) {
		auto s = make_unique<Http3Stream>();
		s->stream_id = stream_id;
		c->streams.emplace(stream_id, move(s));
	}
	return 0;
}

int h3_end_headers_cb(
	nghttp3_conn * /*conn*/,
	i64 /*stream_id*/,
	int /*fin*/,
	void * /*conn_user_data*/,
	void * /*stream_user_data*/) {
	return 0;
}

int h3_end_stream_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	auto it = c->streams.find(stream_id);
	if (it == c->streams.end()) {
		return 0;
	}
	it->second->request_complete = true;
	dispatch_stream(c, *it->second);
	return 0;
}

int h3_stream_close_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	u64 /*app_error_code*/,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	c->streams.erase(stream_id);
	return 0;
}

int h3_acked_stream_data_cb(
	nghttp3_conn * /*conn*/,
	i64 /*stream_id*/,
	u64 /*datalen*/,
	void * /*conn_user_data*/,
	void * /*stream_user_data*/) {
	return 0;
}

int h3_stop_sending_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	u64 app_error_code,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	(void)ngtcp2_conn_shutdown_stream_read(c->conn, 0, stream_id, app_error_code);
	return 0;
}

int h3_reset_stream_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	u64 app_error_code,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	(void)ngtcp2_conn_shutdown_stream_write(c->conn, 0, stream_id, app_error_code);
	return 0;
}

nghttp3_ssize h3_read_response_body_cb(
	nghttp3_conn * /*conn*/,
	i64 stream_id,
	nghttp3_vec *vec,
	SZ veccnt,
	u32 *pflags,
	void *conn_user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(conn_user_data);
	auto it = c->streams.find(stream_id);
	if (it == c->streams.end()) {
		*pflags |= NGHTTP3_DATA_FLAG_EOF;
		return 0;
	}
	auto &s = *it->second;
	if (veccnt == 0) {
		return 0;
	}
	SZ const remaining = s.response_body_buf.size() - s.response_body_offset;
	if (remaining == 0) {
		*pflags |= NGHTTP3_DATA_FLAG_EOF;
		s.response_eof = true;
		return 0;
	}
	vec[0].base = reinterpret_cast<u8 *>(s.response_body_buf.data() + s.response_body_offset);
	vec[0].len = remaining;
	s.response_body_offset += remaining;
	*pflags |= NGHTTP3_DATA_FLAG_EOF;
	s.response_eof = true;
	return 1;
}

int recv_stream_data_cb(
	ngtcp2_conn * /*conn*/,
	u32 flags,
	i64 stream_id,
	u64 /*offset*/,
	u8 const *data,
	SZ datalen,
	void *user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (c->h3conn == nullptr) {
		return 0;
	}
	int const fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0 ? 1 : 0;
	nghttp3_ssize const n = nghttp3_conn_read_stream(c->h3conn, stream_id, data, datalen, fin);
	if (n < 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	ngtcp2_conn_extend_max_stream_offset(c->conn, stream_id, static_cast<u64>(n));
	ngtcp2_conn_extend_max_offset(c->conn, static_cast<u64>(n));
	return 0;
}

int stream_close_cb(
	ngtcp2_conn * /*conn*/,
	u32 flags,
	i64 stream_id,
	u64 app_error_code,
	void *user_data,
	void * /*stream_user_data*/) {
	auto *c = static_cast<Http3Conn *>(user_data);
	if (c->h3conn != nullptr) {
		u64 code = app_error_code;
		if ((flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) == 0) {
			code = NGHTTP3_H3_NO_ERROR;
		}
		(void)nghttp3_conn_close_stream(c->h3conn, stream_id, code);
	}
	c->streams.erase(stream_id);
	return 0;
}

int handshake_completed_cb(
	ngtcp2_conn * /*conn*/,
	void *user_data) {
	auto *c = static_cast<Http3Conn *>(user_data);
	c->handshake_done = true;
	nghttp3_settings settings;
	nghttp3_settings_default(&settings);
	nghttp3_callbacks cbs{};
	cbs.stream_close = h3_stream_close_cb;
	cbs.recv_data = h3_recv_data_cb;
	cbs.recv_header = h3_recv_header_cb;
	cbs.begin_headers = h3_begin_headers_cb;
	cbs.end_headers = h3_end_headers_cb;
	cbs.end_stream = h3_end_stream_cb;
	cbs.acked_stream_data = h3_acked_stream_data_cb;
	cbs.stop_sending = h3_stop_sending_cb;
	cbs.reset_stream = h3_reset_stream_cb;
	if (nghttp3_conn_server_new(&c->h3conn, &cbs, &settings, nullptr, c) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	i64 ctrl_id{};
	i64 qenc_id{};
	i64 qdec_id{};
	if (ngtcp2_conn_open_uni_stream(c->conn, &ctrl_id, nullptr) != 0
		|| ngtcp2_conn_open_uni_stream(c->conn, &qenc_id, nullptr) != 0
		|| ngtcp2_conn_open_uni_stream(c->conn, &qdec_id, nullptr) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	if (nghttp3_conn_bind_control_stream(c->h3conn, ctrl_id) != 0
		|| nghttp3_conn_bind_qpack_streams(c->h3conn, qenc_id, qdec_id) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
}

void fill_callbacks(
	ngtcp2_callbacks &cb) {
	cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
	cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
	cb.encrypt = ngtcp2_crypto_encrypt_cb;
	cb.decrypt = ngtcp2_crypto_decrypt_cb;
	cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
	cb.recv_stream_data = recv_stream_data_cb;
	cb.acked_stream_data_offset = acked_stream_data_offset_cb;
	cb.stream_open = stream_open_cb;
	cb.stream_close = stream_close_cb;
	cb.rand = rand_cb;
	cb.get_new_connection_id = get_new_connection_id_cb;
	cb.remove_connection_id = remove_connection_id_cb;
	cb.update_key = ngtcp2_crypto_update_key_cb;
	cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
	cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
	cb.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
	cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
	cb.handshake_completed = handshake_completed_cb;
	cb.extend_max_local_streams_bidi = extend_max_local_streams_bidi_cb;
	cb.stream_reset = stream_reset_cb;
	cb.stream_stop_sending = stream_stop_sending_cb;
}

S addr_to_string(
	sockaddr const *sa) {
	char buf[INET6_ADDRSTRLEN + 16]{};
	if (sa->sa_family == AF_INET) {
		auto const *in = reinterpret_cast<sockaddr_in const *>(sa);
		char ip[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
		(void)snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(in->sin_port));
	} else if (sa->sa_family == AF_INET6) {
		auto const *in6 = reinterpret_cast<sockaddr_in6 const *>(sa);
		char ip[INET6_ADDRSTRLEN]{};
		inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
		(void)snprintf(buf, sizeof(buf), "[%s]:%u", ip, ntohs(in6->sin6_port));
	}
	return S{buf};
}

void dispatch_stream(
	Http3Conn *c,
	Http3Stream &s) {
	if (s.response_submitted) {
		return;
	}
	s.response_submitted = true;
	HttpFieldsView hdrs_view;
	for (auto const &[k, v]: s.headers) {
		hdrs_view.emplace_back(k, v);
	}
	S const remote = addr_to_string(reinterpret_cast<sockaddr const *>(&c->remote_addr));
	HttpRequestView const req{s.method, s.path, "HTTP/3", remote, true, {}, move(hdrs_view), {}, {}, {}, {}, s.body};
	if (c->router == nullptr) {
		s.response = HttpResponse::internal_error("no router");
	} else {
		try {
			s.response = c->router->dispatch(req);
		} catch (exception const &ex) { s.response = HttpResponse::internal_error(ex.what()); } catch (...) {
			s.response = HttpResponse::internal_error();
		}
	}
	if (s.response.is_text()) {
		s.response_body_buf = s.response.take_text_body();
	} else if (s.response.is_mapped_file()) {
		auto const &mf = s.response.mapped_file_ptr();
		if (mf) {
			char const *base = static_cast<char const *>(mf->ptr) + mf->send_offset;
			s.response_body_buf.assign(base, mf->send_size);
		}
	} else {
		s.response.status = 501;
		s.response.status_text = "Not Implemented";
		s.response_body_buf = "HTTP/3 does not support this response kind yet\n";
	}
	V<nghttp3_nv> nva;
	s.status_str = to_string(s.response.status);
	nva.push_back(
		{reinterpret_cast<u8 const *>(":status"),
		 reinterpret_cast<u8 const *>(s.status_str.c_str()),
		 7,
		 s.status_str.size(),
		 NGHTTP3_NV_FLAG_NO_COPY_NAME});
	static constexpr SV kCT = "content-type";
	nva.push_back(
		{reinterpret_cast<u8 const *>(kCT.data()),
		 reinterpret_cast<u8 const *>(s.response.content_type.c_str()),
		 kCT.size(),
		 s.response.content_type.size(),
		 NGHTTP3_NV_FLAG_NO_COPY_NAME});
	s.content_length_str = to_string(s.response_body_buf.size());
	static constexpr SV kCL = "content-length";
	nva.push_back(
		{reinterpret_cast<u8 const *>(kCL.data()),
		 reinterpret_cast<u8 const *>(s.content_length_str.c_str()),
		 kCL.size(),
		 s.content_length_str.size(),
		 NGHTTP3_NV_FLAG_NO_COPY_NAME});
	for (auto const &[k, v]: s.response.headers) {
		nva.push_back(
			{reinterpret_cast<u8 const *>(k.c_str()), reinterpret_cast<u8 const *>(v.c_str()), k.size(), v.size(), 0});
	}
	for (auto const &sc: s.response.set_cookies) {
		static constexpr SV kSC = "set-cookie";
		nva.push_back(
			{reinterpret_cast<u8 const *>(kSC.data()),
			 reinterpret_cast<u8 const *>(sc.c_str()),
			 kSC.size(),
			 sc.size(),
			 NGHTTP3_NV_FLAG_NO_COPY_NAME});
	}
	nghttp3_data_reader dr{};
	dr.read_data = h3_read_response_body_cb;
	(void)nghttp3_conn_submit_response(c->h3conn, s.stream_id, nva.data(), nva.size(), &dr);
}

struct CidHash {
	SZ operator ()(
		A<u8, kCidLen> const &k) const noexcept {
		SZ h = 0xcbf29ce484222325ULL;
		for (auto b: k) {
			h ^= b;
			h *= 0x100000001b3ULL;
		}
		return h;
	}
};

A<u8, kCidLen> cid_to_key(
	u8 const *data,
	SZ datalen) {
	A<u8, kCidLen> k{};
	SZ const copy = min(datalen, k.size());
	memcpy(k.data(), data, copy);
	return k;
}

} // namespace http3_detail

using namespace http3_detail;

export class Http3Listener {
public:
	Http3Listener(
		Router const *router,
		Http3Config const &cfg,
		u16 port,
		SSL_CTX *ssl_ctx)
		: router_(router)
		, cfg_(cfg)
		, port_(port)
		, ssl_ctx_(ssl_ctx) {}

	~Http3Listener() { stop(); }

	Http3Listener(Http3Listener const &) = delete;
	Http3Listener &operator =(Http3Listener const &) = delete;
	Http3Listener(Http3Listener &&) = delete;
	Http3Listener &operator =(Http3Listener &&) = delete;

	void start() {
		SL const lk{stop_mu_};
		if (ngtcp2_crypto_ossl_init() != 0) {
			throw RE{"ngtcp2_crypto_ossl_init failed"};
		}
		if (RAND_bytes(retry_secret_.data(), static_cast<int>(retry_secret_.size())) != 1) {
			throw RE{"RAND_bytes(retry_secret) failed"};
		}
		shutdown_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (shutdown_efd_ < 0) {
			throw RE{"eventfd(h3 shutdown) failed"};
		}
		timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (timer_fd_ < 0) {
			::close(shutdown_efd_);
			shutdown_efd_ = -1;
			throw RE{"timerfd_create(h3) failed"};
		}
		udp_fd_ = ::socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
		if (udp_fd_ < 0) {
			::close(timer_fd_);
			::close(shutdown_efd_);
			timer_fd_ = -1;
			shutdown_efd_ = -1;
			throw RE{"socket(AF_INET6 UDP) failed"};
		}
		int on = 1;
		(void)setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		(void)setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
		int off = 0;
		(void)setsockopt(udp_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
		sockaddr_in6 bind_addr{};
		bind_addr.sin6_family = AF_INET6;
		bind_addr.sin6_port = htons(port_);
		bind_addr.sin6_addr = in6addr_any;
		if (::bind(udp_fd_, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
			int const e = errno;
			::close(udp_fd_);
			::close(timer_fd_);
			::close(shutdown_efd_);
			udp_fd_ = timer_fd_ = shutdown_efd_ = -1;
			throw RE{format("bind(h3 UDP :{}) failed: {}", port_, strerror(e))};
		}
		stopping_.clear(memory_order_release);
		thread_ = thread{[this] { run_loop(); }};
	}

	void stop() {
		SL const lk{stop_mu_};
		if (shutdown_efd_ >= 0 && !stopping_.test_and_set()) {
			u64 v = 1;
			(void)::write(shutdown_efd_, &v, sizeof(v));
		}
		if (thread_.joinable()) {
			thread_.join();
		}
		if (udp_fd_ >= 0) {
			::close(udp_fd_);
			udp_fd_ = -1;
		}
		if (timer_fd_ >= 0) {
			::close(timer_fd_);
			timer_fd_ = -1;
		}
		if (shutdown_efd_ >= 0) {
			::close(shutdown_efd_);
			shutdown_efd_ = -1;
		}
	}

private:
	void run_loop() {
		pollfd pfds[3]{};
		while (!stopping_.test(memory_order_acquire)) {
			pfds[0] = {udp_fd_, POLLIN, 0};
			pfds[1] = {shutdown_efd_, POLLIN, 0};
			pfds[2] = {timer_fd_, POLLIN, 0};
			int const timeout_ms = compute_poll_timeout_ms();
			int const rv = ::poll(pfds, 3, timeout_ms);
			if (rv < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			if ((pfds[1].revents & POLLIN) != 0) {
				break;
			}
			if ((pfds[2].revents & POLLIN) != 0) {
				u64 expirations{};
				(void)::read(timer_fd_, &expirations, sizeof(expirations));
			}
			if ((pfds[0].revents & POLLIN) != 0) {
				drain_udp();
			}
			process_expirations();
			flush_all();
			arm_earliest_timer();
			gc_closed_conns();
		}
		drain_close_all();
	}

	static A<u8, 16> remote_ip_key(
		sockaddr_storage const &ss) {
		A<u8, 16> key{};
		if (ss.ss_family == AF_INET6) {
			memcpy(key.data(), &reinterpret_cast<sockaddr_in6 const &>(ss).sin6_addr, 16);
		} else if (ss.ss_family == AF_INET) {
			key[10] = 0xff;
			key[11] = 0xff;
			memcpy(key.data() + 12, &reinterpret_cast<sockaddr_in const &>(ss).sin_addr, 4);
		}
		return key;
	}

	[[nodiscard]] int compute_poll_timeout_ms() const {
		u64 const now = now_ns();
		u64 earliest = UINT64_MAX;
		for (auto const &[k, c]: conns_) {
			u64 const exp = ngtcp2_conn_get_expiry(c->conn);
			if (exp < earliest) {
				earliest = exp;
			}
		}
		if (earliest == UINT64_MAX) {
			return -1;
		}
		if (earliest <= now) {
			return 0;
		}
		u64 const delta = earliest - now;
		u64 const ms = delta / 1'000'000ULL;
		return static_cast<int>(min<u64>(ms, 1'000ULL));
	}

	void arm_earliest_timer() {
		u64 earliest = UINT64_MAX;
		for (auto const &[k, c]: conns_) {
			u64 const exp = ngtcp2_conn_get_expiry(c->conn);
			if (exp < earliest) {
				earliest = exp;
			}
		}
		itimerspec ts{};
		if (earliest == UINT64_MAX) {
			(void)timerfd_settime(timer_fd_, 0, &ts, nullptr);
			return;
		}
		u64 const now = now_ns();
		u64 const delta = earliest > now ? earliest - now : 1;
		ts.it_value.tv_sec = static_cast<time_t>(delta / 1'000'000'000ULL);
		ts.it_value.tv_nsec = static_cast<long>(delta % 1'000'000'000ULL);
		(void)timerfd_settime(timer_fd_, 0, &ts, nullptr);
	}

	void process_expirations() {
		u64 const now = now_ns();
		for (auto &[k, c]: conns_) {
			if (c->closed || c->closing) {
				continue;
			}
			if (ngtcp2_conn_get_expiry(c->conn) <= now) {
				if (ngtcp2_conn_handle_expiry(c->conn, now) != 0) {
					c->closing = true;
				} else {
					dirty_conns_.insert(c->scid_key);
				}
			}
		}
	}

	void flush_all() {
		for (auto const &k: dirty_conns_) {
			if (auto it = conns_.find(k); it != conns_.end() && !it->second->closed) {
				send_stream_data(it->second.get());
			}
		}
		dirty_conns_.clear();
	}

	void drain_udp() {
		static constexpr SZ kBatch = 32;
		A<u8[kMaxUdpPayload], kBatch> bufs;
		A<sockaddr_storage, kBatch> addrs;
		A<iovec, kBatch> iovs;
		A<mmsghdr, kBatch> msgs;
		for (SZ i = 0; i < kBatch; ++i) {
			iovs[i].iov_base = bufs[i];
			iovs[i].iov_len = kMaxUdpPayload;
			msgs[i].msg_hdr.msg_iov = &iovs[i];
			msgs[i].msg_hdr.msg_iovlen = 1;
			msgs[i].msg_hdr.msg_control = nullptr;
			msgs[i].msg_hdr.msg_controllen = 0;
		}
		for (;;) {
			for (SZ i = 0; i < kBatch; ++i) {
				msgs[i].msg_hdr.msg_name = &addrs[i];
				msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
				msgs[i].msg_hdr.msg_flags = 0;
			}
			int const n = ::recvmmsg(udp_fd_, msgs.data(), static_cast<unsigned int>(kBatch), MSG_DONTWAIT, nullptr);
			if (n <= 0) {
				return;
			}
			for (SZ i = 0; i < static_cast<SZ>(n); ++i) {
				if (msgs[i].msg_len == 0) {
					continue;
				}
				handle_packet(
					static_cast<u8 const *>(iovs[i].iov_base),
					msgs[i].msg_len,
					addrs[i],
					msgs[i].msg_hdr.msg_namelen);
			}
		}
	}

	void handle_packet(
		u8 const *pkt,
		SZ pktlen,
		sockaddr_storage const &remote,
		socklen_t remote_len) {
		ngtcp2_version_cid vcid{};
		int const rv = ngtcp2_pkt_decode_version_cid(&vcid, pkt, pktlen, kCidLen);
		if (rv != 0 && rv != NGTCP2_ERR_VERSION_NEGOTIATION) {
			return;
		}
		A<u8, kCidLen> const key = cid_to_key(vcid.dcid, vcid.dcidlen);
		Http3Conn *c = nullptr;
		if (auto it = cid_index_.find(key); it != cid_index_.end()) {
			c = it->second;
		} else {
			c = accept_new_conn(pkt, pktlen, remote, remote_len);
			if (c == nullptr) {
				return;
			}
		}
		ngtcp2_path path{};
		path.local.addr = reinterpret_cast<ngtcp2_sockaddr *>(&c->local_addr);
		path.local.addrlen = c->local_addrlen;
		sockaddr_storage remote_copy = remote;
		path.remote.addr = reinterpret_cast<ngtcp2_sockaddr *>(&remote_copy);
		path.remote.addrlen = remote_len;
		ngtcp2_pkt_info pi{};
		int const prv = ngtcp2_conn_read_pkt(c->conn, &path, &pi, pkt, pktlen, now_ns());
		if (prv != 0) {
			if (prv == NGTCP2_ERR_DRAINING || prv == NGTCP2_ERR_CLOSING || prv == NGTCP2_ERR_DROP_CONN) {
				c->closing = true;
				c->closed = true;
				for (auto const &k: c->cid_keys) {
					cid_index_.erase(k);
				}
				return;
			}
			if (prv == NGTCP2_ERR_RETRY) {
				// Remove newly-created connection before sending Retry.
				for (auto const &k: c->cid_keys) {
					cid_index_.erase(k);
				}
				auto const ipkey = c->ip_key;
				auto const scid_key = c->scid_key;
				if (auto cnt = ip_conn_count_.find(ipkey); cnt != ip_conn_count_.end()) {
					if (--(cnt->second) <= 0) {
						ip_conn_count_.erase(cnt);
					}
				}
				free_quic_state(c);
				conns_.erase(scid_key);
				// Generate Retry token.
				u8 token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN];
				ngtcp2_cid odcid{};
				odcid.datalen = vcid.dcidlen;
				memcpy(odcid.data, vcid.dcid, vcid.dcidlen);
				ngtcp2_cid retry_scid{};
				retry_scid.datalen = kCidLen;
				RAND_bytes(retry_scid.data, static_cast<int>(retry_scid.datalen));
				ngtcp2_cid client_scid{};
				client_scid.datalen = vcid.scidlen;
				memcpy(client_scid.data, vcid.scid, vcid.scidlen);
				ngtcp2_ssize const tokenlen = ngtcp2_crypto_generate_retry_token(
					token,
					retry_secret_.data(),
					retry_secret_.size(),
					vcid.version,
					reinterpret_cast<ngtcp2_sockaddr const *>(&remote),
					remote_len,
					&retry_scid,
					&odcid,
					now_ns());
				if (tokenlen < 0) {
					return;
				}
				u8 retry_buf[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
				ngtcp2_ssize const wlen = ngtcp2_crypto_write_retry(
					retry_buf,
					sizeof(retry_buf),
					vcid.version,
					&client_scid,
					&retry_scid,
					&odcid,
					token,
					static_cast<SZ>(tokenlen));
				if (wlen > 0) {
					::sendto(
						udp_fd_,
						retry_buf,
						static_cast<SZ>(wlen),
						0,
						reinterpret_cast<sockaddr const *>(&remote),
						remote_len);
				}
				return;
			}
			c->closing = true;
		}
		send_stream_data(c);
	}

	Http3Conn *accept_new_conn(
		u8 const *pkt,
		SZ pktlen,
		sockaddr_storage const &remote,
		socklen_t remote_len) {
		if (conns_.size() >= 65536U) {
			return nullptr;
		}
		auto const ipkey = remote_ip_key(remote);
		{
			auto it = ip_conn_count_.find(ipkey);
			if (it != ip_conn_count_.end() && it->second >= 100) {
				return nullptr;
			}
		}
		ngtcp2_pkt_hd hd{};
		if (ngtcp2_accept(&hd, pkt, pktlen) != 0) {
			return nullptr;
		}
		auto c = make_unique<Http3Conn>();
		c->router = router_;
		c->listener = this;
		c->ip_key = ipkey;
		c->max_body_size = cfg_.max_body_size;
		memcpy(&c->remote_addr, &remote, remote_len);
		c->remote_addrlen = remote_len;
		c->local_addrlen = sizeof(c->local_addr);
		if (::getsockname(udp_fd_, reinterpret_cast<sockaddr *>(&c->local_addr), &c->local_addrlen) < 0) {
			return nullptr;
		}
		ngtcp2_cid scid{};
		scid.datalen = kCidLen;
		if (RAND_bytes(scid.data, static_cast<int>(scid.datalen)) != 1) {
			return nullptr;
		}
		c->scid_key = cid_to_key(scid.data, scid.datalen);
		c->cid_keys.push_back(c->scid_key);
		ngtcp2_path path{};
		path.local.addr = reinterpret_cast<ngtcp2_sockaddr *>(&c->local_addr);
		path.local.addrlen = c->local_addrlen;
		path.remote.addr = reinterpret_cast<ngtcp2_sockaddr *>(&c->remote_addr);
		path.remote.addrlen = c->remote_addrlen;
		ngtcp2_settings settings;
		ngtcp2_settings_default(&settings);
		settings.initial_ts = now_ns();
		settings.max_tx_udp_payload_size = kMaxUdpPayload;
		ngtcp2_transport_params params;
		ngtcp2_transport_params_default(&params);
		params.initial_max_stream_data_bidi_local = cfg_.max_stream_data;
		params.initial_max_stream_data_bidi_remote = cfg_.max_stream_data;
		params.initial_max_stream_data_uni = cfg_.max_stream_data;
		params.initial_max_data = cfg_.max_conn_data;
		params.initial_max_streams_bidi = cfg_.max_streams_bidi;
		params.initial_max_streams_uni = 3;
		params.max_idle_timeout = static_cast<ngtcp2_duration>(cfg_.idle_timeout_ms) * NGTCP2_MILLISECONDS;
		params.original_dcid_present = 1;
		params.original_dcid = hd.dcid;
		ngtcp2_callbacks cbs{};
		fill_callbacks(cbs);
		if (ngtcp2_conn_server_new(
				&c->conn,
				&hd.scid,
				&scid,
				&path,
				hd.version,
				&cbs,
				&settings,
				&params,
				nullptr,
				c.get())
			!= 0) {
			return nullptr;
		}
		c->ssl = SSL_new(ssl_ctx_);
		if (c->ssl == nullptr) {
			ngtcp2_conn_del(c->conn);
			return nullptr;
		}
		c->conn_ref.get_conn = crypto_conn_ref_get_conn;
		c->conn_ref.user_data = c.get();
		SSL_set_accept_state(c->ssl);
		if (ngtcp2_crypto_ossl_ctx_new(&c->ossl_ctx, c->ssl) != 0) {
			SSL_free(c->ssl);
			ngtcp2_conn_del(c->conn);
			return nullptr;
		}
		if (ngtcp2_crypto_ossl_configure_server_session(c->ssl) != 0) {
			ngtcp2_crypto_ossl_ctx_del(c->ossl_ctx);
			c->ossl_ctx = nullptr;
			SSL_free(c->ssl);
			ngtcp2_conn_del(c->conn);
			return nullptr;
		}
		SSL_set_app_data(c->ssl, &c->conn_ref);
		ngtcp2_conn_set_tls_native_handle(c->conn, c->ossl_ctx);
		auto const key = c->scid_key;
		auto const client_dcid_key = cid_to_key(hd.dcid.data, hd.dcid.datalen);
		c->cid_keys.push_back(client_dcid_key);
		++ip_conn_count_[ipkey];
		auto [it, inserted] = conns_.emplace(key, move(c));
		auto *raw = it->second.get();
		cid_index_.emplace(key, raw);
		cid_index_.emplace(client_dcid_key, raw);
		return raw;
	}

	void send_stream_data(
		Http3Conn *c) {
		if (c->closed || c->closing) {
			return;
		}
		u8 buf[kMaxUdpPayload];
		ngtcp2_path_storage ps;
		ngtcp2_path_storage_zero(&ps);
		ngtcp2_pkt_info pi{};
		for (;;) {
			i64 stream_id = -1;
			int fin = 0;
			nghttp3_vec vec[16];
			nghttp3_ssize sveccnt = 0;
			if (c->h3conn != nullptr) {
				sveccnt = nghttp3_conn_writev_stream(c->h3conn, &stream_id, &fin, vec, 16);
				if (sveccnt < 0) {
					c->closing = true;
					return;
				}
			}
			ngtcp2_ssize ndatalen{};
			u32 const flags = (sveccnt > 0 ? NGTCP2_WRITE_STREAM_FLAG_MORE : NGTCP2_WRITE_STREAM_FLAG_NONE)
							| (fin != 0 ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0u);
			ngtcp2_ssize const nwrite = ngtcp2_conn_writev_stream(
				c->conn,
				&ps.path,
				&pi,
				buf,
				sizeof(buf),
				&ndatalen,
				flags,
				stream_id,
				reinterpret_cast<ngtcp2_vec *>(vec),
				static_cast<SZ>(sveccnt),
				now_ns());
			if (nwrite < 0) {
				if (nwrite == NGTCP2_ERR_WRITE_MORE) {
					if (c->h3conn != nullptr && ndatalen >= 0) {
						(void)nghttp3_conn_add_write_offset(c->h3conn, stream_id, static_cast<SZ>(ndatalen));
					}
					continue;
				}
				if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED && c->h3conn != nullptr) {
					nghttp3_conn_block_stream(c->h3conn, stream_id);
					continue;
				}
				c->closing = true;
				return;
			}
			if (nwrite == 0) {
				if (ndatalen >= 0 && c->h3conn != nullptr) {
					(void)nghttp3_conn_add_write_offset(c->h3conn, stream_id, static_cast<SZ>(ndatalen));
				}
				return;
			}
			if (ndatalen >= 0 && c->h3conn != nullptr) {
				(void)nghttp3_conn_add_write_offset(c->h3conn, stream_id, static_cast<SZ>(ndatalen));
			}
			(void)::sendto(
				udp_fd_,
				buf,
				static_cast<SZ>(nwrite),
				0,
				reinterpret_cast<sockaddr const *>(&c->remote_addr),
				c->remote_addrlen);
		}
	}

	void write_immediate_close(
		Http3Conn *c) {
		if (c->conn == nullptr || c->closed) {
			return;
		}
		u8 buf[kMaxUdpPayload];
		ngtcp2_path_storage ps;
		ngtcp2_path_storage_zero(&ps);
		ngtcp2_ccerr ccerr;
		ngtcp2_ccerr_default(&ccerr);
		ngtcp2_pkt_info pi{};
		ngtcp2_ssize const n =
			ngtcp2_conn_write_connection_close(c->conn, &ps.path, &pi, buf, sizeof(buf), &ccerr, now_ns());
		if (n > 0) {
			(void)::sendto(
				udp_fd_,
				buf,
				static_cast<SZ>(n),
				0,
				reinterpret_cast<sockaddr const *>(&c->remote_addr),
				c->remote_addrlen);
		}
		c->closed = true;
	}

	static void free_quic_state(
		Http3Conn *c) noexcept {
		if (c->h3conn != nullptr) {
			nghttp3_conn_del(c->h3conn);
			c->h3conn = nullptr;
		}
		if (c->ossl_ctx != nullptr) {
			ngtcp2_crypto_ossl_ctx_del(c->ossl_ctx);
			c->ossl_ctx = nullptr;
		}
		if (c->ssl != nullptr) {
			SSL_set_app_data(c->ssl, nullptr);
			SSL_free(c->ssl);
			c->ssl = nullptr;
		}
		if (c->conn != nullptr) {
			ngtcp2_conn_del(c->conn);
			c->conn = nullptr;
		}
	}

	void drain_close_all() {
		for (auto &[k, c]: conns_) {
			write_immediate_close(c.get());
			free_quic_state(c.get());
		}
		conns_.clear();
		cid_index_.clear();
		ip_conn_count_.clear();
	}

	void gc_closed_conns() {
		for (auto it = conns_.begin(); it != conns_.end();) {
			if (it->second->closed) {
				for (auto const &k: it->second->cid_keys) {
					cid_index_.erase(k);
				}
				auto const &ipkey = it->second->ip_key;
				if (auto cnt = ip_conn_count_.find(ipkey); cnt != ip_conn_count_.end()) {
					if (--(cnt->second) <= 0) {
						ip_conn_count_.erase(cnt);
					}
				}
				free_quic_state(it->second.get());
				it = conns_.erase(it);
			} else {
				++it;
			}
		}
	}

public:
	void register_cid(
		Http3Conn *c,
		A<u8, kCidLen> const &key) {
		cid_index_.insert_or_assign(key, c);
	}

	void unregister_cid(
		A<u8, kCidLen> const &key) {
		cid_index_.erase(key);
	}

private:
	Router const *router_;
	Http3Config cfg_;
	u16 port_;
	SSL_CTX *ssl_ctx_;
	int udp_fd_{-1};
	int shutdown_efd_{-1};
	int timer_fd_{-1};
	atomic_flag stopping_{};
	thread thread_;
	mutex stop_mu_;
	A<u8, 32> retry_secret_{};
	std::unordered_map<A<u8, 16>, int, CidHash> ip_conn_count_;
	std::unordered_map<A<u8, kCidLen>, UP<Http3Conn>, CidHash> conns_;
	std::unordered_set<A<u8, kCidLen>, CidHash> dirty_conns_;
	std::unordered_map<A<u8, kCidLen>, Http3Conn *, CidHash> cid_index_;
};

namespace http3_detail {

inline void register_cid_on_listener(
	Http3Conn *c,
	A<u8, kCidLen> const &key) {
	if (c->listener != nullptr) {
		static_cast<Http3Listener *>(c->listener)->register_cid(c, key);
	}
}
inline void unregister_cid_on_listener(
	Http3Conn *c,
	A<u8, kCidLen> const &key) {
	if (c->listener != nullptr) {
		static_cast<Http3Listener *>(c->listener)->unregister_cid(key);
	}
}

} // namespace http3_detail
