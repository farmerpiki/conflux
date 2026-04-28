module;
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>

export module conflux.net.tls;
import std;
import conflux.types;
import conflux.utils;
import conflux.work;
import conflux.file_io;

using namespace std;

export struct TlsError : runtime_error {
	using runtime_error::runtime_error;
};

export class TlsContext {
	SSL_CTX *ctx_{nullptr};

public:
	TlsContext() {
		ctx_ = SSL_CTX_new(TLS_client_method());
		if (ctx_ == nullptr) {
			throw TlsError{"TlsContext: SSL_CTX_new failed"};
		}
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_default_verify_paths(ctx_);
	}

	TlsContext(TlsContext const &) = delete;
	TlsContext &operator =(TlsContext const &) = delete;

	TlsContext(
		TlsContext &&other) noexcept
		: ctx_{other.ctx_} {
		other.ctx_ = nullptr;
	}

	TlsContext &operator =(
		TlsContext &&other) noexcept {
		if (this != &other) {
			if (ctx_ != nullptr) {
				SSL_CTX_free(ctx_);
			}
			ctx_ = other.ctx_;
			other.ctx_ = nullptr;
		}
		return *this;
	}

	~TlsContext() {
		if (ctx_ != nullptr) {
			SSL_CTX_free(ctx_);
		}
	}

	void set_verify_peer(
		bool enable) {
		SSL_CTX_set_verify(ctx_, enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
	}

	void disable_verify() { SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr); }

	bool set_default_verify_paths() { return SSL_CTX_set_default_verify_paths(ctx_) == 1; }

	bool set_min_proto_version(
		int version) {
		return SSL_CTX_set_min_proto_version(ctx_, version) == 1;
	}

	[[nodiscard]] SSL_CTX *native_handle() const noexcept { return ctx_; }
};

export class TlsStream {
	SSL *ssl_{nullptr};
	int fd_{-1};

public:
	TlsStream(
		TlsContext &ctx,
		int fd) {
		ssl_ = SSL_new(ctx.native_handle());
		if (ssl_ == nullptr) {
			throw TlsError{"TlsStream: SSL_new failed"};
		}
		if (SSL_set_fd(ssl_, fd) != 1) {
			SSL_free(ssl_);
			ssl_ = nullptr;
			throw TlsError{"TlsStream: SSL_set_fd failed"};
		}
		fd_ = fd;
	}

	TlsStream(TlsStream const &) = delete;
	TlsStream &operator =(TlsStream const &) = delete;

	TlsStream(
		TlsStream &&other) noexcept
		: ssl_{other.ssl_}
		, fd_{other.fd_} {
		other.ssl_ = nullptr;
		other.fd_ = -1;
	}

	TlsStream &operator =(
		TlsStream &&other) noexcept {
		if (this != &other) {
			close_ssl();
			ssl_ = other.ssl_;
			fd_ = other.fd_;
			other.ssl_ = nullptr;
			other.fd_ = -1;
		}
		return *this;
	}

	~TlsStream() { close_ssl(); }

	bool set_server_name(
		string_view sni) {
		if (sni.empty()) {
			return true;
		}
		string const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(ssl_, s.c_str()) == 1;
#pragma GCC diagnostic pop
	}

	bool set_verify_hostname(
		string_view host) {
		if (host.empty()) {
			return true;
		}
		string const s{host};
		return SSL_set1_host(ssl_, s.c_str()) == 1;
	}

	// Blocking client handshake. `timeout_sec <= 0` disables timeout.
	bool handshake_connect(
		int timeout_sec) {
		for (;;) {
			if (!wait_fd(fd_, POLLIN | POLLOUT, timeout_sec)) {
				return false;
			}
			int const rc = SSL_connect(ssl_);
			if (rc == 1) {
				return true;
			}
			int const err = SSL_get_error(ssl_, rc);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
				return false;
			}
		}
	}

	// Blocking send-all. Returns false on timeout or connection error.
	bool write_all(
		string_view data,
		int timeout_sec) {
		size_t sent = 0;
		while (sent < data.size()) {
			if (!wait_fd(fd_, POLLOUT, timeout_sec)) {
				return false;
			}
			int const n = SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent));
			if (n > 0) {
				sent += static_cast<size_t>(n);
				continue;
			}
			int const err = SSL_get_error(ssl_, n);
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
		string &out,
		int timeout_sec) {
		array<char, 4096> tmp{};
		for (;;) {
			if (!wait_fd(fd_, POLLIN, timeout_sec)) {
				return false;
			}
			int const n = SSL_read(ssl_, tmp.data(), static_cast<int>(tmp.size()));
			if (n > 0) {
				out.append(tmp.data(), static_cast<size_t>(n));
				return true;
			}
			int const err = SSL_get_error(ssl_, n);
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
		if (ssl_ != nullptr) {
			SSL_shutdown(ssl_);
		}
	}

	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_; }

	[[nodiscard]] int fd() const noexcept { return fd_; }

private:
	void close_ssl() noexcept {
		if (ssl_ != nullptr) {
			SSL_free(ssl_);
			ssl_ = nullptr;
		}
	}
};

