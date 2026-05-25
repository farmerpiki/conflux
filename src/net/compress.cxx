module;
#if CONFLUX_HAS_BROTLI
	#include <brotli/encode.h>
#endif
#if CONFLUX_HAS_ZSTD
	#include <zstd.h>
#endif

export module conflux.net.compress;
import std;
import conflux.types;
#if CONFLUX_HAS_ZLIB
import conflux.net.compress.backend.zlib;
#endif
#if CONFLUX_HAS_LIBDEFLATE
import conflux.net.compress.backend.libdeflate;
#endif
#if CONFLUX_HAS_ZLIB_NG
import conflux.net.compress.backend.zlibng;
#endif
#if CONFLUX_HAS_ISAL
import conflux.net.compress.backend.isal;
#endif
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;
export struct CompressOptions {
	// Responses smaller than this (in bytes) are not compressed.
	std::size_t min_body_size{256};
};

export enum class GzipBackend : std::uint8_t {
	auto_select,
	zlib,
	libdeflate,
	zlib_ng,
	isa_l,
};
export [[nodiscard]] std::string_view gzip_backend_name(
	GzipBackend backend) noexcept {
	switch (backend) {
	case GzipBackend::auto_select: return "auto";
	case GzipBackend::zlib       : return "zlib";
	case GzipBackend::libdeflate : return "libdeflate";
	case GzipBackend::zlib_ng    : return "zlib-ng";
	case GzipBackend::isa_l      : return "isa-l";
	}
	return "unknown";
}

export enum class CompressionCalibration : std::uint8_t {
	disabled,
	startup,
};

export enum class DynamicEncodingPreference : std::uint8_t {
	gzip_first,
	zstd_first,
};
// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace compress_detail {

