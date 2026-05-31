module;
#include <fcntl.h>
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

namespace conflux::net_tls {

using conflux::IoError;
using conflux::socket_io::TcpStream;

export struct TlsError : std::runtime_error {
	explicit TlsError(
		std::string const &msg)
		: std::runtime_error{msg} {}
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
	std::string_view cert_file;
	std::string_view key_file;
	std::string_view cipher_list{}; // empty = built-in TLS 1.2 default
	std::string_view ciphersuites{}; // empty = built-in TLS 1.3 default
	bool ktls{false};
};
namespace tls_detail {

constexpr std::string_view kDefaultTls12CipherList =
	"ECDHE-ECDSA-AES128-GCM-SHA256:"
	"ECDHE-RSA-AES128-GCM-SHA256:"
	"ECDHE-ECDSA-AES256-GCM-SHA384:"
	"ECDHE-RSA-AES256-GCM-SHA384:"
	"ECDHE-ECDSA-CHACHA20-POLY1305:"
	"ECDHE-RSA-CHACHA20-POLY1305";

constexpr std::string_view kDefaultTls13Ciphersuites =
	"TLS_AES_128_GCM_SHA256:"
	"TLS_AES_256_GCM_SHA384:"
	"TLS_CHACHA20_POLY1305_SHA256";

// Session id context: 1-std::byte tag unique to this build; SSL_CTX requires a
// non-empty id to enable server-side session cache.
constexpr std::array<unsigned char, 8> kSessionIdContext{'c', 'o', 'n', 'f', 'l', 'u', 'x', '1'};
inline UniqueSslCtx make_server_ctx(
	TlsServerOptions const &opts) {
	UniqueSslCtx ctx{SSL_CTX_new(TLS_server_method())};
	if (!ctx) {
		throw TlsError{"SSL_CTX_new failed"};
	}
	SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
	std::uint64_t tls_opts = SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION;
	if (opts.ktls) {
		tls_opts |= SSL_OP_ENABLE_KTLS | SSL_OP_ENABLE_KTLS_TX_ZEROCOPY_SENDFILE;
	}
	SSL_CTX_set_options(ctx.get(), tls_opts);
	std::string const cipher_list{opts.cipher_list.empty() ? kDefaultTls12CipherList : opts.cipher_list};
	if (SSL_CTX_set_cipher_list(ctx.get(), cipher_list.c_str()) != 1) {
		throw TlsError{"SSL_CTX_set_cipher_list failed"};
	}
	std::string const ciphersuites{opts.ciphersuites.empty() ? kDefaultTls13Ciphersuites : opts.ciphersuites};
	if (SSL_CTX_set_ciphersuites(ctx.get(), ciphersuites.c_str()) != 1) {
		throw TlsError{"SSL_CTX_set_ciphersuites failed"};
	}
	SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
	SSL_CTX_set_session_id_context(ctx.get(), kSessionIdContext.data(), kSessionIdContext.size());
	if (SSL_CTX_use_certificate_chain_file(ctx.get(), std::string{opts.cert_file}.c_str()) != 1) {
		throw TlsError{std::format("TLS: cannot load cert: {}", opts.cert_file)};
	}
	if (SSL_CTX_use_PrivateKey_file(ctx.get(), std::string{opts.key_file}.c_str(), SSL_FILETYPE_PEM) != 1) {
		throw TlsError{std::format("TLS: cannot load key: {}", opts.key_file)};
	}
	return ctx;
}
inline int sni_callback(
	SSL *ssl,
	int * /*alert*/,
	void *user_data) {
	auto const *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (name == nullptr) {
		return SSL_TLSEXT_ERR_OK;
	}
	auto &vhosts = *static_cast<std::unordered_map<std::string, UniqueSslCtx> *>(user_data);
	auto const it = vhosts.find(ascii_lower(name));
	if (it != vhosts.end()) {
		SSL_set_SSL_CTX(ssl, it->second.get());
	}
	return SSL_TLSEXT_ERR_OK;
}

inline void set_tls_fd_nonblocking(
	int fd) {
	int const flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		throw TlsError{"TLS: fcntl(F_GETFL) failed"};
	}
	if ((flags & O_NONBLOCK) != 0) {
		return;
	}
	if (::fcntl(fd, F_SETFL, static_cast<int>(static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK))) < 0) {
		throw TlsError{"TLS: fcntl(F_SETFL O_NONBLOCK) failed"};
	}
}

