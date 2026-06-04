// libFuzzer driver for normalize_static_path and static-file containment.
// Invariants:
//   - accepted paths are absolute, canonicalized server-root-relative paths
//   - no NUL, repeated separators, dot segments, or traversal segments survive
//   - percent-decoded route captures cannot escape the static root
//   - contained open rejects symlinks, magic links, and absolute/traversal paths

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.utils;
import conflux.file_io_sync;
import conflux.net.http.static_core;

using namespace std;
namespace http_detail = conflux::http::detail;
namespace file_sync = conflux::file_io_sync;

namespace {

[[noreturn]] void fuzz_trap() {
	__builtin_trap();
}

void check_normalized_path(
	std::string const &path) {
	if (!path.empty() && path.front() != '/') {
		fuzz_trap();
	}
	if (path.find('\0') != std::string::npos
		|| path.find("//") != std::string::npos
		|| path.find("/./") != std::string::npos
		|| path == "/."
		|| path.find("/../") != std::string::npos
		|| path == "/..") {
		fuzz_trap();
	}
}

void exercise_normalize(
	std::string_view raw) {
	auto const normalized = http_detail::normalize_static_path(raw);
	if (normalized) {
		check_normalized_path(*normalized);
	}
}

std::string percent_encode_byte(
	std::uint8_t value) {
	constexpr std::string_view hex{"0123456789ABCDEF"};
	std::string out;
	out.resize(3);
	out[0] = '%';
	out[1] = hex[(value >> 4U) & 0x0fU];
	out[2] = hex[value & 0x0fU];
	return out;
}

std::string synthesize_path(
	std::span<std::uint8_t const> bytes,
	bool encoded) {
	static constexpr std::array<std::string_view, 16> segments{
		"safe.txt",
		"dir",
		"nested.txt",
		".",
		"..",
		"",
		"symlink-safe",
		"symlink-escape",
		"%2e",
		"%2E%2E",
		"%2f",
		"%5c",
		"%00",
		"%C0%AE",
		"%E2%88%95",
		"%zz"};

	std::string out;
	if (!bytes.empty() && (bytes[0] & 1U) != 0U) {
		out += '/';
	}
	std::size_t const count = std::min<std::size_t>(16, bytes.empty() ? 0 : (bytes[0] >> 1U) + 1U);
	for (std::size_t i = 0; i < count; ++i) {
		if (!out.empty() && out.back() != '/') {
			out += ((i < bytes.size() && (bytes[i] & 0x10U) != 0U) ? "//" : "/");
		}
		auto const b = bytes.empty() ? std::uint8_t{} : bytes[i % bytes.size()];
		auto const seg = segments[b % segments.size()];
		if (encoded && (b & 0x20U) != 0U) {
			for (char const c: seg) {
				auto const byte = static_cast<std::uint8_t>(static_cast<unsigned char>(c));
				out += percent_encode_byte(byte);
			}
		} else {
			out.append(seg.data(), seg.size());
		}
	}
	if (bytes.size() > 1 && (bytes[1] & 0x80U) != 0U) {
		out += '/';
	}
	return out;
}

struct StaticPathFixture {
	std::string dir;
	file_sync::UniqueFd root;
	ino_t safe_ino{};
	dev_t safe_dev{};
	ino_t nested_ino{};
	dev_t nested_dev{};

	StaticPathFixture() {
		std::array<char, 64> tmpl{};
		std::string_view const prefix{"/tmp/conflux_static_path_fuzz_XXXXXX"};
		std::ranges::copy(prefix, tmpl.begin());
		char *made = ::mkdtemp(tmpl.data());
		if (made == nullptr) {
			return;
		}
		dir = made;
		write_file("safe.txt", "safe");
		(void)::mkdir((dir + "/dir").c_str(), 0700);
		write_file("dir/nested.txt", "nested");
		(void)::symlink("safe.txt", (dir + "/symlink-safe").c_str());
		(void)::symlink("/etc/passwd", (dir + "/dir/symlink-escape").c_str());
		(void)::symlink("/etc/passwd", (dir + "/symlink-escape").c_str());
		auto fd = file_sync::blocking_open_directory(dir);
		if (fd) {
			root = std::move(*fd);
		}
		if (root) {
			if (auto safe = file_sync::blocking_openat_contained(root.fd(), "safe.txt", O_RDONLY); safe) {
				struct ::stat st{};
				if (::fstat(safe->fd(), &st) == 0) {
					safe_ino = st.st_ino;
					safe_dev = st.st_dev;
				}
			}
			if (auto nested = file_sync::blocking_openat_contained(root.fd(), "dir/nested.txt", O_RDONLY); nested) {
				struct ::stat st{};
				if (::fstat(nested->fd(), &st) == 0) {
					nested_ino = st.st_ino;
					nested_dev = st.st_dev;
				}
			}
		}
	}