[[nodiscard]] bool gzip_backend_available(
	GzipBackend backend) noexcept {
	switch (backend) {
	case GzipBackend::auto_select: return true;
	case GzipBackend::zlib:
#if CONFLUX_HAS_ZLIB
		return true;
#else
		return false;
#endif
	case GzipBackend::libdeflate:
#if CONFLUX_HAS_LIBDEFLATE
		return true;
#else
		return false;
#endif
	case GzipBackend::zlib_ng:
#if CONFLUX_HAS_ZLIB_NG
		return true;
#else
		return false;
#endif
	case GzipBackend::isa_l:
#if CONFLUX_HAS_ISAL
		return true;
#else
		return false;
#endif
	}
	return false;
}
std::vector<GzipBackend> available_gzip_backends() {
	std::vector<GzipBackend> backends;
#if CONFLUX_HAS_LIBDEFLATE
	backends.push_back(GzipBackend::libdeflate);
#endif
#if CONFLUX_HAS_ZLIB_NG
	backends.push_back(GzipBackend::zlib_ng);
#endif
#if CONFLUX_HAS_ISAL
	backends.push_back(GzipBackend::isa_l);
#endif
#if CONFLUX_HAS_ZLIB
	backends.push_back(GzipBackend::zlib);
#endif
	return backends;
}
bool gzip_supported() noexcept {
#if CONFLUX_HAS_COMPRESS
	return true;
#else
	return false;
#endif
}
bool zstd_supported() noexcept {
#if CONFLUX_HAS_ZSTD
	return true;
#else
	return false;
#endif
}
bool is_compressible(
	std::string_view content_type) {
	return content_type.starts_with("text/")
		|| content_type.starts_with("application/json")
		|| content_type.starts_with("application/xml")
		|| content_type.starts_with("application/javascript")
		|| content_type.starts_with("image/svg+xml");
}
struct DynamicEncodingQs {
	float gzip{-1.0F};
	float zstd{-1.0F};
};
DynamicEncodingQs header_encoding_q_values(
	std::string_view hdr) {
	float q_star = -1.0F;
	DynamicEncodingQs qs{};

	for (auto const token: conflux::http::header_tokens(hdr)) {
		auto const semi = token.find(';');
		auto const name = conflux::http::trim_http_whitespace(token.substr(0, semi));

		float q = 1.0F;
		if (semi != std::string_view::npos) {
			if (auto const eq = token.find('=', semi); eq != std::string_view::npos) {
				auto const key = conflux::http::trim_http_whitespace(token.substr(semi + 1, eq - semi - 1));
				if (conflux::http::ascii_iequals(key, "q")) {
					auto const val = conflux::http::trim_http_whitespace(token.substr(eq + 1));
					std::from_chars(val.data(), val.data() + val.size(), q);
				}
			}
		}

		if (name == "*") {
			q_star = std::max(q_star, q);
		} else if (conflux::http::ascii_iequals(name, "gzip")) {
			qs.gzip = std::max(qs.gzip, q);
		} else if (conflux::http::ascii_iequals(name, "zstd")) {
			qs.zstd = std::max(qs.zstd, q);
		}
	}

	if (qs.gzip < 0.0F) {
		qs.gzip = q_star;
	}
	if (qs.zstd < 0.0F) {
		qs.zstd = q_star;
	}
	return qs;
}
std::string gzip_compress_with_backend(
	GzipBackend backend,
	[[maybe_unused]] std::string_view input) {
	switch (backend) {
#if CONFLUX_HAS_LIBDEFLATE
	case GzipBackend::libdeflate: return conflux::compress_backends::libdeflate_gzip_compress(input);
#endif
#if CONFLUX_HAS_ZLIB_NG
	case GzipBackend::zlib_ng: return conflux::compress_backends::zlib_ng_gzip_compress(input);
#endif
#if CONFLUX_HAS_ISAL
	case GzipBackend::isa_l: return conflux::compress_backends::isal_gzip_compress(input);
#endif
#if CONFLUX_HAS_ZLIB
	case GzipBackend::zlib: return conflux::compress_backends::zlib_gzip_compress(input);
#endif
	case GzipBackend::auto_select: return {};
	default                      : return {};
	}
}
#if CONFLUX_HAS_ZSTD
std::string zstd_compress(std::string_view input);
#endif
GzipBackend benchmark_fastest_gzip_backend() {
	auto const backends = available_gzip_backends();
	if (backends.empty()) {
		return GzipBackend::auto_select;
	}
	if (backends.size() == 1) {
		return backends.front();
	}

	std::string const sample(4096, 'x');
	GzipBackend winner = backends.front();
	auto best = std::chrono::steady_clock::duration::max();
	for (auto const backend: backends) {
		auto const start = std::chrono::steady_clock::now();
		for (int i = 0; i < 32; ++i) {
			auto compressed = gzip_compress_with_backend(backend, sample);
			if (compressed.empty()) {
				continue;
			}
		}
		auto const elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed < best) {
			best = elapsed;
			winner = backend;
		}
	}
	return winner;
}
CompressionCalibration &gzip_calibration_policy() {
	static CompressionCalibration policy = CompressionCalibration::startup;
	return policy;
}
GzipBackend default_gzip_backend() {
	auto const backends = available_gzip_backends();
	if (backends.empty()) {
		return GzipBackend::auto_select;
	}
	if (backends.size() == 1) {
		return backends.front();
	}

	std::array preferred{
		GzipBackend::isa_l,
		GzipBackend::zlib_ng,
		GzipBackend::libdeflate,
		GzipBackend::zlib,
	};
	for (auto const backend: preferred) {
		if (gzip_backend_available(backend)) {
			return backend;
		}
	}
	return backends.front();
}
GzipBackend &configured_gzip_backend() {
	static GzipBackend backend = [] {
		auto const backends = available_gzip_backends();
		if (backends.size() <= 1) {
			return default_gzip_backend();
		}
		if (gzip_calibration_policy() == CompressionCalibration::startup) {
			return benchmark_fastest_gzip_backend();
		}
		return default_gzip_backend();
	}();
	return backend;
}
GzipBackend resolve_gzip_backend() {
	return configured_gzip_backend();
}
DynamicEncodingPreference benchmark_dynamic_encoding_preference() {
	if (!gzip_supported()) {
		return DynamicEncodingPreference::zstd_first;
	}
	if (!zstd_supported()) {
		return DynamicEncodingPreference::gzip_first;
	}

	std::string const sample(4096, 'x');
	auto const gzip_backend = resolve_gzip_backend();

	auto const gzip_start = std::chrono::steady_clock::now();
	for (int i = 0; i < 32; ++i) {
		auto compressed = gzip_compress_with_backend(gzip_backend, sample);
		if (compressed.empty()) {
			break;
		}
	}
	auto const gzip_elapsed = std::chrono::steady_clock::now() - gzip_start;

#if CONFLUX_HAS_ZSTD
	auto const zstd_start = std::chrono::steady_clock::now();
	for (int i = 0; i < 32; ++i) {
		auto compressed = zstd_compress(sample);
		if (compressed.empty()) {
			break;
		}
	}
	auto const zstd_elapsed = std::chrono::steady_clock::now() - zstd_start;
	return (gzip_elapsed <= zstd_elapsed) ? DynamicEncodingPreference::gzip_first :
											DynamicEncodingPreference::zstd_first;
#else
	return DynamicEncodingPreference::gzip_first;
#endif
}
DynamicEncodingPreference default_dynamic_encoding_preference() {
	if (!gzip_supported()) {
		return DynamicEncodingPreference::zstd_first;
	}
	if (!zstd_supported()) {
		return DynamicEncodingPreference::gzip_first;
	}

	switch (resolve_gzip_backend()) {
	case GzipBackend::isa_l:
	case GzipBackend::zlib_ng    : return DynamicEncodingPreference::gzip_first;
	case GzipBackend::libdeflate :
	case GzipBackend::zlib       :
	case GzipBackend::auto_select: return DynamicEncodingPreference::zstd_first;
	}
	return DynamicEncodingPreference::zstd_first;
}
DynamicEncodingPreference &configured_dynamic_encoding_preference() {
	static DynamicEncodingPreference pref = [] {
		if (!(gzip_supported() && zstd_supported())) {
			return default_dynamic_encoding_preference();
		}
		if (gzip_calibration_policy() == CompressionCalibration::startup) {
			return benchmark_dynamic_encoding_preference();
		}
		return default_dynamic_encoding_preference();
	}();
	return pref;
}
DynamicEncodingPreference resolve_dynamic_encoding_preference() {
	return configured_dynamic_encoding_preference();
}
// Parse Accept-Encoding header and return the best dynamic codec available.
// Dynamic responses only consider gzip and zstd. Brotli is intentionally left
// out of this path and should be reserved for cached/static responses.
std::string pick_encoding(
	std::string_view hdr) {
	auto const qs = header_encoding_q_values(hdr);
	float const q_gzip = gzip_supported() ? qs.gzip : -1.0F;
	float const q_zstd = zstd_supported() ? qs.zstd : -1.0F;

	if (q_gzip <= 0.0F && q_zstd <= 0.0F) {
		return {};
	}

	auto const preference = resolve_dynamic_encoding_preference();
	if (preference == DynamicEncodingPreference::gzip_first) {
		if (q_gzip >= q_zstd && q_gzip > 0.0F) {
			return "gzip";
		}
		if (q_zstd > 0.0F) {
			return "zstd";
		}
		if (q_gzip > 0.0F) {
			return "gzip";
		}
		return {};
	}

	if (q_zstd >= q_gzip && q_zstd > 0.0F) {
		return "zstd";
	}
	if (q_gzip > 0.0F) {
		return "gzip";
	}
	if (q_zstd > 0.0F) {
		return "zstd";
	}
	return {};
}
#if CONFLUX_HAS_BROTLI
std::string brotli_compress(
	std::string_view input) {
	std::size_t out_size = BrotliEncoderMaxCompressedSize(input.size());
	if (out_size == 0) {
		return {};
	}
	std::string out(out_size, '\0');
	if (BrotliEncoderCompress(
			BROTLI_DEFAULT_QUALITY,
			BROTLI_DEFAULT_WINDOW,
			BROTLI_MODE_TEXT,
			input.size(),
			reinterpret_cast<std::uint8_t const *>(input.data()),
			&out_size,
			reinterpret_cast<std::uint8_t *>(out.data()))
		== 0) {
		return {};
	}
	out.resize(out_size);
	return out;
}
#endif // CONFLUX_HAS_BROTLI

