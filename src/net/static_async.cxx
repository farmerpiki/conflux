module;
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <unistd.h>
module conflux.net.http.static_async;

import std;
import conflux.types;
import conflux.utils;
import conflux.work;
import conflux.uring.handle;
import conflux.file_io;
import conflux.file_io_sync;
import conflux.file_map;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.static_files;
import conflux.net.http.response;
import conflux.net.http.static_core;

namespace http_detail = conflux::http::detail;
using conflux::errnum;
using conflux::http::kHttpCreated;
using conflux::http::kHttpForbidden;
using conflux::http::kHttpNoContent;
using conflux::http::kHttpNotModified;
using conflux::http::kHttpOk;
using conflux::http::kHttpPartialContent;
using conflux::http::kHttpRangeNotSatisfiable;
using conflux::uring::FileHandle;

namespace {

int contained_static_open(
	int root_fd,
	char const *relative,
	int flags,
	mode_t mode = 0) noexcept {
	std::string_view rel{relative == nullptr ? "" : relative};
	if (rel.empty() || rel == ".") {
		return ::openat(root_fd, ".", flags | O_CLOEXEC, mode);
	}
	auto fd = conflux::file_io_sync::blocking_openat_contained(root_fd, rel, flags, mode);
	if (!fd) {
		errno = fd.error().code().value();
		return -1;
	}
	return fd->release();
}

struct StaticDir {
	DIR *dir{};
	StaticDir() noexcept = default;
	explicit StaticDir(
		DIR *d) noexcept
		: dir{d} {}
	StaticDir(StaticDir const &) = delete;
	StaticDir &operator =(StaticDir const &) = delete;
	~StaticDir() noexcept { reset(); }
	void reset() noexcept {
		if (dir != nullptr) {
			::closedir(dir);
			dir = nullptr;
		}
	}
	[[nodiscard]] DIR *get() const noexcept { return dir; }
	[[nodiscard]] explicit operator bool() const noexcept { return dir != nullptr; }
};

struct StaticAcceptedEncodings {
	bool br{};
	bool gzip{};
};

[[nodiscard]] StaticAcceptedEncodings parse_static_accept_encoding(
	std::string_view ae) {
	auto const qs = conflux::http::parse_accept_encoding_qs(ae, conflux::http::AcceptEncodingMerge::first);
	return StaticAcceptedEncodings{.br = qs.br > 0.0F, .gzip = qs.gzip > 0.0F};
}

[[nodiscard]] std::string static_file_etag(
	off_t size,
	time_t mtime) {
	std::string out;
	out.reserve(2 + sizeof(off_t) * 2 + 1 + sizeof(time_t) * 2);
	out.push_back('"');
	conflux::http::detail::append_hex(out, size);
	out.push_back('-');
	conflux::http::detail::append_hex(out, mtime);
	out.push_back('"');
	return out;
}

[[nodiscard]] std::string static_last_modified(
	time_t mtime) {
	// Thread-local cache keyed on mtime: hot directories commonly serve many
	// files sharing a handful of mtimes, so strftime runs once per mtime value
	// per thread.
	thread_local time_t last_mtime_cached = 0;
	thread_local std::string last_modified_cached;
	if (mtime == last_mtime_cached && !last_modified_cached.empty()) {
		return last_modified_cached;
	}
	last_modified_cached = conflux::http::http_date(mtime);
	last_mtime_cached = mtime;
	return last_modified_cached;
}

[[nodiscard]] conflux::http::Response static_not_modified() {
	conflux::http::Response resp;
	resp.status = kHttpNotModified;
	resp.status_text = "Not Modified";
	resp.content_type.clear();
	resp.set_text_body({});
	return resp;
}

[[nodiscard]] conflux::http::Response static_write_result_response(
	bool existed) {
	conflux::http::Response resp;
	resp.status = existed ? kHttpNoContent : kHttpCreated;
	resp.status_text = existed ? "No Content" : "Created";
	return resp;
}

[[nodiscard]] std::optional<conflux::http::Response> static_conditional_not_modified(
	http_detail::StaticRequest const &request,
	std::string_view etag,
	time_t mtime) {
	if (auto const &inm = request.if_none_match; !inm.empty() && inm == etag) {
		return static_not_modified();
	}
	if (auto const ims = request.if_modified_since; !ims.empty() && ims.size() < 64) {
		std::array<char, 64> ims_buf{};
		std::ranges::copy(ims, ims_buf.data());
		tm req_tm{};
		if (::strptime(ims_buf.data(), "%a, %d %b %Y %H:%M:%S GMT", &req_tm)) {
			req_tm.tm_isdst = 0;
			if (mtime <= ::timegm(&req_tm)) {
				return static_not_modified();
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::string static_content_range(
	std::size_t first,
	std::size_t last,
	std::size_t total) {
	std::string out;
	out.reserve(32 + 3 * 20);
	out += "bytes ";
	conflux::http::detail::append_decimal(out, first);
	out.push_back('-');
	conflux::http::detail::append_decimal(out, last);
	out.push_back('/');
	conflux::http::detail::append_decimal(out, total);
	return out;
}

[[nodiscard]] std::string static_unsatisfied_content_range(
	std::size_t total) {
	std::string out;
	out.reserve(16 + 20);
	out += "bytes */";
	conflux::http::detail::append_decimal(out, total);
	return out;
}

struct StaticRangeParseResult {
	bool range_request{};
	bool unsatisfiable{};
	std::size_t start{};
	std::size_t end{};
};

struct StaticSelectedSidecar {
	std::string rel_path;
	std::string full_path;
	struct ::stat stat{};
	std::string_view content_encoding;
};

struct StaticResolvedTarget {
	std::string file_param;
	std::string full_path;
	std::string rel_path;
	struct ::stat stat{};
};

struct StaticTargetResult {
	std::optional<StaticResolvedTarget> target;
	std::optional<conflux::http::Response> response;
};

struct StaticRangeSelection {
	std::size_t start{};
	std::size_t end{};
	bool is_range_request{};
};

[[nodiscard]] bool parse_static_range_size(
	std::string_view text,
	std::size_t &out) noexcept {
	if (text.empty()) {
		return false;
	}
	auto const *first = text.data();
	auto const *last = std::ranges::next(text.data(), ssize(text));
	auto [ptr, ec] = std::from_chars(first, last, out);
	return ec == std::errc{} && ptr == last;
}

[[nodiscard]] StaticRangeParseResult parse_static_range_request(
	std::string_view range_header,
	std::size_t file_size) noexcept {
	StaticRangeParseResult result{.start = 0, .end = file_size - 1};
	if (range_header.empty() || !range_header.starts_with("bytes=")) {
		return result;
	}
	auto const spec = range_header.substr(6);
	auto const dash = spec.find('-');
	if (dash == std::string_view::npos) {
		return result;
	}

	auto const start_text = spec.substr(0, dash);
	auto const end_text = spec.substr(dash + 1);
	std::size_t start = 0;
	std::size_t end = file_size - 1;
	bool ok = true;
	bool satisfiable = false;

	if (start_text.empty()) {
		std::size_t suffix_len = 0;
		ok = parse_static_range_size(end_text, suffix_len);
		if (ok && suffix_len > 0) {
			start = suffix_len >= file_size ? 0 : file_size - suffix_len;
			end = file_size - 1;
			satisfiable = true;
		}
	} else {
		ok = parse_static_range_size(start_text, start);
		if (ok && !end_text.empty()) {
			ok = parse_static_range_size(end_text, end);
		}
		if (ok) {
			end = std::min(end, file_size - 1);
			satisfiable = start < file_size && start <= end;
		}
	}

	if (!ok) {
		return result;
	}
	if (!satisfiable) {
		result.unsatisfiable = true;
		return result;
	}
	result.range_request = true;
	result.start = start;
	result.end = end;
	return result;
}

[[nodiscard]] std::optional<StaticSelectedSidecar> probe_static_sidecar(
	int root_fd,
	std::string const &base_full_path,
	std::string const &rel_path,
	std::string_view suffix,
	std::string_view content_encoding) {
	auto sidecar_rel = rel_path + std::string{suffix};
	conflux::file_io_sync::UniqueFd sidecar_fd{contained_static_open(root_fd, sidecar_rel.c_str(), O_PATH | O_CLOEXEC)};
	if (!sidecar_fd) {
		return std::nullopt;
	}
	struct ::stat sidecar_stat{};
	if (::fstat(sidecar_fd.fd(), &sidecar_stat) != 0 || !S_ISREG(sidecar_stat.st_mode)) {
		return std::nullopt;
	}
	return StaticSelectedSidecar{
		.rel_path = std::move(sidecar_rel),
		.full_path = base_full_path + std::string{suffix},
		.stat = sidecar_stat,
		.content_encoding = content_encoding};
}

[[nodiscard]] std::optional<StaticSelectedSidecar> select_static_precompressed_sidecar(
	int root_fd,
	std::string const &base_full_path,
	std::string const &rel_path,
	std::string_view accept_encoding) {
	auto const accepted = parse_static_accept_encoding(accept_encoding);
	if (accepted.br) {
		if (auto sidecar = probe_static_sidecar(root_fd, base_full_path, rel_path, ".br", "br")) {
			return sidecar;
		}
	}
	if (accepted.gzip) {
		if (auto sidecar = probe_static_sidecar(root_fd, base_full_path, rel_path, ".gz", "gzip")) {
			return sidecar;
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::string_view static_mime_type(
	std::string_view file_param) noexcept {
	auto const ext_pos = file_param.rfind('.');
	if (ext_pos == std::string_view::npos) {
		return "application/octet-stream";
	}
	auto const ext = file_param.substr(ext_pos);
	if (ext == ".html" || ext == ".htm") {
		return "text/html; charset=utf-8";
	}
	if (ext == ".css") {
		return "text/css; charset=utf-8";
	}
	if (ext == ".js" || ext == ".mjs") {
		return "application/javascript; charset=utf-8";
	}
	if (ext == ".json") {
		return "application/json";
	}
	if (ext == ".xml") {
		return "application/xml";
	}
	if (ext == ".txt") {
		return "text/plain; charset=utf-8";
	}
	if (ext == ".svg") {
		return "image/svg+xml";
	}
	if (ext == ".png") {
		return "image/png";
	}
	if (ext == ".jpg" || ext == ".jpeg") {
		return "image/jpeg";
	}
	if (ext == ".gif") {
		return "image/gif";
	}
	if (ext == ".webp") {
		return "image/webp";
	}
	if (ext == ".ico") {
		return "image/x-icon";
	}
	if (ext == ".woff") {
		return "font/woff";
	}
	if (ext == ".woff2") {
		return "font/woff2";
	}
	if (ext == ".ttf") {
		return "font/ttf";
	}
	if (ext == ".otf") {
		return "font/otf";
	}
	if (ext == ".pdf") {
		return "application/pdf";
	}
	if (ext == ".gz") {
		return "application/gzip";
	}
	if (ext == ".zip") {
		return "application/zip";
	}
	if (ext == ".wasm") {
		return "application/wasm";
	}
	return "application/octet-stream";
}

void add_static_cached_headers(
	conflux::http::Response &resp,
	http_detail::StaticCacheEntry const &entry,
	std::string_view cache_control) {
	resp.headers["ETag"] = entry.etag;
	resp.headers["Last-Modified"] = entry.last_modified;
	resp.headers["Accept-Ranges"] = "bytes";
	if (!entry.content_encoding.empty()) {
		resp.headers["Content-Encoding"] = entry.content_encoding;
	}
	if (!cache_control.empty()) {
		resp.headers["Cache-Control"] = cache_control;
	}
}

void add_static_file_headers(
	conflux::http::Response &resp,
	std::string_view etag,
	std::string_view last_modified,
	std::string_view content_encoding,
	std::string_view cache_control) {
	resp.headers["ETag"] = etag;
	resp.headers["Last-Modified"] = last_modified;
	resp.headers["Accept-Ranges"] = "bytes";
	if (!content_encoding.empty()) {
		resp.headers["Content-Encoding"] = content_encoding;
	}
	if (!cache_control.empty()) {
		resp.headers["Cache-Control"] = cache_control;
	}
}

[[nodiscard]] conflux::http::Response make_static_file_response(
	int status,
	std::string_view status_text,
	std::string_view mime,
	std::string_view etag,
	std::string_view last_modified,
	std::string_view content_encoding,
	std::string_view cache_control) {
	auto resp = conflux::http::Response{
		.status = status,
		.status_text = std::string{status_text},
		.content_type = std::string{mime}};
	add_static_file_headers(resp, etag, last_modified, content_encoding, cache_control);
	return resp;
}

[[nodiscard]] conflux::http::Response make_static_cached_response(
	http_detail::StaticCacheEntry const &entry,
	std::string_view cache_control) {
	auto resp = conflux::http::Response{.status = kHttpOk, .status_text = "OK", .content_type = entry.mime};
	add_static_cached_headers(resp, entry, cache_control);
	resp.set_text_body(entry.body);
	return resp;
}

[[nodiscard]] conflux::http::Response make_static_cached_range_response(
	http_detail::StaticCacheEntry const &entry,
	std::string_view cache_control,
	std::size_t range_start,
	std::size_t range_end,
	std::size_t file_size) {
	auto const send_sz = range_end - range_start + 1;
	auto resp = conflux::http::Response{
		.status = kHttpPartialContent,
		.status_text = "Partial Content",
		.content_type = entry.mime};
	add_static_cached_headers(resp, entry, cache_control);
	resp.headers["Content-Range"] = static_content_range(range_start, range_end, file_size);
	resp.set_text_body(entry.body.substr(range_start, send_sz));
	return resp;
}

[[nodiscard]] conflux::http::Response make_static_range_not_satisfiable_response(
	std::size_t file_size) {
	auto resp = conflux::http::Response{
		.status = kHttpRangeNotSatisfiable,
		.status_text = "Range Not Satisfiable",
		.content_type = "text/plain; charset=utf-8"};
	resp.headers["Content-Range"] = static_unsatisfied_content_range(file_size);
	return resp;
}

[[nodiscard]] StaticRangeSelection select_static_range(
	http_detail::StaticRequest const &request,
	std::string_view content_encoding,
	std::size_t file_size,
	std::optional<conflux::http::Response> &early_response) {
	StaticRangeSelection selection{.start = 0, .end = file_size - 1};
	if (!content_encoding.empty()) {
		return selection;
	}
	auto const range = parse_static_range_request(request.range, file_size);
	if (range.unsatisfiable) {
		early_response = make_static_range_not_satisfiable_response(file_size);
		return selection;
	}
	if (range.range_request) {
		selection.start = range.start;
		selection.end = range.end;
		selection.is_range_request = true;
	}
	return selection;
}

[[nodiscard]] conflux::http::Response make_static_mapped_response(
	conflux::file_map::MappedFileLease lease,
	std::string_view mime,
	std::string_view etag,
	std::string_view last_modified,
	std::string_view content_encoding,
	std::string_view cache_control,
	std::size_t range_start,
	std::size_t range_end,
	std::size_t file_size,
	bool is_range_request) {
	auto const send_off = is_range_request ? range_start : std::size_t{0};
	auto const send_sz = is_range_request ? (range_end - range_start + 1) : file_size;
	auto resp = make_static_file_response(
		is_range_request ? kHttpPartialContent : kHttpOk,
		is_range_request ? "Partial Content" : "OK",
		mime,
		etag,
		last_modified,
		content_encoding,
		cache_control);
	if (is_range_request) {
		resp.headers["Content-Range"] = static_content_range(range_start, range_end, file_size);
	}
	resp.set_mapped_file(
		std::make_shared<conflux::file_map::MappedBody>(
			conflux::file_map::MappedBody{.lease = std::move(lease), .offset = send_off, .size = send_sz}));
	return resp;
}

[[nodiscard]] std::optional<conflux::http::Response> try_serve_static_cached_file(
	conflux::http::StaticOptions const &static_options,
	http_detail::StaticCacheStore &static_cache,
	std::string_view full_path,
	std::string_view content_encoding,
	struct ::stat const &st,
	int root_fd,
	std::string const &rel_str,
	std::string_view file_param,
	std::string_view mime,
	std::string_view etag,
	std::string_view last_modified,
	std::size_t range_start,
	std::size_t range_end,
	std::size_t file_size,
	bool is_range_request) {
	if (!static_options.file_cache.enabled || file_size > static_options.file_cache.small_file_max_bytes) {
		return std::nullopt;
	}
	auto make_cached_response = [&](http_detail::StaticCacheEntry const &entry) {
		if (is_range_request) {
			return make_static_cached_range_response(
				entry,
				static_options.cache_control,
				range_start,
				range_end,
				file_size);
		}
		return make_static_cached_response(entry, static_options.cache_control);
	};
	if (auto cached =
			static_cache.with_cached(full_path, content_encoding, st, [&](http_detail::StaticCacheEntry const &entry) {
				return make_cached_response(entry);
			})) {
		return std::move(*cached);
	}
	conflux::file_io_sync::UniqueFd fd{contained_static_open(root_fd, rel_str.c_str(), O_RDONLY | O_CLOEXEC)};
	if (!fd) {
		return conflux::http::Response::not_found(file_param);
	}
	std::string body(file_size, '\0');
	std::size_t off = 0;
	while (off < body.size()) {
		ssize_t const n = ::read(fd.fd(), body.data() + off, body.size() - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return conflux::http::Response::internal_error();
		}
		if (n == 0) {
			break;
		}
		off += static_cast<std::size_t>(n);
	}
	if (off != body.size()) {
		body.resize(off);
	}
	http_detail::StaticCacheEntry entry{
		.body = std::move(body),
		.mime = std::string{mime},
		.etag = std::string{etag},
		.last_modified = std::string{last_modified},
		.content_encoding = std::string{content_encoding},
		.size = st.st_size,
		.mtime = st.st_mtime,
		.dev = st.st_dev,
		.ino = st.st_ino};
	auto resp = make_cached_response(entry);
	static_cache.put(
		std::string{full_path},
		std::string{content_encoding},
		std::move(entry),
		static_options.file_cache.max_total_bytes);
	return resp;
}

[[nodiscard]] conflux::http::Response static_forbidden() {
	return conflux::http::Response::html(
		"<html><body><h1>403 Forbidden</h1></body></html>",
		kHttpForbidden,
		"Forbidden");
}

[[nodiscard]] std::optional<conflux::http::Response> render_static_directory_listing(
	int root_fd,
	std::string const &rel_path,
	std::string_view file_param) {
	conflux::file_io_sync::UniqueFd dfd{
		rel_path.empty() ? contained_static_open(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
						   contained_static_open(root_fd, rel_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
	if (!dfd) {
		return std::nullopt;
	}
	StaticDir dir{::fdopendir(dfd.fd())};
	if (!dir) {
		return std::nullopt;
	}
	(void)dfd.release();

	std::string html;
	html.reserve(128 + file_param.size() * 2);
	html += "<html><head><title>Index of ";
	conflux::http::detail::append_html_escaped(html, file_param);
	html += "</title></head><body><h1>Index of ";
	conflux::http::detail::append_html_escaped(html, file_param);
	html += "</h1><ul>";
	if (!file_param.empty() && file_param != "/") {
		html += "<li><a href=\"../\">..</a></li>";
	}
	struct ::dirent *ent{};
	std::vector<std::string> names;
	while ((ent = ::readdir(dir.get())) != nullptr) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay): POSIX exposes d_name as an
		// array field.
		std::string_view const name{ent->d_name};
		if (name == "." || name == "..") {
			continue;
		}
		names.emplace_back(name);
	}
	std::ranges::sort(names);
	for (auto const &name: names) {
		html += "<li><a href=\"";
		conflux::http::append_url_percent_encoded(html, name);
		html += "\">";
		conflux::http::detail::append_html_escaped(html, name);
		html += "</a></li>";
	}
	html += "</ul></body></html>";
	return conflux::http::Response::html(std::move(html));
}

[[nodiscard]] StaticTargetResult resolve_static_target(
	std::string const &rd,
	int root_fd,
	conflux::http::StaticOptions const &static_options,
	std::string_view file_param_view) {
	StaticResolvedTarget target{
		.file_param = std::string{file_param_view},
	};
	target.full_path = rd + target.file_param;
	std::string_view rel_path = std::string_view{target.file_param};
	if (rel_path.starts_with('/')) {
		rel_path.remove_prefix(1);
	}
	target.rel_path = std::string{rel_path};

	conflux::file_io_sync::UniqueFd probe_fd{
		target.rel_path.empty() ? contained_static_open(root_fd, ".", O_PATH | O_CLOEXEC | O_DIRECTORY) :
								  contained_static_open(root_fd, target.rel_path.c_str(), O_PATH | O_CLOEXEC)};
	if (!probe_fd) {
		return StaticTargetResult{.response = conflux::http::Response::not_found(target.file_param)};
	}
	if (::fstat(probe_fd.fd(), &target.stat) != 0) {
		return StaticTargetResult{.response = conflux::http::Response::not_found(target.file_param)};
	}
	probe_fd.reset();

	if (S_ISDIR(target.stat.st_mode)) {
		auto index_rel = target.rel_path.empty() ? std::string{"index.html"} : target.rel_path + "/index.html";
		conflux::file_io_sync::UniqueFd idx_fd{contained_static_open(root_fd, index_rel.c_str(), O_PATH | O_CLOEXEC)};
		if (idx_fd) {
			if (::fstat(idx_fd.fd(), &target.stat) != 0 || !S_ISREG(target.stat.st_mode)) {
				return StaticTargetResult{.response = conflux::http::Response::not_found(target.file_param)};
			}
			idx_fd.reset();
			target.full_path = rd + "/" + index_rel;
			target.file_param += "/index.html";
			target.rel_path = std::move(index_rel);
		} else if (static_options.directory_listing) {
			auto listing = render_static_directory_listing(root_fd, target.rel_path, target.file_param);
			if (!listing) {
				return StaticTargetResult{.response = static_forbidden()};
			}
			return StaticTargetResult{.response = std::move(*listing)};
		} else {
			return StaticTargetResult{.response = static_forbidden()};
		}
	}

	return StaticTargetResult{.target = std::move(target)};
}

void select_static_variant(
	StaticResolvedTarget &target,
	std::string &content_encoding,
	int root_fd,
	bool precompressed,
	std::string_view accept_encoding) {
	if (!precompressed) {
		return;
	}
	if (auto sidecar =
			select_static_precompressed_sidecar(root_fd, target.full_path, target.rel_path, accept_encoding)) {
		target.full_path = std::move(sidecar->full_path);
		target.stat = sidecar->stat;
		content_encoding = sidecar->content_encoding;
		target.rel_path = std::move(sidecar->rel_path);
	}
}

} // namespace

namespace conflux::http {

conflux::work::root::Task<void> do_save_static_file(
	conflux::file_io::FileReader *fr,
	std::shared_ptr<std::string> body_owned,
	std::shared_ptr<std::string> fp,
	bool existed,
	http_detail::StaticCacheStore &static_cache,
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	int dir_fd,
	std::string rel_path);
conflux::work::root::Task<void> do_delete_static_file(
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	std::shared_ptr<std::string> fp,
	http_detail::StaticCacheStore &static_cache,
	conflux::work::root::Task<void> unlink_task);

[[nodiscard]] std::optional<conflux::http::Response> try_static_async_file_response(
	int root_fd,
	std::string const &rel_path,
	std::string_view mime,
	std::string_view etag,
	std::string_view last_modified,
	std::string_view content_encoding,
	std::string_view cache_control,
	StaticRangeSelection range,
	std::size_t file_size) {
	if (auto *fr = conflux::file_io::current_file_reader(); fr != nullptr && content_encoding.empty()) {
		auto dr = std::make_shared<conflux::http::DeferredResponse>();
		auto base = make_static_file_response(
			range.is_range_request ? kHttpPartialContent : kHttpOk,
			range.is_range_request ? "Partial Content" : "OK",
			mime,
			etag,
			last_modified,
			content_encoding,
			cache_control);
		if (range.is_range_request) {
			base.headers["Content-Range"] = static_content_range(range.start, range.end, file_size);
		}
		auto const send_off = range.is_range_request ? range.start : std::size_t{0};
		auto const send_sz = range.is_range_request ? (range.end - range.start + 1) : file_size;
		do_serve_static_file(
			dr,
			std::move(base),
			send_off,
			send_sz,
			file_size,
			fr->async_openat2(
				root_fd,
				std::string{rel_path},
				open_how{
					.flags = static_cast<__u64>(O_RDONLY | O_CLOEXEC),
					.mode = 0,
					.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS}))
			.detach();
		return conflux::http::Response::deferred(std::move(dr));
	}
	return std::nullopt;
}

conflux::http::Response handle_static_get_request(
	std::string const &rd,
	int root_fd,
	conflux::http::StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	http_detail::StaticCacheStore &static_cache) {
	try {
		auto norm = http_detail::normalize_static_path(req.params["file"]);
		if (!norm) {
			return static_forbidden();
		}

		http_detail::StaticRequest const sreq{
			.file_param = *norm,
			.method = req.method,
			.accept_encoding = req.headers["accept-encoding"],
			.if_none_match = std::as_const(req.headers)["if-none-match"],
			.if_modified_since = std::as_const(req.headers)["if-modified-since"],
			.range = req.headers["range"],
			.tls = req.is_tls,
		};

		if (sopts.offload_pool) {
			auto owned_sreq = http_detail::StaticRequestStorage::from(sreq);
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto ok = sopts.offload_pool->enqueue(
				[rd, root_fd, sopts, sreq = std::move(owned_sreq), &static_cache, dr]() mutable {
					try {
						dr->complete(handle_static_get(rd, root_fd, sopts, sreq.view(), static_cache));
					} catch (...) { dr->complete(conflux::http::Response::internal_error()); }
				});
			if (!ok) {
				return conflux::http::Response::internal_error("offload queue full");
			}
			return conflux::http::Response::deferred(std::move(dr));
		}

		return handle_static_get(rd, root_fd, sopts, sreq, static_cache);
	} catch (...) { return conflux::http::Response::internal_error(); }
}

conflux::http::Response handle_static_put(
	std::string const &rd,
	int root_fd,
	conflux::http::StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	http_detail::StaticCacheStore &static_cache) {
	try {
		auto norm = http_detail::normalize_static_path(req.params["file"]);
		if (!norm) {
			return static_forbidden();
		}
		auto full_path = rd + *norm;
		std::string_view rel_sv = std::string_view{*norm};
		if (rel_sv.starts_with('/')) {
			rel_sv.remove_prefix(1);
		}
		std::string rel{rel_sv};

		conflux::file_io_sync::UniqueFd probe{contained_static_open(root_fd, rel.c_str(), O_PATH | O_CLOEXEC)};
		bool const existed = probe.valid();
		probe.reset();

		if (auto *fr = conflux::file_io::current_file_reader(); fr != nullptr) {
			auto body_owned = std::make_shared<std::string>(req.body);
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto fp = std::make_shared<std::string>(full_path);
			do_save_static_file(fr, body_owned, fp, existed, static_cache, dr, root_fd, std::string{rel}).detach();
			return conflux::http::Response::deferred(std::move(dr));
		}

		if (sopts.offload_pool) {
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto body_owned = std::make_shared<std::string>(req.body);
			auto rfd = root_fd;
			auto ok = sopts.offload_pool->enqueue([full_path = std::move(full_path),
												   rel = std::move(rel),
												   rfd,
												   body_owned,
												   existed,
												   &static_cache,
												   dr]() mutable {
				auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
					rfd,
					std::string_view{rel},
					std::string_view{*body_owned});
				if (!r) {
					dr->complete(conflux::http::Response::internal_error());
					return;
				}
				static_cache.evict_path_and_sidecars(full_path);
				dr->complete(static_write_result_response(existed));
			});
			if (!ok) {
				return conflux::http::Response::internal_error("offload queue full");
			}
			return conflux::http::Response::deferred(std::move(dr));
		}

		if (!conflux::file_io_sync::blocking_write_text_file_atomic_at(
				root_fd,
				std::string_view{rel},
				std::string_view{req.body})) {
			return conflux::http::Response::internal_error();
		}
		static_cache.evict_path_and_sidecars(full_path);
		return static_write_result_response(existed);
	} catch (...) { return conflux::http::Response::internal_error(); }
}

conflux::http::Response handle_static_delete(
	std::string const &rd,
	int root_fd,
	conflux::http::StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	http_detail::StaticCacheStore &static_cache) {
	try {
		auto norm = http_detail::normalize_static_path(req.params["file"]);
		if (!norm) {
			return static_forbidden();
		}
		auto full_path = rd + *norm;
		std::string_view rel_sv = std::string_view{*norm};
		if (rel_sv.starts_with('/')) {
			rel_sv.remove_prefix(1);
		}
		std::string rel{rel_sv};

		conflux::file_io_sync::UniqueFd probe{contained_static_open(root_fd, rel.c_str(), O_PATH | O_CLOEXEC)};
		if (!probe) {
			return errno == ENOENT ? conflux::http::Response::not_found(*norm) : conflux::http::Response::forbidden();
		}
		probe.reset();

		if (auto *fr = conflux::file_io::current_file_reader(); fr != nullptr) {
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto fp = std::make_shared<std::string>(full_path);
			do_delete_static_file(dr, fp, static_cache, fr->async_unlink(root_fd, rel)).detach();
			return conflux::http::Response::deferred(std::move(dr));
		}

		if (sopts.offload_pool) {
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto rfd = root_fd;
			auto ok = sopts.offload_pool->enqueue(
				[full_path = std::move(full_path), rel = std::move(rel), rfd, &static_cache, dr]() mutable {
					try {
						auto unlinked = conflux::file_io_sync::blocking_unlink_file_at(rfd, rel);
						if (!unlinked) {
							auto const err = errnum(unlinked);
							dr->complete(
								err == ENOENT ? conflux::http::Response::not_found(full_path) :
												conflux::http::Response::internal_error());
							return;
						}
						static_cache.evict_path_and_sidecars(full_path);
						dr->complete(conflux::http::Response::no_content());
					} catch (...) { dr->complete(conflux::http::Response::internal_error()); }
				});
			if (!ok) {
				return conflux::http::Response::internal_error("offload queue full");
			}
			return conflux::http::Response::deferred(std::move(dr));
		}

		auto unlinked = conflux::file_io_sync::blocking_unlink_file_at(root_fd, rel);
		if (!unlinked) {
			auto const err = errnum(unlinked);
			return err == ENOENT ? conflux::http::Response::not_found(*norm) :
								   conflux::http::Response::internal_error();
		}
		static_cache.evict_path_and_sidecars(full_path);
		return conflux::http::Response::no_content();
	} catch (...) { return conflux::http::Response::internal_error(); }
}

conflux::http::Response handle_static_get(
	std::string const &rd,
	int root_fd,
	conflux::http::StaticOptions const &static_options,
	http_detail::StaticRequest const &r,
	http_detail::StaticCacheStore &static_cache) {
	try {
		auto resolved = resolve_static_target(rd, root_fd, static_options, r.file_param);
		if (resolved.response) {
			return std::move(*resolved.response);
		}
		auto target = std::move(*resolved.target);

		std::string content_encoding;
		select_static_variant(target, content_encoding, root_fd, static_options.precompressed, r.accept_encoding);
		if (!S_ISREG(target.stat.st_mode)) {
			return static_forbidden();
		}

		auto etag = static_file_etag(target.stat.st_size, target.stat.st_mtime);

		auto last_modified = static_last_modified(target.stat.st_mtime);

		if (auto not_modified = static_conditional_not_modified(r, etag, target.stat.st_mtime)) {
			return std::move(*not_modified);
		}

		auto const mime = static_mime_type(target.file_param);

		auto file_size = static_cast<std::size_t>(target.stat.st_size);

		if (r.method == "HEAD") {
			auto resp = make_static_file_response(
				kHttpOk,
				"OK",
				mime,
				etag,
				last_modified,
				content_encoding,
				static_options.cache_control);
			resp.head_only = true;
			resp.content_length_hint = file_size;
			return resp;
		}

		if (file_size == 0) {
			return make_static_file_response(
				kHttpOk,
				"OK",
				mime,
				etag,
				last_modified,
				content_encoding,
				static_options.cache_control);
		}

		std::optional<conflux::http::Response> range_response;
		auto const range = select_static_range(r, content_encoding, file_size, range_response);
		if (range_response) {
			return std::move(*range_response);
		}

		if (auto cached = try_serve_static_cached_file(
				static_options,
				static_cache,
				target.full_path,
				content_encoding,
				target.stat,
				root_fd,
				target.rel_path,
				target.file_param,
				mime,
				etag,
				last_modified,
				range.start,
				range.end,
				file_size,
				range.is_range_request)) {
			return std::move(*cached);
		}

		if (auto async_response = try_static_async_file_response(
				root_fd,
				target.rel_path,
				mime,
				etag,
				last_modified,
				content_encoding,
				static_options.cache_control,
				range,
				file_size)) {
			return std::move(*async_response);
		}

		auto lease = conflux::file_map::blocking_map_file_readonly(root_fd, std::string_view{target.rel_path});
		if (!lease) {
			return conflux::http::Response::internal_error();
		}
		if (lease->size() != file_size) {
			return conflux::http::Response::internal_error();
		}

		if (range.is_range_request) {
			return make_static_mapped_response(
				std::move(*lease),
				mime,
				etag,
				last_modified,
				content_encoding,
				static_options.cache_control,
				range.start,
				range.end,
				file_size,
				true);
		}

		return make_static_mapped_response(
			std::move(*lease),
			mime,
			etag,
			last_modified,
			content_encoding,
			static_options.cache_control,
			range.start,
			range.end,
			file_size,
			false);
	} catch (...) { return conflux::http::Response::internal_error(); }
}

conflux::work::root::Task<void> do_serve_static_file(
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	conflux::http::Response base,
	std::size_t send_off,
	std::size_t send_sz,
	std::size_t total_size,
	conflux::work::root::Task<FileHandle> open_task) {
	try {
		auto fh = co_await std::move(open_task);
		auto streamed = std::make_shared<conflux::http::StreamedFile>();
		streamed->handle = conflux::http::StreamedFileHandle::from(std::make_shared<FileHandle>(std::move(fh)));
		streamed->send_offset = send_off;
		streamed->send_size = send_sz;
		streamed->total_size = total_size;
		base.set_streamed_file(std::move(streamed));
		dr->complete(std::move(base));
	} catch (...) { dr->complete(conflux::http::Response::not_found("async open failed")); }
}
conflux::work::root::Task<void> do_save_static_file(
	conflux::file_io::FileReader *fr,
	std::shared_ptr<std::string> body_owned,
	std::shared_ptr<std::string> fp,
	bool existed,
	http_detail::StaticCacheStore &static_cache,
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	int dir_fd,
	std::string rel_path) {
	try {
		co_await fr->async_atomic_write(dir_fd, std::move(rel_path), std::as_bytes(std::span{*body_owned}));
		static_cache.evict_path_and_sidecars(*fp);
		dr->complete(static_write_result_response(existed));
	} catch (...) { dr->complete(conflux::http::Response::internal_error()); }
}

conflux::work::root::Task<void> do_delete_static_file(
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	std::shared_ptr<std::string> fp,
	http_detail::StaticCacheStore &static_cache,
	conflux::work::root::Task<void> unlink_task) {
	try {
		co_await std::move(unlink_task);
		static_cache.evict_path_and_sidecars(*fp);
		dr->complete(conflux::http::Response::no_content());
	} catch (conflux::IoError const &e) {
		dr->complete(
			e.code().value() == ENOENT ? conflux::http::Response::not_found(*fp) :
										 conflux::http::Response::internal_error());
	} catch (...) { dr->complete(conflux::http::Response::internal_error()); }
}

} // namespace conflux::http
