module;
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>

export module conflux.net.tls;
import std;
import conflux.types;
import conflux.utils;
import conflux.work;
import conflux.uring.handle;
import conflux.file_io;
import conflux.socket_io.coro;
import conflux.net.cancel;
namespace wroot = conflux::work::root;
export struct TlsError : RE {
	using RE::runtime_error;
};
export struct SslCtxDeleter {
	void operator ()(
		SSL_CTX *p) const noexcept {
		SSL_CTX_free(p);
	}
};
export struct SslDeleter {
	void operator ()(
		SSL *p) const noexcept {
		SSL_free(p);
	}
};
export struct BioDeleter {
	void operator ()(
		BIO *p) const noexcept {
		BIO_free(p);
	}
};
export using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
export using UniqueSsl = std::unique_ptr<SSL, SslDeleter>;
export using UniqueBio = std::unique_ptr<BIO, BioDeleter>;
export void init_openssl_once() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		if (OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1) {
			throw TlsError{"OPENSSL_init_ssl failed"};
		}
		if (OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1) {
			throw TlsError{"OPENSSL_init_crypto failed"};
		}
	});
}
export struct TlsServerOptions {
	SV cert_file;
	SV key_file;
	SV cipher_list{}; // empty = built-in TLS 1.2 default
	SV ciphersuites{}; // empty = built-in TLS 1.3 default
	bool ktls{false};
};
namespace tls_detail {

constexpr SV kDefaultTls12CipherList =
	"ECDHE-ECDSA-AES128-GCM-SHA256:"
	"ECDHE-RSA-AES128-GCM-SHA256:"
	"ECDHE-ECDSA-AES256-GCM-SHA384:"
	"ECDHE-RSA-AES256-GCM-SHA384:"
	"ECDHE-ECDSA-CHACHA20-POLY1305:"
	"ECDHE-RSA-CHACHA20-POLY1305";

constexpr SV kDefaultTls13Ciphersuites =
	"TLS_AES_128_GCM_SHA256:"
	"TLS_AES_256_GCM_SHA384:"
	"TLS_CHACHA20_POLY1305_SHA256";

// Session id context: 1-byte tag unique to this build; SSL_CTX requires a
// non-empty id to enable server-side session cache.
constexpr A<unsigned char, 8> kSessionIdContext{'c', 'o', 'n', 'f', 'l', 'u', 'x', '1'};
inline UniqueSslCtx make_server_ctx(
	TlsServerOptions const &opts) {
	UniqueSslCtx ctx{SSL_CTX_new(TLS_server_method())};
	if (!ctx) {
		throw TlsError{"SSL_CTX_new failed"};
	}
	SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
	u64 tls_opts = SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION;
	if (opts.ktls) {
		tls_opts |= SSL_OP_ENABLE_KTLS | SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE;
	}
	SSL_CTX_set_options(ctx.get(), tls_opts);
	S const cipher_list{opts.cipher_list.empty() ? kDefaultTls12CipherList : opts.cipher_list};
	if (SSL_CTX_set_cipher_list(ctx.get(), cipher_list.c_str()) != 1) {
		throw TlsError{"SSL_CTX_set_cipher_list failed"};
	}
	S const ciphersuites{opts.ciphersuites.empty() ? kDefaultTls13Ciphersuites : opts.ciphersuites};
	if (SSL_CTX_set_ciphersuites(ctx.get(), ciphersuites.c_str()) != 1) {
		throw TlsError{"SSL_CTX_set_ciphersuites failed"};
	}
	SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
	SSL_CTX_set_session_id_context(ctx.get(), kSessionIdContext.data(), kSessionIdContext.size());
	if (SSL_CTX_use_certificate_chain_file(ctx.get(), S{opts.cert_file}.c_str()) != 1) {
		throw TlsError{format("TLS: cannot load cert: {}", opts.cert_file)};
	}
	if (SSL_CTX_use_PrivateKey_file(ctx.get(), S{opts.key_file}.c_str(), SSL_FILETYPE_PEM) != 1) {
		throw TlsError{format("TLS: cannot load key: {}", opts.key_file)};
	}
	return ctx;
}
inline S ascii_lower_local(
	SV s) {
	S out{s};
	for (auto &c: out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + 32);
		}
	}
	return out;
}
inline int sni_callback(
	SSL *ssl,
	int * /*alert*/,
	void *user_data) {
	auto const *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (name == nullptr) {
		return SSL_TLSEXT_ERR_OK;
	}
	auto &vhosts = *static_cast<UM<S, UniqueSslCtx> *>(user_data);
	auto const it = vhosts.find(ascii_lower_local(name));
	if (it != vhosts.end()) {
		SSL_set_SSL_CTX(ssl, it->second.get());
	}
	return SSL_TLSEXT_ERR_OK;
}

} // namespace tls_detail
// Owns a primary server SSL_CTX plus optional per-SNI vhost contexts.
// init_openssl_once() must be called before constructing.
export class TlsServerContext {
	UniqueSslCtx ctx_;
	UM<S, UniqueSslCtx> vhost_ctxs_;

public:
	explicit TlsServerContext(
		TlsServerOptions const &opts)
		: ctx_{tls_detail::make_server_ctx(opts)} {}
	void add_vhost(
		SV hostname,
		TlsServerOptions const &opts) {
		vhost_ctxs_.emplace(tls_detail::ascii_lower_local(hostname), tls_detail::make_server_ctx(opts));
	}
	// Install SNI servername callback dispatching to vhost contexts. No-op if
	// no vhosts were added. Caller must keep this TlsServerContext alive while
	// the primary SSL_CTX is in use.
	void install_sni() {
		if (vhost_ctxs_.empty()) {
			return;
		}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		SSL_CTX_set_tlsext_servername_callback(ctx_.get(), tls_detail::sni_callback);
#pragma GCC diagnostic pop
		SSL_CTX_set_tlsext_servername_arg(ctx_.get(), &vhost_ctxs_);
	}
	[[nodiscard]] SSL_CTX *native_handle() const noexcept { return ctx_.get(); }
	[[nodiscard]] bool has_vhosts() const noexcept { return !vhost_ctxs_.empty(); }
};
export class TlsContext {
	UniqueSslCtx ctx_;

public:
	TlsContext()
		: ctx_{SSL_CTX_new(TLS_client_method())} {
		if (!ctx_) {
			throw TlsError{"TlsContext: SSL_CTX_new failed"};
		}
		SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_default_verify_paths(ctx_.get());
	}
	void set_verify_peer(
		bool enable) {
		SSL_CTX_set_verify(ctx_.get(), enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
	}
	void disable_verify() { SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr); }
	bool set_default_verify_paths() { return SSL_CTX_set_default_verify_paths(ctx_.get()) == 1; }
	bool set_min_proto_version(
		int version) {
		return SSL_CTX_set_min_proto_version(ctx_.get(), version) == 1;
	}
	[[nodiscard]] SSL_CTX *native_handle() const noexcept { return ctx_.get(); }
};
export class TlsStream {
	UniqueSsl ssl_;
	int fd_{-1};

public:
	TlsStream(
		TlsContext &ctx,
		int fd)
		: ssl_{SSL_new(ctx.native_handle())} {
		if (!ssl_) {
			throw TlsError{"TlsStream: SSL_new failed"};
		}
		if (SSL_set_fd(ssl_.get(), fd) != 1) {
			throw TlsError{"TlsStream: SSL_set_fd failed"};
		}
		fd_ = fd;
	}
	TlsStream(TlsStream const &) = delete;
	TlsStream &operator =(TlsStream const &) = delete;
	TlsStream(
		TlsStream &&other) noexcept
		: ssl_{move(other.ssl_)}
		, fd_{exchange(other.fd_, -1)} {}
	TlsStream &operator =(
		TlsStream &&other) noexcept {
		if (this != &other) {
			ssl_ = move(other.ssl_);
			fd_ = exchange(other.fd_, -1);
		}
		return *this;
	}
	bool set_server_name(
		SV sni) {
		if (sni.empty()) {
			return true;
		}
		S const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(ssl_.get(), s.c_str()) == 1;
#pragma GCC diagnostic pop
	}
	bool set_verify_hostname(
		SV host) {
		if (host.empty()) {
			return true;
		}
		S const s{host};
		return SSL_set1_host(ssl_.get(), s.c_str()) == 1;
	}
	// Blocking client handshake. `timeout_sec <= 0` disables timeout.
	bool handshake_connect(
		int timeout_sec) {
		for (;;) {
			if (!wait_fd(fd_, POLLIN | POLLOUT, timeout_sec)) {
				return false;
			}
			int const rc = SSL_connect(ssl_.get());
			if (rc == 1) {
				return true;
			}
			int const err = SSL_get_error(ssl_.get(), rc);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
				return false;
			}
		}
	}
	// Blocking send-all. Returns false on timeout or connection error.
	bool write_all(
		SV data,
		int timeout_sec) {
		SZ sent = 0;
		while (sent < data.size()) {
			if (!wait_fd(fd_, POLLOUT, timeout_sec)) {
				return false;
			}
			int const n = SSL_write(ssl_.get(), data.data() + sent, static_cast<int>(data.size() - sent));
			if (n > 0) {
				sent += static_cast<SZ>(n);
				continue;
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_WANT_READ) {
				if (!wait_fd(fd_, POLLIN, timeout_sec)) {
					return false;
				}
				continue;
			}
			if (err != SSL_ERROR_WANT_WRITE) {
				return false;
			}
		}
		return true;
	}
	// Blocking read. Appends up to tmp.size() bytes into `out`, returns false on
	// error/timeout. Short reads (one SSL record) are normal.
	bool read_some(
		S &out,
		int timeout_sec) {
		A<char, 4096> tmp{};
		for (;;) {
			if (!wait_fd(fd_, POLLIN, timeout_sec)) {
				return false;
			}
			int const n = SSL_read(ssl_.get(), tmp.data(), static_cast<int>(tmp.size()));
			if (n > 0) {
				out.append(tmp.data(), static_cast<SZ>(n));
				return true;
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_WANT_READ) {
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				if (!wait_fd(fd_, POLLOUT, timeout_sec)) {
					return false;
				}
				continue;
			}
			return false;
		}
	}
	void shutdown_safe() noexcept {
		if (ssl_) {
			SSL_shutdown(ssl_.get());
		}
	}
	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_.get(); }
	[[nodiscard]] int fd() const noexcept { return fd_; }
};
// Async client TLS over a FileReader-driven socket. SSL is attached to memory
// BIOs; ciphertext is shuttled to/from the socket via io_uring.
export class TlsAsyncStream {
	UniqueSsl ssl_;
	BIO *rbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	BIO *wbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	FileReader *files_{nullptr};
	FileHandle sock_{};
	A<byte, static_cast<SZ>(16U) * 1024U> scratch_{};

public:
	TlsAsyncStream(
		TlsContext &ctx,
		FileReader &files,
		FileHandle sock)
		: ssl_{SSL_new(ctx.native_handle())}
		, files_{&files}
		, sock_{move(sock)} {
		if (!ssl_) {
			throw TlsError{"TlsAsyncStream: SSL_new failed"};
		}
		UniqueBio rbio{BIO_new(BIO_s_mem())};
		UniqueBio wbio{BIO_new(BIO_s_mem())};
		if (!rbio || !wbio) {
			throw TlsError{"TlsAsyncStream: BIO_new failed"};
		}
		rbio_ = rbio.get();
		wbio_ = wbio.get();
		SSL_set_bio(ssl_.get(), rbio.release(), wbio.release()); // SSL owns both BIOs
	}
	TlsAsyncStream(TlsAsyncStream const &) = delete;
	TlsAsyncStream &operator =(TlsAsyncStream const &) = delete;
	TlsAsyncStream(
		TlsAsyncStream &&other) noexcept
		: ssl_{move(other.ssl_)}
		, rbio_{exchange(other.rbio_, nullptr)}
		, wbio_{exchange(other.wbio_, nullptr)}
		, files_{exchange(other.files_, nullptr)}
		, sock_{move(other.sock_)}
		, scratch_{other.scratch_} {}
	TlsAsyncStream &operator =(
		TlsAsyncStream &&other) noexcept {
		if (this != &other) {
			ssl_ = move(other.ssl_);
			rbio_ = exchange(other.rbio_, nullptr);
			wbio_ = exchange(other.wbio_, nullptr);
			files_ = exchange(other.files_, nullptr);
			sock_ = move(other.sock_);
			scratch_ = other.scratch_;
		}
		return *this;
	}
	bool set_server_name(
		SV sni) {
		if (sni.empty()) {
			return true;
		}
		S const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(ssl_.get(), s.c_str()) == 1;
#pragma GCC diagnostic pop
	}
	bool set_verify_hostname(
		SV host) {
		if (host.empty()) {
			return true;
		}
		S const s{host};
		return SSL_set1_host(ssl_.get(), s.c_str()) == 1;
	}
	conflux::work::root::Task<void> handshake_connect() {
		SSL_set_connect_state(ssl_.get());
		for (;;) {
			int const rc = SSL_do_handshake(ssl_.get());
			co_await drain_wbio();
			if (rc == 1) {
				co_return;
			}
			int const err = SSL_get_error(ssl_.get(), rc);
			if (err == SSL_ERROR_WANT_READ) {
				co_await fill_rbio();
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				continue;
			}
			throw TlsError{"TlsAsyncStream: handshake failed"};
		}
	}
	conflux::work::root::Task<SZ> read_some(
		span<byte> dst) {
		for (;;) {
			int const n = SSL_read(ssl_.get(), dst.data(), static_cast<int>(dst.size()));
			if (n > 0) {
				co_return static_cast<SZ>(n);
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_ZERO_RETURN) {
				co_return SZ{0};
			}
			if (err == SSL_ERROR_WANT_READ) {
				co_await fill_rbio();
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				co_await drain_wbio();
				continue;
			}
			throw TlsError{"TlsAsyncStream: read failed"};
		}
	}
	conflux::work::root::Task<void> write_all(
		span<byte const> src) {
		SZ sent = 0;
		while (sent < src.size()) {
			int const n = SSL_write(ssl_.get(), src.data() + sent, static_cast<int>(src.size() - sent));
			if (n > 0) {
				sent += static_cast<SZ>(n);
				co_await drain_wbio();
				continue;
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_WANT_READ) {
				co_await fill_rbio();
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				co_await drain_wbio();
				continue;
			}
			throw TlsError{"TlsAsyncStream: write failed"};
		}
	}
	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_.get(); }
	[[nodiscard]] FileHandle const &handle() const noexcept { return sock_; }