#if CONFLUX_HAS_ZSTD
std::string zstd_compress(
	std::string_view input) {
	std::size_t const bound = ZSTD_compressBound(input.size());
	std::string out(bound, '\0');
	std::size_t const result = ZSTD_compress(
		out.data(),
		bound,
		input.data(),
		input.size(),
		3); // level 3: good ratio/speed tradeoff
	if (ZSTD_isError(result) != 0U) {
		return {};
	}
	out.resize(result);
	return out;
}
#endif // CONFLUX_HAS_ZSTD

} // namespace compress_detail
export [[nodiscard]] std::vector<GzipBackend> available_gzip_backends() {
	return compress_detail::available_gzip_backends();
}
export [[nodiscard]] GzipBackend current_gzip_backend() {
	return compress_detail::resolve_gzip_backend();
}
export [[nodiscard]] DynamicEncodingPreference current_dynamic_encoding_preference() {
	return compress_detail::resolve_dynamic_encoding_preference();
}
export [[nodiscard]] CompressionCalibration compression_calibration() {
	return compress_detail::gzip_calibration_policy();
}
export void set_compression_calibration(
	CompressionCalibration policy) {
	compress_detail::gzip_calibration_policy() = policy;
	auto const backends = compress_detail::available_gzip_backends();
	if (backends.size() <= 1) {
		compress_detail::configured_gzip_backend() = compress_detail::default_gzip_backend();
		compress_detail::configured_dynamic_encoding_preference() =
			compress_detail::default_dynamic_encoding_preference();
		return;
	}
	if (policy == CompressionCalibration::startup) {
		compress_detail::configured_gzip_backend() = compress_detail::benchmark_fastest_gzip_backend();
		compress_detail::configured_dynamic_encoding_preference() =
			compress_detail::benchmark_dynamic_encoding_preference();
	} else {
		compress_detail::configured_gzip_backend() = compress_detail::default_gzip_backend();
		compress_detail::configured_dynamic_encoding_preference() =
			compress_detail::default_dynamic_encoding_preference();
	}
}
export void calibrate_gzip_backend() {
	auto const backends = compress_detail::available_gzip_backends();
	if (backends.size() <= 1) {
		compress_detail::configured_gzip_backend() = compress_detail::default_gzip_backend();
		compress_detail::configured_dynamic_encoding_preference() =
			compress_detail::default_dynamic_encoding_preference();
		return;
	}
	compress_detail::configured_gzip_backend() = compress_detail::benchmark_fastest_gzip_backend();
	compress_detail::configured_dynamic_encoding_preference() =
		compress_detail::benchmark_dynamic_encoding_preference();
}
export bool set_gzip_backend(
	GzipBackend backend) {
	if (backend != GzipBackend::auto_select && !compress_detail::gzip_backend_available(backend)) {
		return false;
	}
	if (backend == GzipBackend::auto_select) {
		compress_detail::configured_gzip_backend() = compress_detail::benchmark_fastest_gzip_backend();
	} else {
		compress_detail::configured_gzip_backend() = backend;
	}
	compress_detail::configured_dynamic_encoding_preference() = compress_detail::default_dynamic_encoding_preference();
	return true;
}
// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

