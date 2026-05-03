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

export struct TlsError : RE {
	using RE::runtime_error;
};

struct SslCtxDeleter {
	void operator ()(SSL_CTX *p) const noexcept { SSL_CTX_free(p); }
};
struct SslDeleter {
	void operator ()(SSL *p) const noexcept { SSL_free(p); }
};
struct BioDeleter {
	void operator ()(BIO *p) const noexcept { BIO_free(p); }
};
using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using UniqueSsl    = std::unique_ptr<SSL, SslDeleter>;
using UniqueBio    = std::unique_ptr<BIO, BioDeleter>;

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
// BIOs; ciphertext is shuttled to/from the socket via io_uring. All ops are
// Task<T> so both coroutine and callback-style (via Task::flow() + block_on)
// call sites work.
export class TlsAsyncStream {
	UniqueSsl ssl_;
	BIO *rbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	BIO *wbio_{nullptr}; // owned by ssl_ after SSL_set_bio
	FileReader *files_{nullptr};
	FileHandle sock_{};
	A<std::byte, static_cast<SZ>(16U) * 1024U> scratch_{};

public:
	TlsAsyncStream(
		TlsContext &ctx,
		FileReader &files,
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

	Task<void> handshake_connect() {
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

	Task<SZ> read_some(
		std::span<std::byte> dst) {
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

	Task<void> write_all(
		std::span<std::byte const> src) {
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
};