struct ClientNameAccessors {
	template<typename Self>
	bool set_server_name(
		this Self &self,
		std::string_view sni) {
		if (sni.empty()) {
			return true;
		}
		std::string const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(self.native_handle(), s.c_str()) == 1;
#pragma GCC diagnostic pop
	}
	template<typename Self>
	bool set_verify_hostname(
		this Self &self,
		std::string_view host) {
		if (host.empty()) {
			return true;
		}
		std::string const s{host};
		return SSL_set1_host(self.native_handle(), s.c_str()) == 1;
	}
};

} // namespace tls_detail
// Owns a primary server SSL_CTX plus optional per-SNI vhost contexts.
// init_openssl_once() must be called before constructing.
export class TlsServerContext {
	UniqueSslCtx ctx_;
	std::unordered_map<std::string, UniqueSslCtx> vhost_ctxs_;

public:
	explicit TlsServerContext(
		TlsServerOptions const &opts)
		: ctx_{tls_detail::make_server_ctx(opts)} {}
	void add_vhost(
		std::string_view hostname,
		TlsServerOptions const &opts) {
		vhost_ctxs_.emplace(ascii_lower(hostname), tls_detail::make_server_ctx(opts));
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
export class TlsStream : public tls_detail::ClientNameAccessors {
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
		tls_detail::set_tls_fd_nonblocking(fd);
		if (SSL_set_fd(ssl_.get(), fd) != 1) {
			throw TlsError{"TlsStream: SSL_set_fd failed"};
		}
		fd_ = fd;
	}
	TlsStream(TlsStream const &) = delete;
	TlsStream &operator =(TlsStream const &) = delete;
	TlsStream(
		TlsStream &&other) noexcept
		: ssl_{std::move(other.ssl_)}
		, fd_{std::exchange(other.fd_, -1)} {}
	TlsStream &operator =(
		TlsStream &&other) noexcept {
		if (this != &other) {
			ssl_ = std::move(other.ssl_);
			fd_ = std::exchange(other.fd_, -1);
		}
		return *this;
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
		std::string_view data,
		int timeout_sec) {
		std::size_t sent = 0;
		while (sent < data.size()) {
			if (!wait_fd(fd_, POLLOUT, timeout_sec)) {
				return false;
			}
			int const n = SSL_write(ssl_.get(), data.data() + sent, static_cast<int>(data.size() - sent));
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
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
		std::string &out,
		int timeout_sec) {
		std::array<char, 4096> tmp{};
		for (;;) {
			if (!wait_fd(fd_, POLLIN, timeout_sec)) {
				return false;
			}
			int const n = SSL_read(ssl_.get(), tmp.data(), static_cast<int>(tmp.size()));
			if (n > 0) {
				out.append(tmp.data(), static_cast<std::size_t>(n));
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
// Async client TLS over a conflux::file_io::FileReader-driven socket. SSL is attached to memory
// BIOs; ciphertext is shuttled to/from the socket via io_uring.
export class TlsAsyncStream : public tls_detail::ClientNameAccessors {
	UniqueSsl ssl_;
	BIO *rbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	BIO *wbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	conflux::file_io::FileReader *files_{nullptr};
	FileHandle sock_{};
	std::array<std::byte, static_cast<std::size_t>(16U) * 1024U> scratch_{};

public:
	TlsAsyncStream(
		TlsContext &ctx,
		conflux::file_io::FileReader &files,
		FileHandle sock)
		: ssl_{SSL_new(ctx.native_handle())}
		, files_{&files}
		, sock_{std::move(sock)} {
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
		: ssl_{std::move(other.ssl_)}
		, rbio_{std::exchange(other.rbio_, nullptr)}
		, wbio_{std::exchange(other.wbio_, nullptr)}
		, files_{std::exchange(other.files_, nullptr)}
		, sock_{std::move(other.sock_)}
		, scratch_{other.scratch_} {}
	TlsAsyncStream &operator =(
		TlsAsyncStream &&other) noexcept {
		if (this != &other) {
			ssl_ = std::move(other.ssl_);
			rbio_ = std::exchange(other.rbio_, nullptr);
			wbio_ = std::exchange(other.wbio_, nullptr);
			files_ = std::exchange(other.files_, nullptr);
			sock_ = std::move(other.sock_);
			scratch_ = other.scratch_;
		}
		return *this;
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
	conflux::work::root::Task<std::size_t> read_some(
		std::span<std::byte> dst) {
		for (;;) {
			int const n = SSL_read(ssl_.get(), dst.data(), static_cast<int>(dst.size()));
			if (n > 0) {
				co_return static_cast<std::size_t>(n);
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_ZERO_RETURN) {
				co_return std::size_t{0};
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
		std::span<std::byte const> src) {
		std::size_t sent = 0;
		while (sent < src.size()) {
			int const n = SSL_write(ssl_.get(), src.data() + sent, static_cast<int>(src.size() - sent));
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
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
			int const want = static_cast<int>(std::min(scratch_.size(), static_cast<std::size_t>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			std::size_t off = 0;
			while (off < static_cast<std::size_t>(got)) {
				auto const w = co_await files_->write_into(
					sock_,
					0,
					std::span<std::byte const>{scratch_.data() + off, static_cast<std::size_t>(got) - off});
				if (w == 0) {
					throw TlsError{"TlsAsyncStream: socket write 0"};
				}
				off += w;
			}
		}
	}
	conflux::work::root::Task<void> fill_rbio() {
		auto const got = co_await files_->read_into(sock_, 0, std::span<std::byte>{scratch_});
		if (got == 0) {
			throw TlsError{"TlsAsyncStream: socket EOF"};
		}
		BIO_write(rbio_, reinterpret_cast<char const *>(scratch_.data()), static_cast<int>(got));
	}
};
enum class CancelMode : std::uint8_t {
	throw_cancelled,
	return_early,
};
export class TcpTlsStream : public tls_detail::ClientNameAccessors {
	UniqueSsl ssl_;
	BIO *rbio_{nullptr};
	BIO *wbio_{nullptr};
	TcpStream stream_;
	std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel_;
	std::array<std::uint8_t, 16384> scratch_{};
	using TP = std::chrono::steady_clock::time_point;
	using ms = std::chrono::milliseconds;
	[[nodiscard]] wroot::Task<void> drain_wbio_until(
		TP deadline,
		CancelMode cm = CancelMode::throw_cancelled) {
		for (;;) {
			int const pend = BIO_pending(wbio_);
			if (pend <= 0) {
				co_return;
			}
			int const want = static_cast<int>(std::min(scratch_.size(), static_cast<std::size_t>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			std::size_t off = 0;
			while (off < static_cast<std::size_t>(got)) {
				if (cm == CancelMode::throw_cancelled) {
					cancel_->throw_if_cancelled();
				} else if (cancel_->is_cancelled()) {
					co_return;
				}
				auto const now = std::chrono::steady_clock::now();
				if (now >= deadline) {
					throw IoError{ETIMEDOUT, "tcp: send timed out"};
				}
				auto remaining = std::chrono::ceil<ms>(deadline - now);
				auto child = stream_.async_write_borrowed(
					std::span<std::uint8_t const>{scratch_.data() + off, static_cast<std::size_t>(got) - off},
					remaining);
				try {
					std::size_t const n = co_await cancel_->await_child(std::move(child));
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
			int const want = static_cast<int>(std::min(scratch_.size(), static_cast<std::size_t>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			std::size_t off = 0;
			while (off < static_cast<std::size_t>(got)) {
				if (cm == CancelMode::throw_cancelled) {
					cancel_->throw_if_cancelled();
				} else if (cancel_->is_cancelled()) {
					co_return;
				}
				auto child = stream_.async_write_borrowed(
					std::span<std::uint8_t const>{scratch_.data() + off, static_cast<std::size_t>(got) - off},
					per_write);
				try {
					std::size_t const n = co_await cancel_->await_child(std::move(child));
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
		auto const now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			throw IoError{ETIMEDOUT, "tcp: recv timed out"};
		}
		auto remaining = std::chrono::ceil<ms>(deadline - now);
		auto child = stream_.async_recv_borrowed(std::span<std::uint8_t>{scratch_}, remaining);
		auto const got = co_await cancel_->await_child(std::move(child));
		if (got == 0) {
			throw TlsError{"TcpTlsStream: socket EOF"};
		}
		BIO_write(rbio_, reinterpret_cast<char const *>(scratch_.data()), static_cast<int>(got));
	}

public:
	TcpTlsStream(
		TlsContext &ctx,
		TcpStream stream,
		std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel =
			std::make_shared<conflux::net::detail::ActiveTaskCancelRelay>())
		: ssl_{SSL_new(ctx.native_handle())}
		, stream_{std::move(stream)}
		, cancel_{std::move(cancel)} {
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
		: ssl_{std::move(other.ssl_)}
		, rbio_{std::exchange(other.rbio_, nullptr)}
		, wbio_{std::exchange(other.wbio_, nullptr)}
		, stream_{std::move(other.stream_)}
		, cancel_{std::move(other.cancel_)}
		, scratch_{other.scratch_} {}
	TcpTlsStream &operator =(
		TcpTlsStream &&other) noexcept {
		if (this != &other) {
			ssl_ = std::move(other.ssl_);
			rbio_ = std::exchange(other.rbio_, nullptr);
			wbio_ = std::exchange(other.wbio_, nullptr);
			stream_ = std::move(other.stream_);
			cancel_ = std::move(other.cancel_);
			scratch_ = other.scratch_;
		}
		return *this;
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
	[[nodiscard]] wroot::Task<std::size_t> read_some(
		std::span<std::uint8_t> dst,
		ms per_recv) {
		auto const deadline = std::chrono::steady_clock::now() + per_recv;
		for (;;) {
			cancel_->throw_if_cancelled();
			int const n = SSL_read(ssl_.get(), reinterpret_cast<char *>(dst.data()), static_cast<int>(dst.size()));
			if (n > 0) {
				co_return static_cast<std::size_t>(n);
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_ZERO_RETURN) {
				co_return std::size_t{0};
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
		std::span<std::uint8_t const> src,
		ms per_write) {
		std::size_t sent = 0;
		while (sent < src.size()) {
			cancel_->throw_if_cancelled();
			int const n = SSL_write(
				ssl_.get(),
				reinterpret_cast<char const *>(src.data() + sent),
				static_cast<int>(src.size() - sent));
			if (n > 0) {
				sent += static_cast<std::size_t>(n);
				co_await drain_wbio_for(per_write);
				continue;
			}
			int const err = SSL_get_error(ssl_.get(), n);
			if (err == SSL_ERROR_WANT_READ) {
				auto const deadline = std::chrono::steady_clock::now() + per_write;
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
		co_await stream_.async_close();
	}
	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_.get(); }
};

} // namespace conflux::net_tls