// Compress responses using the best encoding the client accepts.
// Supports br (brotli), zstd, and gzip depending on what was compiled in.
// Skips small responses, SSE streams, and non-compressible MIME types.
export Router::Middleware compress_middleware(
	CompressOptions opts = {}) {
	return [opts](RequestView const &req, Router::Handler const &next) -> Response {
		auto resp = next(req);

		if (!resp.is_text()) {
			return resp;
		}
		if (resp.text_body().size() < opts.min_body_size) {
			return resp;
		}
		if (!compress_detail::is_compressible(resp.content_type)) {
			return resp;
		}

		auto enc = compress_detail::pick_encoding(req.headers["accept-encoding"]);
		if (enc.empty()) {
			return resp;
		}

		std::string compressed;
#if CONFLUX_HAS_BROTLI
		if (enc == "br")
			compressed = compress_detail::brotli_compress(resp.text_body());
#endif
#if CONFLUX_HAS_ZSTD
		if (enc == "zstd")
			compressed = compress_detail::zstd_compress(resp.text_body());
#endif
#if CONFLUX_HAS_COMPRESS
		if (enc == "gzip")
			compressed =
				compress_detail::gzip_compress_with_backend(compress_detail::resolve_gzip_backend(), resp.text_body());
#endif

		if (compressed.empty()) {
			return resp;
		} // fall back to uncompressed

		resp.set_text_body(std::move(compressed));
		resp.headers["Content-Encoding"] = enc;
		resp.append_vary("Accept-Encoding");
		return resp;
	};
}
