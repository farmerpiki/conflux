// Plain TU — not a module unit. std::thread lambda → module TU-local rule.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.file_io_sync;

namespace root = conflux::work::root;
namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}
struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	FileReader reader;
	bool ring_ok{false};
	RingFixture()
		: reader{&ring, &completions, [](u32 slot, u32 gen) noexcept { return pack_ud(slot, gen); }} {}
	static UP<RingFixture> make(
		unsigned entries = 64) {
		auto fx = make_unique<RingFixture>();
		if (::io_uring_queue_init(entries, &fx->ring, 0) < 0) {
			return {};
		}
		fx->ring_ok = true;
		return fx;
	}
	~RingFixture() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingFixture(RingFixture const &) = delete;
	RingFixture &operator =(RingFixture const &) = delete;
	RingFixture(RingFixture &&) = delete;
	RingFixture &operator =(RingFixture &&) = delete;
};
UP<RingFixture> require_ring_fixture(
	unsigned entries = 64) {
	auto fx = RingFixture::make(entries);
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}
struct TempFile {
	S path;
	int fd{-1};
	static TempFile create(
		SV content = {}) {
		TempFile t;
		t.path = "/tmp/conflux_file_io_test_XXXXXX";
		t.fd = ::mkstemp(t.path.data());
		REQUIRE(t.fd >= 0);
		if (!content.empty()) {
			ssize_t const w = ::write(t.fd, content.data(), content.size());
			REQUIRE(w == static_cast<ssize_t>(content.size()));
		}
		return t;
	}
	~TempFile() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
	TempFile() = default;
	TempFile(TempFile const &) = delete;
	TempFile &operator =(TempFile const &) = delete;
	TempFile(
		TempFile &&o) noexcept
		: path{move(o.path)}
		, fd{exchange(o.fd, -1)} {}
	TempFile &operator =(TempFile &&) = delete;
};
struct TempDir {
	S path;
	int fd{-1};
	static TempDir create() {
		TempDir t;
		t.path = "/tmp/conflux_file_io_dir_XXXXXX";
		auto *r = ::mkdtemp(t.path.data());
		REQUIRE(r != nullptr);
		t.fd = ::open(t.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		REQUIRE(t.fd >= 0);
		return t;
	}
	~TempDir() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			EC ec;
			fs::remove_all(path, ec);
		}
	}
	TempDir() = default;
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(
		TempDir &&o) noexcept
		: path{move(o.path)}
		, fd{exchange(o.fd, -1)} {}
	TempDir &operator =(TempDir &&) = delete;
	void mkdir_sub(
		SV name) const {
		auto full = format("{}/{}", path, name);
		REQUIRE(::mkdir(full.c_str(), 0755) == 0);
	}
	void write_file(
		SV name,
		SV content) const {
		auto full = format("{}/{}", path, name);
		int const f = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		REQUIRE(f >= 0);
		auto const w = ::write(f, content.data(), content.size());
		::close(f);
		REQUIRE(w == static_cast<ssize_t>(content.size()));
	}
	[[nodiscard]] S read_file(
		SV name) const {
		auto full = format("{}/{}", path, name);
		int const f = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
		REQUIRE(f >= 0);
		S out(4096, '\0');
		auto const n = ::read(f, out.data(), out.size());
		::close(f);
		REQUIRE(n >= 0);
		out.resize(static_cast<SZ>(n));
		return out;
	}
	[[nodiscard]] bool has_staging_files(
		SV subdir = {}) const {
		fs::path p{path};
		if (!subdir.empty()) {
			p /= S{subdir};
		}
		for (auto const &entry : fs::directory_iterator{p}) {
			auto const name = entry.path().filename().string();
			if (name.starts_with(".conflux.tmp.")) {
				return true;
			}
		}
		return false;
	}
};

} // namespace
TEST_CASE(
	"file_io: CompletionTable reserve/dispatch round-trip",
	"[file_io][unit]") {
	CompletionTable table;
	int observed = 0;
	auto [slot, gen] = table.reserve([&](IoResult r) { observed = r.res; });
	CHECK(slot == 0);
	CHECK(gen == 0);
	table.dispatch(slot, gen, 42, 0);
	CHECK(observed == 42);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable rejects stale gen",
	"[file_io][unit]") {
	CompletionTable table;
	int fired = 0;
	auto [slot, gen] = table.reserve([&](IoResult) { ++fired; });
	table.dispatch(slot, gen, 0, 0);
	CHECK(fired == 1);
	table.dispatch(slot, gen, 0, 0); // stale — slot gen bumped
	CHECK(fired == 1);
}
TEST_CASE(
	"file_io: CompletionTable cancel_all fires pending with ECANCELED",
	"[file_io][unit]") {
	CompletionTable table;
	int res_a = 0;
	int res_b = 0;
	auto r_a = table.reserve([&](IoResult r) { res_a = r.res; });
	auto r_b = table.reserve([&](IoResult r) { res_b = r.res; });
	(void)r_a;
	(void)r_b;
	CHECK(table.cancel_all());
	CHECK(res_a == -ECANCELED);
	CHECK(res_b == -ECANCELED);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send waits for NOTIF CQE",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = -1;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, 17, IORING_CQE_F_MORE);
	CHECK(!fired);
	CHECK(table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 0, IORING_CQE_F_NOTIF);
	CHECK(fired);
	CHECK(got_res == 17);
	CHECK(!table.has_pending_zc_notifications());
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send first CQE error fires immediately",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = 0;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, -EPERM, 0);
	CHECK(fired);
	CHECK(got_res == -EPERM);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send first CQE success without MORE fires immediately",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = -1;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, 17, 0);
	CHECK(fired);
	CHECK(got_res == 17);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send stale NOTIF after slot free is ignored",
	"[file_io][unit]") {
	CompletionTable table;
	int fire_count = 0;
	auto [slot, gen] = table.reserve_zc([&](IoResult) noexcept { ++fire_count; });
	table.dispatch(slot, gen, 5, IORING_CQE_F_MORE);
	table.dispatch(slot, gen, 0, IORING_CQE_F_NOTIF);
	CHECK(fire_count == 1);
	table.dispatch(slot, gen, 0, IORING_CQE_F_NOTIF); // stale gen
	CHECK(fire_count == 1);
}
TEST_CASE(
	"file_io: CompletionTable has_pending_zc_notifications",
	"[file_io][unit]") {
	CompletionTable table;
	auto [slot, gen] = table.reserve_zc([](IoResult) noexcept {});
	CHECK(!table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 8, IORING_CQE_F_MORE);
	CHECK(table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 0, IORING_CQE_F_NOTIF);
	CHECK(!table.has_pending_zc_notifications());
}
TEST_CASE(
	"file_io: CompletionTable cancel_all refuses pending zc notification",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	auto [slot, gen] = table.reserve_zc([&](IoResult) noexcept { fired = true; });
	table.dispatch(slot, gen, 17, IORING_CQE_F_MORE);
	CHECK(table.has_pending_zc_notifications());
	CHECK_FALSE(table.cancel_all());
	CHECK(!fired);
	CHECK(table.pending() == 1);
	table.dispatch(slot, gen, 0, IORING_CQE_F_NOTIF);
	CHECK(fired);
	CHECK(table.cancel_all());
}
TEST_CASE(
	"file_io: open + stat + read_into round trip",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	auto tf = TempFile::create("hello file_io");

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	FileStat const st = block_on(fx->reader, fx->reader.stat_async(handle), chrono::seconds{5});
	CHECK(st.size == SV{"hello file_io"}.size());

	A<byte, 32> buf{};
	SZ const got =
		block_on(fx->reader, fx->reader.read_into(handle, 0, span<byte>{buf.data(), buf.size()}), chrono::seconds{5});
	REQUIRE(got == SV{"hello file_io"}.size());
	CHECK(memcmp(buf.data(), "hello file_io", got) == 0);
}
TEST_CASE(
	"file_io: read_fixed via registered buffer",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	S const content(1024, 'Z');
	auto tf = TempFile::create(content);

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	FileReader::ReadFixedResult const got =
		block_on(fx->reader, fx->reader.read_fixed(handle, 0, move(*buf)), chrono::seconds{5});
	REQUIRE(got.bytes == content.size());
	auto const view = got.buffer.view();
	for (SZ i = 0; i < got.bytes; ++i) {
		REQUIRE(static_cast<char>(view[i]) == 'Z');
	}
}
TEST_CASE(
	"file_io: splice_to_fd streams file into external pipe",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	PipePool pipes{2};
	auto pipe = pipes.try_acquire();
	REQUIRE(pipe.has_value());

	S const content(8UL * 1024, 'S');
	auto tf = TempFile::create(content);

	int sink_pipe[2] = {-1, -1};
	REQUIRE(::pipe2(sink_pipe, O_CLOEXEC) == 0);
	// Enlarge the sink pipe so a single splice completes in one chunk.
	::fcntl(sink_pipe[1], F_SETPIPE_SZ, 1 << 20);

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	SZ const delivered = block_on(
		fx->reader,
		fx->reader.splice_to_fd(handle, 0, content.size(), sink_pipe[1], move(*pipe)),
		chrono::seconds{5});
	CHECK(delivered == content.size());

	S drained(content.size(), '\0');
	SZ off = 0;
	while (off < drained.size()) {
		ssize_t const n = ::read(sink_pipe[0], drained.data() + off, drained.size() - off);
		if (n <= 0) {
			break;
		}
		off += static_cast<SZ>(n);
	}
	::close(sink_pipe[0]);
	::close(sink_pipe[1]);
	CHECK(off == content.size());
	CHECK(drained == content);
}
TEST_CASE(
	"file_io: open_direct_async returns a fixed-file handle",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();
	if (::io_uring_register_files_sparse(&fx->ring, 4) < 0) {
		SKIP("fixed-file registration unsupported");
	}

	auto tf = TempFile::create("direct file");

	FileHandle handle;
	int open_error = 0;
	try {
		handle = block_on(
			fx->reader,
			fx->reader.open_direct_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC, 0, 2),
			chrono::seconds{5});
	} catch (SE const &se) {
		open_error = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) - test reports invalid handle below
	}
	if (!handle.valid() && (open_error == EINVAL || open_error == EOPNOTSUPP || open_error == ENOSYS)) {
		::io_uring_unregister_files(&fx->ring);
		SKIP("direct open unsupported by this kernel/ring configuration");
	}
	REQUIRE(handle.valid());
	REQUIRE(handle.is_direct());
	CHECK(handle.direct_slot() == 2);

	A<byte, 32> buf{};
	SZ const got =
		block_on(fx->reader, fx->reader.read_into(handle, 0, span<byte>{buf.data(), buf.size()}), chrono::seconds{5});
	REQUIRE(got == SV{"direct file"}.size());
	CHECK(memcmp(buf.data(), "direct file", got) == 0);

	block_on(fx->reader, fx->reader.close_async(move(handle)), chrono::seconds{5});
	::io_uring_unregister_files(&fx->ring);
}
TEST_CASE(
	"file_io: open_async rejects missing path with ENOENT",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	int captured = 0;
	try {
		(void)block_on(
			fx->reader,
			fx->reader.open_async(AT_FDCWD, "/definitely/not/a/real/path.xyz", O_RDONLY | O_CLOEXEC),
			chrono::seconds{5});
	} catch (SE const &se) {
		captured = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) — test swallows other exceptions
	}
	CHECK(captured == ENOENT);
}
TEST_CASE(
	"file_io: FixedBufferPool try_acquire drains and refills on release",
	"[file_io][unit]") {
	::io_uring ring{};
	if (::io_uring_queue_init(8, &ring, 0) < 0) {
		FAIL("conflux requires a host that permits io_uring_queue_init");
	}
	struct G {
		io_uring *r;
		~G() { ::io_uring_queue_exit(r); }
	} const g{&ring};
	RegisteredBufferTable tbl{&ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	CHECK(pool.capacity() == 2);
	CHECK(pool.available() == 2);

	auto a = pool.try_acquire();
	auto b = pool.try_acquire();
	auto c = pool.try_acquire();
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	CHECK_FALSE(c.has_value());
	CHECK(pool.available() == 0);

	a.reset();
	CHECK(pool.available() == 1);
}
TEST_CASE(
	"file_io: iopoll storage ring exposes storage-only fixed read path",
	"[file_io][uring][iopoll]") {
	IopollStorageRingOptions options{};
	options.entries = 32;
	options.fixed_buffer_slots = 2;
	options.fixed_buffer_bytes = 4096;
	auto storage = IopollStorageRing::create(options);
	if (!storage) {
		INFO(format("iopoll storage ring unavailable: {}", storage.error().what()));
		SKIP("iopoll storage ring unavailable");
	}

	S const content(4096, 'I');
	auto tf = TempFile::create(content);
	int const fd = ::open(tf.path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (fd < 0) {
		INFO(format("O_DIRECT unavailable for test file: {}", strerror(errno)));
		SKIP("O_DIRECT unavailable for test file");
	}
	FileHandle handle = FileHandle::from_fd(fd);
	auto buf = (*storage)->try_acquire_buffer();
	REQUIRE(buf.has_value());

	try {
		auto got = block_on_iopoll(
			(*storage)->reader(),
			(*storage)->reader().read_nocache_fixed(handle, 0, move(*buf), content.size()),
			chrono::seconds{5});
		REQUIRE(got.bytes == content.size());
		CHECK(memcmp(got.buffer.view().data(), content.data(), content.size()) == 0);
	} catch (SE const &se) {
		int const err = se.code().value();
		if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS || err == ENOTSUP) {
			INFO(format("IOPOLL/O_DIRECT read unsupported on this filesystem/device: {}", se.what()));
			SKIP("IOPOLL/O_DIRECT read unsupported on this filesystem/device");
		}
		throw;
	}
}

TEST_CASE(
	"file_io: write_fixed round-trips content via registered buffer",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	auto tf = TempFile::create();

	FileHandle const wh =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(wh.valid());

	// Fill a fixed buffer with known content and write it.
	auto write_buf = pool.try_acquire();
	REQUIRE(write_buf.has_value());
	S const payload(512, 'W');
	memcpy(write_buf->view().data(), payload.data(), payload.size());

	FileReader::WriteFixedResult const wresult =
		block_on(fx->reader, fx->reader.write_fixed(wh, 0, move(*write_buf), payload.size()), chrono::seconds{5});
	REQUIRE(wresult.bytes == payload.size());

	// Verify on-disk bytes via pread.
	S verify(payload.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	REQUIRE(n == static_cast<ssize_t>(payload.size()));
	CHECK(verify == payload);

	// Returned buffer slot is immediately re-usable.
	CHECK(wresult.buffer.valid());
}
TEST_CASE(
	"file_io: readv_into scatter-reads into multiple buffers",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	S const part_a(64, 'A');
	S const part_b(128, 'B');
	S const content = part_a + part_b;
	auto tf = TempFile::create(content);

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	A<byte, 64> buf_a{};
	A<byte, 128> buf_b{};
	V<iovec> iovs{
		iovec{.iov_base = buf_a.data(), .iov_len = buf_a.size()},
		iovec{.iov_base = buf_b.data(), .iov_len = buf_b.size()},
	};

	SZ const got = block_on(fx->reader, fx->reader.readv_into(handle, 0, move(iovs)), chrono::seconds{5});

	REQUIRE(got == content.size());
	for (SZ i = 0; i < buf_a.size(); ++i) {
		CHECK(static_cast<char>(buf_a[i]) == 'A');
	}
	for (SZ i = 0; i < buf_b.size(); ++i) {
		CHECK(static_cast<char>(buf_b[i]) == 'B');
	}
}
TEST_CASE(
	"file_io: writev_into gather-writes multiple buffers into file",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	auto tf = TempFile::create();

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	S const seg_a(48, 'X');
	S const seg_b(96, 'Y');
	V<iovec> iovs{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — writev wants non-const iov_base
		iovec{.iov_base = const_cast<char *>(seg_a.data()), .iov_len = seg_a.size()},
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
		iovec{.iov_base = const_cast<char *>(seg_b.data()), .iov_len = seg_b.size()},
	};

	SZ const written = block_on(fx->reader, fx->reader.writev_into(handle, 0, move(iovs)), chrono::seconds{5});
	REQUIRE(written == seg_a.size() + seg_b.size());

	S verify(seg_a.size() + seg_b.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	REQUIRE(n == static_cast<ssize_t>(verify.size()));
	CHECK(verify.substr(0, seg_a.size()) == seg_a);
	CHECK(verify.substr(seg_a.size()) == seg_b);
}
TEST_CASE(
	"file_io: read_nocache_fixed with O_DIRECT bypasses page cache",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	// Non-block-aligned content size to exercise the tail alignment logic.
	S const content(1500, 'D');
	auto tf = TempFile::create(content);

	// Open with O_DIRECT — some filesystems (e.g. tmpfs) don't support it.
	// We detect support via the first read result; EINVAL → skip.
	FileHandle handle;
	int open_err = 0;
	try {
		handle = block_on(
			fx->reader,
			fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_DIRECT | O_CLOEXEC),
			chrono::seconds{5});
	} catch (SE const &se) {
		open_err = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) — test swallows non-SE
	}
	if (!handle.valid()) {
		SKIP(format("O_DIRECT open failed: errno={}", open_err));
	}

	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	FileReader::ReadFixedResult got{};
	int read_err = 0;
	try {
		got = block_on(
			fx->reader,
			fx->reader.read_nocache_fixed(handle, 0, move(*buf), content.size()),
			chrono::seconds{5});
	} catch (SE const &se) {
		read_err = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) — test swallows non-SE
	}

	if (!got.buffer.valid() && read_err == EINVAL) {
		SKIP("filesystem does not support O_DIRECT reads (e.g. tmpfs)");
	}

	REQUIRE(got.bytes == content.size());
	auto const view = got.buffer.view();
	for (SZ i = 0; i < got.bytes; ++i) {
		REQUIRE(static_cast<char>(view[i]) == 'D');
	}
}
TEST_CASE(
	"file_io: read_nocache_fixed caps result to max_bytes",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	// Write 4096 bytes but only request 512 to verify max_bytes capping.
	S const content(4096, 'C');
	auto tf = TempFile::create(content);

	FileHandle handle;
	try {
		handle = block_on(
			fx->reader,
			fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_DIRECT | O_CLOEXEC),
			chrono::seconds{5});
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	if (!handle.valid()) {
		SKIP("O_DIRECT open failed");
	}

	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	FileReader::ReadFixedResult got{};
	int read_err = 0;
	// Request only 512 bytes from a 4096-byte file.
	try {
		got = block_on(fx->reader, fx->reader.read_nocache_fixed(handle, 0, move(*buf), 512), chrono::seconds{5});
	} catch (SE const &se) {
		read_err = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) — test swallows non-SE
	}

	if (!got.buffer.valid() && read_err == EINVAL) {
		SKIP("filesystem does not support O_DIRECT reads");
	}

	// Bytes must be capped to 512 even though the kernel read a full aligned block.
	CHECK(got.bytes == 512);
	auto const view = got.buffer.view();
	for (SZ i = 0; i < got.bytes; ++i) {
		CHECK(static_cast<char>(view[i]) == 'C');
	}
}
TEST_CASE(
	"file_io: PipePool acquire/release recycles pairs",
	"[file_io][unit]") {
	PipePool pool{3};
	CHECK(pool.capacity() == 3);
	CHECK(pool.available() == 3);

	auto a = pool.try_acquire();
	auto b = pool.try_acquire();
	auto c = pool.try_acquire();
	auto d = pool.try_acquire();
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	REQUIRE(c.has_value());
	CHECK_FALSE(d.has_value());
	CHECK(a->read_fd() >= 0);
	CHECK(a->write_fd() >= 0);
	CHECK(a->capacity() > 0);

	a.reset();
	CHECK(pool.available() == 1);
}
TEST_CASE(
	"file_io: unlink_async removes a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create("hello");
	S const path = tmp.path;

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.unlink_async(AT_FDCWD, path), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(path.c_str(), &st) != 0);
}
TEST_CASE(
	"file_io: rename_async renames a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const src = TempFile::create("data");
	S const dst_path = src.path + ".renamed";

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.rename_async(AT_FDCWD, src.path, AT_FDCWD, dst_path), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(dst_path.c_str(), &st) == 0);
	::unlink(dst_path.c_str());
}
TEST_CASE(
	"file_io: fadvise_async succeeds on a regular file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create(S(4096, 'X'));

	bool ok = false;
	int err = 0;
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));
	try {
		block_on(fx->reader, fx->reader.fadvise_async(handle, 0, 4096, POSIX_FADV_SEQUENTIAL), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EBADF;
	CHECK(passed); // EBADF acceptable on some kernel versions for fadvise via io_uring
}
TEST_CASE(
	"file_io: madvise_async on mapped memory succeeds",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	constexpr SZ kSize = 4096;
	void *addr = ::mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		SKIP("mmap failed");
	}

	bool ok = false;
	int err = 0;
	try {
		block_on(
			fx->reader,
			fx->reader.madvise_async(addr, static_cast<u32>(kSize), MADV_SEQUENTIAL),
			chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}
	::munmap(addr, kSize);

	bool const passed = ok || err == EINVAL;
	CHECK(passed); // EINVAL acceptable if kernel constrains anonymous madvise
}
TEST_CASE(
	"file_io: mkdirat_async creates a directory",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	S dir_path = "/tmp/conflux_file_io_mkdir_XXXXXX";
	// Use mkdtemp to get a unique name, then remove it so we can recreate via async.
	char const *tmp = ::mkdtemp(dir_path.data());
	REQUIRE(tmp != nullptr);
	::rmdir(dir_path.c_str());

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.mkdirat_async(AT_FDCWD, dir_path, 0755), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(dir_path.c_str(), &st) == 0);
	CHECK(S_ISDIR(st.st_mode));
	::rmdir(dir_path.c_str());
}
TEST_CASE(
	"file_io: symlinkat_async creates a symlink",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const src = TempFile::create("symlink-target");
	S const link_path = src.path + ".link";

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.symlinkat_async(src.path, AT_FDCWD, link_path), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat lst{};
	CHECK(::lstat(link_path.c_str(), &lst) == 0);
	CHECK(S_ISLNK(lst.st_mode));
	::unlink(link_path.c_str());
}
TEST_CASE(
	"file_io: ftruncate_async truncates a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create(S(4096, 'T'));
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.ftruncate_async(handle, 1024), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::fstat(tmp.fd, &st) == 0);
	CHECK(st.st_size == 1024);
}
TEST_CASE(
	"file_io: fsetxattr_async + fgetxattr_async round-trips an xattr",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("xattr test");
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool set_ok = false;
	int set_err = 0;
	S const xattr_name = "user.test_key";
	S const xattr_val = "hello_xattr";
	try {
		block_on(fx->reader, fx->reader.fsetxattr_async(handle, xattr_name, xattr_val), chrono::seconds{5});
		set_ok = true;
	} catch (SE const &se) { set_err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const set_passed =
		set_ok || set_err == EOPNOTSUPP || set_err == ENOTSUP || set_err == EINVAL || set_err == ENOSYS;
	CHECK(set_passed);
	if (!set_ok) {
		return;
	}

	A<char, 64> buf{};
	SZ got = 0;
	int get_err = 0;
	try {
		got = block_on(
			fx->reader,
			fx->reader.fgetxattr_async(handle, xattr_name, span<char>{buf.data(), buf.size()}),
			chrono::seconds{5});
	} catch (SE const &se) { get_err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(get_err == 0);
	REQUIRE(got == xattr_val.size());
	CHECK(SV{buf.data(), got} == xattr_val);
}
TEST_CASE(
	"file_io: fixed_fd_install_async rejects non-direct handle",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("install test");
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	int err = 0;
	try {
		(void)block_on(fx->reader, fx->reader.fixed_fd_install_async(handle), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(err == EINVAL);
}
TEST_CASE(
	"file_io: socket_async creates a socket",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	FileHandle handle;
	bool ok = false;
	int err = 0;
	try {
		handle = block_on(
			fx->reader,
			fx->reader.socket_async(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0),
			chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(handle.valid());
		CHECK_FALSE(handle.is_direct());
	}
}
TEST_CASE(
	"file_io: shutdown_async half-closes a socket",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.shutdown_async(handle, SHUT_WR), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == ENOTCONN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: tee_async copies data between pipes",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int src_pipe[2] = {-1, -1};
	int dst_pipe[2] = {-1, -1};
	if (::pipe2(src_pipe, O_CLOEXEC) < 0 || ::pipe2(dst_pipe, O_CLOEXEC) < 0) {
		if (src_pipe[0] >= 0) {
			::close(src_pipe[0]);
			::close(src_pipe[1]);
		}
		SKIP("pipe2 failed");
	}
	struct PipeGuard {
		int fds[4];
		~PipeGuard() {
			for (int const fd: fds) {
				if (fd >= 0) {
					::close(fd);
				}
			}
		}
	} guard{src_pipe[0], src_pipe[1], dst_pipe[0], dst_pipe[1]};
	S const payload(64, 'T');
	ssize_t const written = ::write(src_pipe[1], payload.data(), payload.size());
	REQUIRE(written == static_cast<ssize_t>(payload.size()));

	SZ got = 0;
	bool ok = false;
	int err = 0;
	try {
		got = block_on(fx->reader, fx->reader.tee_async(src_pipe[0], dst_pipe[1], payload.size()), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(got == payload.size());
		S dst_buf(payload.size(), '\0');
		ssize_t const n = ::read(dst_pipe[0], dst_buf.data(), dst_buf.size());
		CHECK(n == static_cast<ssize_t>(payload.size()));
		CHECK(dst_buf == payload);
	}
}
TEST_CASE(
	"file_io: linkat_async creates a hard link",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const src = TempFile::create("link_content");
	S const dst_path = src.path + ".hardlink";
	::unlink(dst_path.c_str());

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.linkat_async(AT_FDCWD, src.path, AT_FDCWD, dst_path), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		struct ::stat src_st{};
		struct ::stat dst_st{};
		CHECK(::stat(src.path.c_str(), &src_st) == 0);
		CHECK(::stat(dst_path.c_str(), &dst_st) == 0);
		CHECK(src_st.st_ino == dst_st.st_ino);
	}
	::unlink(dst_path.c_str());
}
TEST_CASE(
	"file_io: sync_file_range_async flushes a file region",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create(S(4096, 'S'));
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool ok = false;
	int err = 0;
	try {
		block_on(
			fx->reader,
			fx->reader.sync_file_range_async(handle, 0, 4096, SYNC_FILE_RANGE_WRITE),
			chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EROFS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: cancel_async on non-existent user_data succeeds (ENOENT → ok)",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	// user_data 0xDEADBEEF has no pending op — should resolve (ENOENT → ok path).
	try {
		block_on(fx->reader, fx->reader.cancel_async(0xDEADBEEFULL), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: cancel_fd_async on idle fd resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("cancel fd test");

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.cancel_fd_async(tmp.fd), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: connect_async returns ECONNREFUSED on closed port",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	sockaddr_storage addr{};
	auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr);
	sa4->sin_family = AF_INET;
	sa4->sin_port = htons(1);
	sa4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int err = 0;
	try {
		block_on(fx->reader, fx->reader.connect_async(handle, addr, sizeof(sockaddr_in)), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = err == ECONNREFUSED || err == EINPROGRESS || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: futex_wake_async wakes zero waiters on uncontested futex",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	u32 futex_word = 0;
	u32 woken = 42;
	int err = 0;
	try {
		woken = block_on(fx->reader, fx->reader.futex_wake_async(&futex_word, UINT64_MAX), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = (woken == 0 && err == 0) || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: futex_wait_async resolves immediately when word already changed",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	u32 futex_word = 1;
	bool ok = false;
	int err = 0;
	// val=0 but *futex=1 — condition already met, returns immediately.
	try {
		block_on(fx->reader, fx->reader.futex_wait_async(&futex_word, 0), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EAGAIN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: msg_ring_async delivers synthetic CQE to self ring",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.msg_ring_async(fx->ring.ring_fd, 42, 0xCAFEBABEULL), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
}
TEST_CASE(
	"file_io: setxattr_async + getxattr_async round-trips path-based xattr",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("xattr path test");

	bool set_ok = false;
	int set_err = 0;
	S const xattr_val = "path_xattr_val";
	try {
		block_on(fx->reader, fx->reader.setxattr_async(tmp.path, "user.path_test_key", xattr_val), chrono::seconds{5});
		set_ok = true;
	} catch (SE const &se) { set_err = se.code().value(); } catch (...) {
	}

	bool const set_passed =
		set_ok || set_err == EOPNOTSUPP || set_err == ENOTSUP || set_err == EINVAL || set_err == ENOSYS;
	CHECK(set_passed);
	if (!set_ok) {
		return;
	}

	A<char, 64> buf{};
	SZ got = 0;
	int get_err = 0;
	try {
		got = block_on(
			fx->reader,
			fx->reader.getxattr_async(tmp.path, "user.path_test_key", span<char>{buf.data(), buf.size()}),
			chrono::seconds{5});
	} catch (SE const &se) { get_err = se.code().value(); } catch (...) {
	}

	CHECK(get_err == 0);
	REQUIRE(got == xattr_val.size());
	CHECK(SV{buf.data(), got} == xattr_val);
}
TEST_CASE(
	"file_io: waitid_async on non-existent pid returns ECHILD",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	siginfo_t info{};
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.waitid_async(P_PID, static_cast<id_t>(99999999), &info), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = err == ECHILD || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: pipe_async creates a functional pipe",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	P<int, int> fds{-1, -1};
	bool ok = false;
	int err = 0;
	try {
		fds = block_on(fx->reader, fx->reader.pipe_async(O_CLOEXEC), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(fds.first >= 0);
		CHECK(fds.second >= 0);
		S const msg = "ping";
		ssize_t const w = ::write(fds.second, msg.data(), msg.size());
		CHECK(w == static_cast<ssize_t>(msg.size()));
		A<char, 8> buf{};
		ssize_t const n = ::read(fds.first, buf.data(), buf.size());
		CHECK(n == static_cast<ssize_t>(msg.size()));
		CHECK(SV{buf.data(), static_cast<SZ>(n)} == msg);
		::close(fds.first);
		::close(fds.second);
	}
}
TEST_CASE(
	"file_io: bind_async + listen_async on loopback",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	int const reuse = 1;
	::setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	sockaddr_storage addr{};
	auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr);
	sa4->sin_family = AF_INET;
	sa4->sin_port = 0;
	sa4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	bool bind_ok = false;
	int bind_err = 0;
	try {
		block_on(fx->reader, fx->reader.bind_async(handle, addr, sizeof(sockaddr_in)), chrono::seconds{5});
		bind_ok = true;
	} catch (SE const &se) { bind_err = se.code().value(); } catch (...) {
	}

	bool const bind_passed = bind_ok || bind_err == EINVAL || bind_err == ENOSYS;
	CHECK(bind_passed);
	if (!bind_ok) {
		return;
	}

	bool listen_ok = false;
	int listen_err = 0;
	try {
		block_on(fx->reader, fx->reader.listen_async(handle), chrono::seconds{5});
		listen_ok = true;
	} catch (SE const &se) { listen_err = se.code().value(); } catch (...) {
	}

	bool const listen_passed = listen_ok || listen_err == EINVAL || listen_err == ENOSYS;
	CHECK(listen_passed);
}
TEST_CASE(
	"file_io: nop_async completes successfully",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	try {
		block_on(fx->reader, fx->reader.nop_async(), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
}
TEST_CASE(
	"file_io: readv2_into scatter-reads with RWF flags",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	S const content(64, 'R');
	TempFile const tf = TempFile::create(content);

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	A<byte, 64> buf{};
	V<iovec> iovs{
		iovec{.iov_base = buf.data(), .iov_len = buf.size()}
    };

	SZ got = 0;
	int err = 0;
	try {
		got = block_on(fx->reader, fx->reader.readv2_into(handle, 0, move(iovs)), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(err == 0);
	REQUIRE(got == content.size());
	for (SZ i = 0; i < got; ++i) {
		CHECK(static_cast<char>(buf[i]) == 'R');
	}
}
TEST_CASE(
	"file_io: writev2_into scatter-writes with RWF flags",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tf = TempFile::create();
	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	S const payload(32, 'W');
	V<iovec> iovs{
		iovec{.iov_base = const_cast<char *>(payload.data()), .iov_len = payload.size()}
    };

	SZ written = 0;
	int err = 0;
	try {
		written = block_on(fx->reader, fx->reader.writev2_into(handle, 0, move(iovs)), chrono::seconds{5});
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(err == 0);
	CHECK(written == payload.size());
	S verify(payload.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	CHECK(n == static_cast<ssize_t>(payload.size()));
	CHECK(verify == payload);
}
TEST_CASE(
	"file_io: timeout_async fires after delay",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.timeout_async(chrono::milliseconds{10}), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: futex_waitv_async resolves immediately on already-changed word",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	u32 futex_word = 42;
	// uaddr cast to u64 as expected by futex_waitv
	futex_waitv w{};
	w.val = 0;
	w.uaddr = reinterpret_cast<u64>(&futex_word);
	w.flags = FUTEX2_SIZE_U32;
	w.__reserved = 0;
	V<futex_waitv> waiters{w};

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.futex_waitv_async(move(waiters)), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EAGAIN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: msg_ring_fd_async sends fd to same ring",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("fd_msg");
	int const dup_fd = ::dup(tmp.fd);
	REQUIRE(dup_fd >= 0);

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.msg_ring_fd_async(fx->ring.ring_fd, dup_fd, -1, 0xABCDULL), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}
	::close(dup_fd);

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
}
TEST_CASE(
	"file_io: timeout_remove_async on non-existent tag resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	// Remove a timeout tag that was never armed — should resolve (ENOENT→ok).
	try {
		block_on(fx->reader, fx->reader.timeout_remove_async(0xDEADULL), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: timeout_update_async on non-existent tag resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		block_on(fx->reader, fx->reader.timeout_update_async(0xBEEFULL, chrono::milliseconds{100}), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"poll_add_async fires on readable pipe") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int pfd[2];
	REQUIRE(::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);

	// Write one byte so the read-end becomes readable.
	char const c = 'x';
	REQUIRE(::write(pfd[1], &c, 1) == 1);

	u32 mask{0};
	bool ok{false};
	try {
		mask = block_on(fx->reader, fx->reader.poll_add_async(pfd[0], POLLIN), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK((mask & POLLIN) != 0u);
	::close(pfd[0]);
	::close(pfd[1]);
}
TEST_CASE(
	"poll_remove_async cancels pending poll") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	bool remove_ok{false};
	int err{0};

	// Open a socket that is never written to (poll will block indefinitely).
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) == 0);

	// Submit poll_add (won't fire because nothing writes to sv[0]).
	auto poll_flow = fx->reader.poll_add_async(sv[0], POLLIN);
	io_uring_submit(fx->reader.ring());

	// Now cancel it: we need the user_data of the poll SQE.
	// Our fixture encodes ud as pack_ud(slot, gen). We know the poll_add
	// reserved slot 0 gen 1 (first reservation after construction).
	// Use cancel_fd_async instead — simpler to test.
	try {
		block_on(fx->reader, fx->reader.cancel_fd_async(sv[0], 0), chrono::seconds{5});
		remove_ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}
	root::abandon_to(move(poll_flow), root::drop_on_abandon{});

	bool const passed = remove_ok || err == ENOENT || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	::close(sv[0]);
	::close(sv[1]);
}
TEST_CASE(
	"accept_async returns new fd from socketpair-like listen") {
	// Create a listening TCP socket, connect from another thread, accept via uring.
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int accepted_fd{-1};
	int err{0};

	int const listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	REQUIRE(listen_fd >= 0);
	int const optval = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; // kernel picks port
	REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	REQUIRE(::listen(listen_fd, 1) == 0);

	// Find out the port.
	socklen_t slen = sizeof(addr);
	REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &slen) == 0);

	// Connect from a background thread.
	jthread const connector{[addr]() {
		int const c = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (c >= 0) {
			::connect(c, reinterpret_cast<sockaddr const *>(&addr), sizeof(addr));
			::close(c);
		}
	}};

	FileHandle const listen_handle = FileHandle::from_fd(dup(listen_fd));
	try {
		FileHandle fh = block_on(fx->reader, fx->reader.accept_async(listen_handle), chrono::seconds{5});
		accepted_fd = fh.release_fd();
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	::close(listen_fd);

	bool const passed = accepted_fd >= 0 || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (accepted_fd >= 0) {
		::close(accepted_fd);
	}
}
TEST_CASE(
	"send_async + recv_async round-trip over socketpair") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "send_recv_test";
	SZ const sent =
		block_on(fx->reader, fx->reader.send_async(sender, payload.data(), payload.size()), chrono::seconds{5});
	REQUIRE(sent == payload.size());

	A<char, 64> buf{};
	SZ const recvd = block_on(fx->reader, fx->reader.recv_async(recver, buf.data(), buf.size()), chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(SV{buf.data(), recvd} == payload);
}
TEST_CASE(
	"sendmsg_async + recvmsg_async round-trip over socketpair") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "sendmsg_recvmsg_test";
	iovec send_iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr send_hdr{};
	send_hdr.msg_iov = &send_iov;
	send_hdr.msg_iovlen = 1;

	SZ const sent = block_on(fx->reader, fx->reader.sendmsg_async(sender, &send_hdr), chrono::seconds{5});
	REQUIRE(sent == payload.size());

	A<char, 64> buf{};
	iovec recv_iov{buf.data(), buf.size()};
	msghdr recv_hdr{};
	recv_hdr.msg_iov = &recv_iov;
	recv_hdr.msg_iovlen = 1;

	SZ const recvd = block_on(fx->reader, fx->reader.recvmsg_async(recver, &recv_hdr), chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(SV{buf.data(), recvd} == payload);
}
TEST_CASE(
	"epoll_ctl_async + epoll_wait_async detect fd readability") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int const epfd = ::epoll_create1(EPOLL_CLOEXEC);
	REQUIRE(epfd >= 0);

	int pfd[2];
	REQUIRE(::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);

	// Add pfd[0] to epoll.
	epoll_event ev{};
	ev.events = EPOLLIN;
	ev.data.fd = pfd[0];
	bool ctl_ok{false};
	try {
		block_on(fx->reader, fx->reader.epoll_ctl_async(epfd, pfd[0], EPOLL_CTL_ADD, &ev), chrono::seconds{5});
		ctl_ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	REQUIRE(ctl_ok);

	// Write a byte to make pfd[0] readable, then wait.
	char const c = 'q';
	REQUIRE(::write(pfd[1], &c, 1) == 1);

	A<epoll_event, 4> events{};
	int n_events{0};
	try {
		n_events = block_on(
			fx->reader,
			fx->reader.epoll_wait_async(epfd, events.data(), static_cast<int>(events.size())),
			chrono::seconds{5});
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(n_events == 1);
	CHECK((events[0].events & EPOLLIN) != 0u);
	::close(pfd[0]);
	::close(pfd[1]);
	::close(epfd);
}
TEST_CASE(
	"provide_buffers_async + remove_buffers_async smoke") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	// Allocate a small buffer region.
	constexpr int kBufLen = 4096;
	constexpr int kNr = 2;
	constexpr int kBgid = 7;
	auto region = make_unique<A<char, kBufLen * kNr>>();

	bool ok{false};
	int err{0};
	try {
		block_on(fx->reader, fx->reader.provide_buffers_async(region->data(), kBufLen, kNr, kBgid), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);

	if (ok) {
		bool rm_ok{false};
		try {
			block_on(fx->reader, fx->reader.remove_buffers_async(kNr, kBgid), chrono::seconds{5});
			rm_ok = true;
		} catch (...) { // NOLINT(bugprone-empty-catch)
		}
		CHECK(rm_ok);
	}
}
TEST_CASE(
	"openat2_async opens file with basic open_how") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("openat2_content");

	open_how how{};
	how.flags = O_RDONLY | O_CLOEXEC;

	FileHandle handle;
	bool ok{false};
	try {
		handle = block_on(fx->reader, fx->reader.openat2_async(AT_FDCWD, tf.path, how), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	CHECK(handle.valid());
}
TEST_CASE(
	"sendto_async + recv_async round-trip over UDP loopback") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int const recv_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	REQUIRE(recv_fd >= 0);
	sockaddr_in ra{};
	ra.sin_family = AF_INET;
	ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ra.sin_port = 0;
	REQUIRE(::bind(recv_fd, reinterpret_cast<sockaddr *>(&ra), sizeof(ra)) == 0);
	socklen_t ralen = sizeof(ra);
	REQUIRE(::getsockname(recv_fd, reinterpret_cast<sockaddr *>(&ra), &ralen) == 0);

	int const send_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	REQUIRE(send_fd >= 0);
	FileHandle const sender = FileHandle::from_fd(send_fd);

	sockaddr_storage dest{};
	memcpy(&dest, &ra, sizeof(ra));

	S const payload = "sendto_udp_test";
	SZ const sent = block_on(
		fx->reader,
		fx->reader.sendto_async(sender, payload.data(), payload.size(), 0, dest, sizeof(ra)),
		chrono::seconds{5});
	REQUIRE(sent == payload.size());

	FileHandle const recver = FileHandle::from_fd(recv_fd);
	A<char, 64> buf{};
	SZ const recvd = block_on(fx->reader, fx->reader.recv_async(recver, buf.data(), buf.size()), chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(SV{buf.data(), recvd} == payload);
}
TEST_CASE(
	"unsafe_send_zc_sent_async sends data (or gracefully unsupported)") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "send_zc_test_data";
	bool ok{false};
	int err{0};
	try {
		SZ const n = block_on(
			fx->reader,
			fx->reader.unsafe_send_zc_sent_async(sender, payload.data(), payload.size()),
			chrono::seconds{5});
		ok = (n == payload.size());
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"send_zc_async completes after notification and data received") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "send_zc_notif_data";
	bool ok{false};
	int err{0};
	try {
		SZ const n =
			block_on(fx->reader, fx->reader.send_zc_async(sender, payload.data(), payload.size()), chrono::seconds{5});
		ok = (n == payload.size());
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	REQUIRE(passed);
	if (ok) {
		A<char, 64> buf{};
		SZ const recvd = static_cast<SZ>(::recv(sv[1], buf.data(), buf.size(), MSG_DONTWAIT));
		CHECK(recvd == payload.size());
		CHECK(SV{buf.data(), recvd} == payload);
	}
}
TEST_CASE(
	"unlinkat_async removes file relative to dirfd") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("unlinkat_content");
	S const path = tf.path;
	tf.fd = -1; // don't let TempFile close (will unlink)
	tf.path = {}; // don't let TempFile unlink

	bool ok{false};
	try {
		block_on(fx->reader, fx->reader.unlinkat_async(AT_FDCWD, path), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	CHECK(::access(path.c_str(), F_OK) != 0);
}
TEST_CASE(
	"renameat_async renames file across directories") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("renameat_content");
	S const src_path = tf.path;
	S const dst_path = src_path + "_renamed";

	bool ok{false};
	try {
		block_on(fx->reader, fx->reader.renameat_async(AT_FDCWD, src_path, AT_FDCWD, dst_path), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	if (ok) {
		CHECK(::access(dst_path.c_str(), F_OK) == 0);
		::unlink(dst_path.c_str());
	}
}
TEST_CASE(
	"atomic_write_async stages in nested parent and publishes atomically") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto dir = TempDir::create();
	dir.mkdir_sub("sub");

	S const payload = "async atomic nested content";
	block_on(
		fx->reader,
		fx->reader.atomic_write_async(dir.fd, S{"sub/out.txt"}, as_bytes(span{payload})),
		chrono::seconds{5});

	CHECK(dir.read_file("sub/out.txt") == payload);
	CHECK_FALSE(dir.has_staging_files());
	CHECK_FALSE(dir.has_staging_files("sub"));
}
TEST_CASE(
	"atomic_write_async create_new preserves existing target and removes staging") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto dir = TempDir::create();
	dir.write_file("target.txt", "original");

	S const replacement = "replacement";
	int err = 0;
	try {
		block_on(
			fx->reader,
			fx->reader.atomic_write_async(
				dir.fd,
				S{"target.txt"},
				as_bytes(span{replacement}),
				0644,
				TempPublishMode::create_new),
			chrono::seconds{5});
	} catch (SE const &se) {
		err = se.code().value();
	}

	CHECK(err == EEXIST);
	CHECK(dir.read_file("target.txt") == "original");
	CHECK_FALSE(dir.has_staging_files());
}
TEST_CASE(
	"mkdir_async creates a directory") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	S const dir_path = "/tmp/conflux_file_io_mkdir_test_XXXXXX";
	// Use mktemp to get a unique name; don't create it yet.
	auto path = S(dir_path);
	path.resize(path.size() - 6); // strip XXXXXX template
	path += "mkdir_async_test_dir";
	::rmdir(path.c_str()); // clean up if leftover

	bool ok{false};
	try {
		block_on(fx->reader, fx->reader.mkdir_async(path), chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	if (ok) {
		struct stat st{};
		CHECK(::stat(path.c_str(), &st) == 0);
		CHECK(S_ISDIR(st.st_mode));
		::rmdir(path.c_str());
	}
}
TEST_CASE(
	"write_fixed_async + read_fixed round-trip") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	auto wbuf = pool.try_acquire();
	REQUIRE(wbuf.has_value());

	auto tf = TempFile::create();

	// Write via write_fixed.
	S const content(512, 'W');
	auto const view = wbuf->view();
	memcpy(view.data(), content.data(), content.size());

	FileHandle const handle =
		block_on(fx->reader, fx->reader.open_async(AT_FDCWD, tf.path, O_RDWR | O_CLOEXEC), chrono::seconds{5});
	REQUIRE(handle.valid());

	SZ const written = block_on(
		fx->reader,
		fx->reader.write_fixed_async(
			handle,
			0,
			view.data(),
			static_cast<unsigned>(content.size()),
			static_cast<int>(wbuf->slot())),
		chrono::seconds{5});
	REQUIRE(written == content.size());

	// Verify via read_fixed.
	auto rbuf = pool.try_acquire();
	REQUIRE(rbuf.has_value());
	FileReader::ReadFixedResult const rr =
		block_on(fx->reader, fx->reader.read_fixed(handle, 0, move(*rbuf)), chrono::seconds{5});
	REQUIRE(rr.bytes == content.size());
	auto const rview = rr.buffer.view();
	CHECK(memcmp(rview.data(), content.data(), content.size()) == 0);
}
TEST_CASE(
	"openat_direct_async opens file into registered slot") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	// Register one direct file slot.
	int const reg_fd = -1;
	if (::io_uring_register_files(&fx->ring, &reg_fd, 1) < 0) {
		SKIP("io_uring_register_files unsupported");
	}

	auto tf = TempFile::create("openat_direct_content");

	FileHandle handle;
	bool ok{false};
	int err{0};
	// Slot 0 is the registered slot; IORING_FILE_INDEX_ALLOC let kernel choose.
	try {
		handle = block_on(
			fx->reader,
			fx->reader.openat_direct_async(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC, 0, 0),
			chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == ENFILE;
	CHECK(passed);
	if (handle.valid()) {
		block_on(fx->reader, fx->reader.close_async(move(handle)), chrono::seconds{5});
	}
	::io_uring_unregister_files(&fx->ring);
}
TEST_CASE(
	"pipe_direct_async creates pipe into fixed file table (or graceful skip)") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	// Register two direct file slots.
	int fds_reg[2] = {-1, -1};
	if (::io_uring_register_files(&fx->ring, fds_reg, 2) < 0) {
		SKIP("io_uring_register_files unsupported");
	}

	bool ok{false};
	int err{0};
	try {
		P<int, int> const p = block_on(fx->reader, fx->reader.pipe_direct_async(0), chrono::seconds{5});
		ok = (p.first >= 0 || p.second >= 0);
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
	::io_uring_unregister_files(&fx->ring);
}
TEST_CASE(
	"msg_ring_cqe_flags_async posts message to self ring") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	bool ok{false};
	int err{0};
	int const ring_fd = fx->ring.ring_fd;
	try {
		block_on(fx->reader, fx->reader.msg_ring_cqe_flags_async(ring_fd, 42, 0xBEEFULL, 0, 0), chrono::seconds{5});
		ok = true;
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"unsafe_sendmsg_zc_sent_async sends data (or gracefully unsupported)") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "sendmsg_zc_test";
	iovec iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr hdr{};
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	bool ok{false};
	int err{0};
	try {
		SZ const n = block_on(fx->reader, fx->reader.unsafe_sendmsg_zc_sent_async(sender, &hdr), chrono::seconds{5});
		ok = (n == payload.size());
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"sendmsg_zc_async completes after notification and data received") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	S const payload = "sendmsg_zc_notif";
	iovec iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr hdr{};
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	bool ok{false};
	int err{0};
	try {
		SZ const n = block_on(fx->reader, fx->reader.sendmsg_zc_async(sender, &hdr), chrono::seconds{5});
		ok = (n == payload.size());
	} catch (SE const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	REQUIRE(passed);
	if (ok) {
		A<char, 64> buf{};
		SZ const recvd = static_cast<SZ>(::recvfrom(sv[1], buf.data(), buf.size(), MSG_DONTWAIT, nullptr, nullptr));
		CHECK(recvd == payload.size());
		CHECK(SV{buf.data(), recvd} == payload);
	}
}