	~StaticPathFixture() {
		if (dir.empty()) {
			return;
		}
		(void)::unlink((dir + "/dir/symlink-escape").c_str());
		(void)::unlink((dir + "/dir/nested.txt").c_str());
		(void)::rmdir((dir + "/dir").c_str());
		(void)::unlink((dir + "/symlink-safe").c_str());
		(void)::unlink((dir + "/symlink-escape").c_str());
		(void)::unlink((dir + "/safe.txt").c_str());
		(void)::rmdir(dir.c_str());
	}

	void write_file(
		std::string_view rel,
		std::string_view body) const {
		auto const path = dir + "/" + std::string{rel};
		int const fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
		if (fd < 0) {
			return;
		}
		(void)::write(fd, body.data(), body.size());
		(void)::close(fd);
	}
};

StaticPathFixture &fixture() {
	static StaticPathFixture fx;
	return fx;
}

void exercise_contained_open(
	std::string_view raw) {
	auto &fx = fixture();
	if (!fx.root) {
		return;
	}
	auto normalized = http_detail::normalize_static_path(raw);
	if (!normalized) {
		return;
	}
	check_normalized_path(*normalized);
	std::string_view rel{*normalized};
	if (rel.starts_with('/')) {
		rel.remove_prefix(1);
	}
	if (rel.empty()) {
		return;
	}
	auto opened = file_sync::blocking_openat_contained(fx.root.fd(), rel, O_RDONLY);
	if (!opened) {
		return;
	}
	struct ::stat st{};
	if (::fstat(opened->fd(), &st) != 0) {
		fuzz_trap();
	}
	if (S_ISDIR(st.st_mode)) {
		return;
	}
	// The root contains exactly two regular non-symlink files reachable by
	// policy: safe.txt and dir/nested.txt. Opening any symlink, absolute path, or
	// escaped target would violate the static-file containment contract.
	bool const safe_file = st.st_dev == fx.safe_dev && st.st_ino == fx.safe_ino;
	bool const nested_file = st.st_dev == fx.nested_dev && st.st_ino == fx.nested_ino;
	if (!safe_file && !nested_file) {
		fuzz_trap();
	}
}

void exercise_case(
	std::string_view raw) {
	exercise_normalize(raw);
	auto const decoded = conflux::utils::url_decode_path(raw);
	exercise_normalize(decoded);
	exercise_contained_open(raw);
	exercise_contained_open(decoded);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size > 64U * 1024U) {
		return 0;
	}
	std::string_view input{reinterpret_cast<char const *>(data), size};
	exercise_case(input);

	std::span<std::uint8_t const> bytes{data, size};
	auto const synthetic_plain = synthesize_path(bytes, false);
	auto const synthetic_encoded = synthesize_path(bytes, true);
	exercise_case(synthetic_plain);
	exercise_case(synthetic_encoded);

	static constexpr std::array<std::string_view, 12> fixed_cases{
		"safe.txt",
		"dir/nested.txt",
		"symlink-safe",
		"symlink-escape",
		"dir/symlink-escape",
		"../outside.txt",
		"%2e%2e/outside.txt",
		"safe.txt/%2e%2e/%2e%2e/outside.txt",
		"////safe.txt",
		"/%2Fetc/passwd",
		"%00safe.txt",
		"dir//./nested.txt"};
	for (auto const c: fixed_cases) {
		exercise_case(c);
	}
	return 0;
}