private:
	conflux::work::root::Task<void> drain_wbio() {
		for (;;) {
			int const pend = BIO_pending(wbio_);
			if (pend <= 0) {
				co_return;
			}
			int const want = static_cast<int>(min(scratch_.size(), static_cast<SZ>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			SZ off = 0;
			while (off < static_cast<SZ>(got)) {
				auto const w = co_await files_->write_into(
					sock_,
					0,
					span<byte const>{scratch_.data() + off, static_cast<SZ>(got) - off});
				if (w == 0) {
					throw TlsError{"TlsAsyncStream: socket write 0"};
				}
				off += w;
			}
		}
	}
	conflux::work::root::Task<void> fill_rbio() {
		auto const got = co_await files_->read_into(sock_, 0, span<byte>{scratch_});
		if (got == 0) {
			throw TlsError{"TlsAsyncStream: socket EOF"};
		}
		BIO_write(rbio_, reinterpret_cast<char const *>(scratch_.data()), static_cast<int>(got));
	}
};
enum class CancelMode : u8 {
	throw_cancelled,
	return_early,
};
export class TcpTlsStream {
	UniqueSsl ssl_;
	BIO *rbio_{nullptr};
	BIO *wbio_{nullptr};
	TcpStream stream_;
	SP<ActiveTaskCancelRelay> cancel_;
	A<u8, 16384> scratch_{};
	using TP = chrono::steady_clock::time_point;
	using ms = chrono::milliseconds;
	[[nodiscard]] wroot::Task<void> drain_wbio_until(
		TP deadline,
		CancelMode cm = CancelMode::throw_cancelled) {
		for (;;) {
			int const pend = BIO_pending(wbio_);
			if (pend <= 0) {
				co_return;
			}
			int const want = static_cast<int>(min(scratch_.size(), static_cast<SZ>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			SZ off = 0;
			while (off < static_cast<SZ>(got)) {
				if (cm == CancelMode::throw_cancelled) {
					cancel_->throw_if_cancelled();
				} else if (cancel_->is_cancelled()) {
					co_return;
				}
				auto const now = chrono::steady_clock::now();
				if (now >= deadline) {
					throw IoError{ETIMEDOUT, "tcp: send timed out"};
				}
				auto remaining = chrono::ceil<ms>(deadline - now);
				auto child = stream_.write_borrowed(
					span<u8 const>{scratch_.data() + off, static_cast<SZ>(got) - off},
					remaining);
				try {
					SZ const n = co_await cancel_->await_child(move(child));
					if (n == 0) {
						throw IoError{ECONNRESET, "tcp: connection closed"};
					}
					off += n;
				} catch (wroot::CancelledError const &) {
					if (cm == CancelMode::return_early) {
						co_return;
					}
					throw;
				}
			}
		}
	}
	[[nodiscard]] wroot::Task<void> drain_wbio_for(
		ms per_write,
		CancelMode cm = CancelMode::throw_cancelled) {
		for (;;) {
			int const pend = BIO_pending(wbio_);
			if (pend <= 0) {
				co_return;
			}
			int const want = static_cast<int>(min(scratch_.size(), static_cast<SZ>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			SZ off = 0;
			while (off < static_cast<SZ>(got)) {
				if (cm == CancelMode::throw_cancelled) {
					cancel_->throw_if_cancelled();
				} else if (cancel_->is_cancelled()) {
					co_return;
				}
				auto child = stream_.write_borrowed(
					span<u8 const>{scratch_.data() + off, static_cast<SZ>(got) - off},
					per_write);
				try {
					SZ const n = co_await cancel_->await_child(move(child));
					if (n == 0) {
						throw IoError{ECONNRESET, "tcp: connection closed"};
					}
					off += n;
				} catch (wroot::CancelledError const &) {
					if (cm == CancelMode::return_early) {
						co_return;
					}
					throw;
				}
			}
		}
	}
	[[nodiscard]] wroot::Task<void> fill_rbio(
		TP deadline) {
		cancel_->throw_if_cancelled();
		auto const now = chrono::steady_clock::now();
		if (now >= deadline) {
			throw IoError{ETIMEDOUT, "tcp: recv timed out"};
		}
		auto remaining = chrono::ceil<ms>(deadline - now);
		auto child = stream_.recv_borrowed(span<u8>{scratch_}, remaining);
		auto const got = co_await cancel_->await_child(move(child));
		if (got == 0) {
			throw TlsError{"TcpTlsStream: socket EOF"};
		}
		BIO_write(rbio_, reinterpret_cast<char const *>(scratch_.data()), static_cast<int>(got));
	}

public:
	TcpTlsStream(
		TlsContext &ctx,
		TcpStream stream,
		SP<ActiveTaskCancelRelay> cancel = make_shared<ActiveTaskCancelRelay>())
		: ssl_{SSL_new(ctx.native_handle())}
		, stream_{move(stream)}
		, cancel_{move(cancel)} {
		if (!ssl_) {
			throw TlsError{"TcpTlsStream: SSL_new failed"};
		}
		UniqueBio rbio{BIO_new(BIO_s_mem())};
		UniqueBio wbio{BIO_new(BIO_s_mem())};
		if (!rbio || !wbio) {
			throw TlsError{"TcpTlsStream: BIO_new failed"};
		}
		rbio_ = rbio.get();
		wbio_ = wbio.get();
		SSL_set_bio(ssl_.get(), rbio.release(), wbio.release());
	}
	TcpTlsStream(TcpTlsStream const &) = delete;
	TcpTlsStream &operator =(TcpTlsStream const &) = delete;
	TcpTlsStream(
		TcpTlsStream &&other) noexcept
		: ssl_{move(other.ssl_)}
		, rbio_{exchange(other.rbio_, nullptr)}
		, wbio_{exchange(other.wbio_, nullptr)}
		, stream_{move(other.stream_)}
		, cancel_{move(other.cancel_)}
		, scratch_{other.scratch_} {}
	TcpTlsStream &operator =(
		TcpTlsStream &&other) noexcept {
		if (this != &other) {
			ssl_ = move(other.ssl_);
			rbio_ = exchange(other.rbio_, nullptr);
			wbio_ = exchange(other.wbio_, nullptr);
			stream_ = move(other.stream_);
			cancel_ = move(other.cancel_);
			scratch_ = other.scratch_;
		}
		return *this;
	}
	bool set_server_name(
		SV sni) {
		if (sni.empty()) {
			return true;
		}
		S const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(ssl_.get(), s.c_str()) == 1;
#pragma GCC diagnostic pop
	}
	bool set_verify_hostname(
		SV host) {
		if (host.empty()) {
			return true;
		}
		S const s{host};
		return SSL_set1_host(ssl_.get(), s.c_str()) == 1;
	}
	[[nodiscard]] wroot::Task<void> handshake_connect(
		TP deadline) {
		SSL_set_connect_state(ssl_.get());
		for (;;) {
			cancel_->throw_if_cancelled();
			int const rc = SSL_do_handshake(ssl_.get());
			co_await drain_wbio_until(deadline);
			if (rc == 1) {
				co_return;
			}
			int const err = SSL_get_error(ssl_.get(), rc);
			if (err == SSL_ERROR_WANT_READ) {
				co_await fill_rbio(deadline);
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				continue;
			}
			throw TlsError{"TcpTlsStream: handshake failed"};
		}
	}
	[[nodiscard]] wroot::Task<SZ> read_some(
		span<u8> dst,
		ms per_recv) {
		auto const deadline = chrono::steady_clock::now() + per_recv;
		for (;;) {
			cancel_->throw_if_cancelled();
			int const n = SSL_read(ssl_.get(), reinterpret_cast<char *>(dst.data()), static_cast<int>(dst.size()));
			if (n > 0) {
				co_return static_cast<SZ>(n);
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_ZERO_RETURN) {
				co_return SZ{0};
			}
			if (err == SSL_ERROR_WANT_READ) {
				co_await fill_rbio(deadline);
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				co_await drain_wbio_until(deadline);
				continue;
			}
			throw TlsError{"TcpTlsStream: read failed"};
		}
	}
	[[nodiscard]] wroot::Task<void> write_all(
		span<u8 const> src,
		ms per_write) {
		SZ sent = 0;
		while (sent < src.size()) {
			cancel_->throw_if_cancelled();
			int const n = SSL_write(
				ssl_.get(),
				reinterpret_cast<char const *>(src.data() + sent),
				static_cast<int>(src.size() - sent));
			if (n > 0) {
				sent += static_cast<SZ>(n);
				co_await drain_wbio_for(per_write);
				continue;
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_WANT_READ) {
				auto const deadline = chrono::steady_clock::now() + per_write;
				co_await fill_rbio(deadline);
				continue;
			}
			if (err == SSL_ERROR_WANT_WRITE) {
				co_await drain_wbio_for(per_write);
				continue;
			}
			throw TlsError{"TcpTlsStream: write failed"};
		}
	}
	[[nodiscard]] wroot::Task<void> close(
		ms close_drain_timeout) {
		if (!ssl_) {
			co_return;
		}
		SSL_shutdown(ssl_.get());
		co_await drain_wbio_for(close_drain_timeout, CancelMode::return_early);
		co_await stream_.close();
	}
	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_.get(); }
};