// Async client TLS over a FileReader-driven socket. SSL is attached to memory
// BIOs; ciphertext is shuttled to/from the socket via io_uring. All ops are
// Task<T> so both coroutine and callback-style (via Task::flow() + block_on)
// call sites work.
export class TlsAsyncStream {
	SSL *ssl_{nullptr};
	BIO *rbio_{nullptr}; // SSL reads plaintext-source ciphertext from this
	BIO *wbio_{nullptr}; // SSL writes outgoing ciphertext into this
	FileReader *files_{nullptr};
	FileHandle sock_{};
	A<std::byte, static_cast<SZ>(16U) * 1024U> scratch_{};

public:
	TlsAsyncStream(
		TlsContext &ctx,
		FileReader &files,
		FileHandle sock)
		: files_{&files}
		, sock_{std::move(sock)} {
		ssl_ = SSL_new(ctx.native_handle());
		if (ssl_ == nullptr) {
			throw TlsError{"TlsAsyncStream: SSL_new failed"};
		}
		rbio_ = BIO_new(BIO_s_mem());
		wbio_ = BIO_new(BIO_s_mem());
		if (rbio_ == nullptr || wbio_ == nullptr) {
			if (rbio_ != nullptr) {
				BIO_free(rbio_);
			}
			if (wbio_ != nullptr) {
				BIO_free(wbio_);
			}
			SSL_free(ssl_);
			ssl_ = nullptr;
			throw TlsError{"TlsAsyncStream: BIO_new failed"};
		}
		SSL_set_bio(ssl_, rbio_, wbio_); // SSL owns both BIOs
	}

	TlsAsyncStream(TlsAsyncStream const &) = delete;
	TlsAsyncStream &operator =(TlsAsyncStream const &) = delete;

	TlsAsyncStream(
		TlsAsyncStream &&other) noexcept
		: ssl_{other.ssl_}
		, rbio_{other.rbio_}
		, wbio_{other.wbio_}
		, files_{other.files_}
		, sock_{std::move(other.sock_)}
		, scratch_{other.scratch_} {
		other.ssl_ = nullptr;
		other.rbio_ = nullptr;
		other.wbio_ = nullptr;
		other.files_ = nullptr;
	}

	TlsAsyncStream &operator =(
		TlsAsyncStream &&other) noexcept {
		if (this != &other) {
			close_ssl();
			ssl_ = other.ssl_;
			rbio_ = other.rbio_;
			wbio_ = other.wbio_;
			files_ = other.files_;
			sock_ = std::move(other.sock_);
			scratch_ = other.scratch_;
			other.ssl_ = nullptr;
			other.rbio_ = nullptr;
			other.wbio_ = nullptr;
			other.files_ = nullptr;
		}
		return *this;
	}

	~TlsAsyncStream() { close_ssl(); }

	bool set_server_name(
		SV sni) {
		if (sni.empty()) {
			return true;
		}
		S const s{sni};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		return SSL_set_tlsext_host_name(ssl_, s.c_str()) == 1;
#pragma GCC diagnostic pop
	}

	bool set_verify_hostname(
		SV host) {
		if (host.empty()) {
			return true;
		}
		S const s{host};
		return SSL_set1_host(ssl_, s.c_str()) == 1;
	}

	Task<void> handshake_connect() {
		SSL_set_connect_state(ssl_);
		for (;;) {
			int const rc = SSL_do_handshake(ssl_);
			co_await drain_wbio();
			if (rc == 1) {
				co_return;
			}
			int const err = SSL_get_error(ssl_, rc);
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

	Task<SZ> read_some(
		std::span<std::byte> dst) {
		for (;;) {
			int const n = SSL_read(ssl_, dst.data(), static_cast<int>(dst.size()));
			if (n > 0) {
				co_return static_cast<SZ>(n);
			}
			int const err = SSL_get_error(ssl_, n);
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

	Task<void> write_all(
		std::span<std::byte const> src) {
		SZ sent = 0;
		while (sent < src.size()) {
			int const n = SSL_write(ssl_, src.data() + sent, static_cast<int>(src.size() - sent));
			if (n > 0) {
				sent += static_cast<SZ>(n);
				co_await drain_wbio();
				continue;
			}
			int const err = SSL_get_error(ssl_, n);
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

	[[nodiscard]] SSL *native_handle() const noexcept { return ssl_; }
	[[nodiscard]] FileHandle const &handle() const noexcept { return sock_; }

private:
	Task<void> drain_wbio() {
		for (;;) {
			int const pend = BIO_pending(wbio_);
			if (pend <= 0) {
				co_return;
			}
			int const want = static_cast<int>(std::min(scratch_.size(), static_cast<SZ>(pend)));
			int const got = BIO_read(wbio_, reinterpret_cast<char *>(scratch_.data()), want);
			if (got <= 0) {
				co_return;
			}
			SZ off = 0;
			while (off < static_cast<SZ>(got)) {
				auto const w = co_await files_->write_into(
					sock_,
					0,
					std::span<std::byte const>{scratch_.data() + off, static_cast<SZ>(got) - off});
				if (w == 0) {
					throw TlsError{"TlsAsyncStream: socket write 0"};
				}
				off += w;
			}
		}
	}

	Task<void> fill_rbio() {
		auto const got = co_await files_->read_into(sock_, 0, std::span<std::byte>{scratch_});
		if (got == 0) {
			throw TlsError{"TlsAsyncStream: socket EOF"};
		}
		BIO_write(rbio_, reinterpret_cast<char const *>(scratch_.data()), static_cast<int>(got));
	}

	void close_ssl() noexcept {
		if (ssl_ != nullptr) {
			SSL_free(ssl_); // also frees owned BIOs
			ssl_ = nullptr;
			rbio_ = nullptr;
			wbio_ = nullptr;
		}
	}
};
