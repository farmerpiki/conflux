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
	auto fd = blocking_openat_contained(root_fd, rel, flags, mode);
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

void append_static_html_escape(
	std::string &out,
	std::string_view s) {
	for (char const c: s) {
		switch (c) {
		case '&' : out += "&amp;"; break;
		case '<' : out += "&lt;"; break;
		case '>' : out += "&gt;"; break;
		case '"' : out += "&quot;"; break;
		case '\'': out += "&#39;"; break;
		default  : out += c; break;
		}
	}
}

template<typename T>
void append_static_hex(
	std::string &out,
	T value) {
	using U = std::make_unsigned_t<T>;
	std::array<char, sizeof(U) * 2> buf{};
	auto const v = static_cast<U>(value);
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v, 16);
	if (ec == std::errc{}) {
		out.append(buf.data(), ptr);
	}
}

void append_static_decimal(
	std::string &out,
	std::size_t value) {
	std::array<char, 32> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == std::errc{}) {
		out.append(buf.data(), ptr);
	}
}

struct StaticAcceptedEncodings {
	bool br{};
	bool gzip{};
};

[[nodiscard]] StaticAcceptedEncodings parse_static_accept_encoding(
	std::string_view ae) {
	StaticAcceptedEncodings out{};
	bool seen_br = false;
	bool seen_gzip = false;
	for (auto const item: conflux::http::header_items(ae)) {
		float const q = conflux::http::parse_http_q(item.params).value_or(1.0F);
		if (!seen_br && conflux::http::ascii_iequals(item.name, "br")) {
			out.br = q != 0.0F;
			seen_br = true;
		} else if (!seen_gzip && conflux::http::ascii_iequals(item.name, "gzip")) {
			out.gzip = q != 0.0F;
			seen_gzip = true;
		}
		if (seen_br && seen_gzip) {
			break;
		}
	}
	return out;
}

[[nodiscard]] std::string static_file_etag(
	off_t size,
	time_t mtime) {
	std::string out;
	out.reserve(2 + sizeof(off_t) * 2 + 1 + sizeof(time_t) * 2);
	out.push_back('"');
	append_static_hex(out, size);
	out.push_back('-');
	append_static_hex(out, mtime);
	out.push_back('"');
	return out;
}

[[nodiscard]] std::string static_content_range(
	std::size_t first,
	std::size_t last,
	std::size_t total) {
	std::string out;
	out.reserve(32 + 3 * 20);
	out += "bytes ";
	append_static_decimal(out, first);
	out.push_back('-');
	append_static_decimal(out, last);
	out.push_back('/');
	append_static_decimal(out, total);
	return out;
}

[[nodiscard]] std::string static_unsatisfied_content_range(
	std::size_t total) {
	std::string out;
	out.reserve(16 + 20);
	out += "bytes */";
	append_static_decimal(out, total);
	return out;
}

[[nodiscard]] conflux::http::Response static_forbidden() {
	return conflux::http::Response::html(
		"<html><body><h1>403 Forbidden</h1></body></html>",
		kHttpForbidden,
		"Forbidden");
}

} // namespace

conflux::work::root::Task<void> do_save_static_file(
	FileReader *fr,
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

		UniqueFd probe{contained_static_open(root_fd, rel.c_str(), O_PATH | O_CLOEXEC)};
		bool const existed = probe.valid();
		probe.reset();

		if (auto *fr = current_file_reader(); fr != nullptr) {
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
				auto r = blocking_write_text_file_atomic_at(rfd, std::string_view{rel}, std::string_view{*body_owned});
				if (!r) {
					dr->complete(conflux::http::Response::internal_error());
					return;
				}
				static_cache.evict_path_and_sidecars(full_path);
				conflux::http::Response resp;
				resp.status = existed ? kHttpNoContent : kHttpCreated;
				resp.status_text = existed ? "No Content" : "Created";
				dr->complete(std::move(resp));
			});
			if (!ok) {
				return conflux::http::Response::internal_error("offload queue full");
			}
			return conflux::http::Response::deferred(std::move(dr));
		}

		if (!blocking_write_text_file_atomic_at(root_fd, std::string_view{rel}, std::string_view{req.body})) {
			return conflux::http::Response::internal_error();
		}
		static_cache.evict_path_and_sidecars(full_path);
		conflux::http::Response resp;
		resp.status = existed ? kHttpNoContent : kHttpCreated;
		resp.status_text = existed ? "No Content" : "Created";
		return resp;
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

		UniqueFd probe{contained_static_open(root_fd, rel.c_str(), O_PATH | O_CLOEXEC)};
		if (!probe) {
			return errno == ENOENT ? conflux::http::Response::not_found(*norm) : conflux::http::Response::forbidden();
		}
		probe.reset();

		if (auto *fr = current_file_reader(); fr != nullptr) {
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
						auto unlinked = blocking_unlink_file_at(rfd, rel);
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

		auto unlinked = blocking_unlink_file_at(root_fd, rel);
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
		std::string file_param{r.file_param};
		auto full_path = rd + file_param;
		std::string_view rel_path = std::string_view{file_param};
		if (rel_path.starts_with('/')) {
			rel_path.remove_prefix(1);
		}
		std::string rel_str{rel_path};

		struct ::stat st{};
		UniqueFd probe_fd{
			rel_str.empty() ? contained_static_open(root_fd, ".", O_PATH | O_CLOEXEC | O_DIRECTORY) :
							  contained_static_open(root_fd, rel_str.c_str(), O_PATH | O_CLOEXEC)};
		if (!probe_fd) {
			return conflux::http::Response::not_found(file_param);
		}
		if (::fstat(probe_fd.fd(), &st) != 0) {
			return conflux::http::Response::not_found(file_param);
		}
		probe_fd.reset();

		if (S_ISDIR(st.st_mode)) {
			auto index_rel = rel_str.empty() ? std::string{"index.html"} : rel_str + "/index.html";
			UniqueFd idx_fd{contained_static_open(root_fd, index_rel.c_str(), O_PATH | O_CLOEXEC)};
			if (idx_fd) {
				if (::fstat(idx_fd.fd(), &st) != 0 || !S_ISREG(st.st_mode)) {
					return conflux::http::Response::not_found(file_param);
				}
				idx_fd.reset();
				full_path = rd + "/" + index_rel;
				file_param += "/index.html";
				rel_str = index_rel;
			} else if (static_options.directory_listing) {
				UniqueFd dfd{
					rel_str.empty() ?
						contained_static_open(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
						contained_static_open(root_fd, rel_str.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
				if (!dfd) {
					return static_forbidden();
				}
				StaticDir dir{::fdopendir(dfd.fd())};
				if (!dir) {
					return static_forbidden();
				}
				(void)dfd.release();
				std::string html;
				html.reserve(128 + file_param.size() * 2);
				html += "<html><head><title>Index of ";
				append_static_html_escape(html, file_param);
				html += "</title></head><body><h1>Index of ";
				append_static_html_escape(html, file_param);
				html += "</h1><ul>";
				if (!file_param.empty() && file_param != "/") {
					html += "<li><a href=\"../\">..</a></li>";
				}
				struct ::dirent *ent{};
				std::vector<std::string> names;
				while ((ent = ::readdir(dir.get())) != nullptr) {
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
					std::string_view const n{ent->d_name};
					if (n == "." || n == "..") {
						continue;
					}
					names.emplace_back(n);
				}
				std::ranges::sort(names);
				for (auto const &name: names) {
					html += "<li><a href=\"";
					append_url_percent_encoded(html, name);
					html += "\">";
					append_static_html_escape(html, name);
					html += "</a></li>";
				}
				html += "</ul></body></html>";
				return conflux::http::Response::html(std::move(html));
			} else {
				return static_forbidden();
			}
		}

		// Pre-compressed sidecar: try .br then .gz.
		std::string content_encoding;
		if (static_options.precompressed) {
			auto const accepted = parse_static_accept_encoding(r.accept_encoding);
			if (accepted.br) {
				auto br_rel = rel_str + ".br";
				UniqueFd br_fd{contained_static_open(root_fd, br_rel.c_str(), O_PATH | O_CLOEXEC)};
				if (br_fd) {
					struct ::stat br_st{};
					if (::fstat(br_fd.fd(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
						full_path = rd + "/" + br_rel;
						st = br_st;
						content_encoding = "br";
						rel_str = br_rel;
					}
				}
			}
			if (content_encoding.empty() && accepted.gzip) {
				auto gz_rel = rel_str + ".gz";
				UniqueFd gz_fd{contained_static_open(root_fd, gz_rel.c_str(), O_PATH | O_CLOEXEC)};
				if (gz_fd) {
					struct ::stat gz_st{};
					if (::fstat(gz_fd.fd(), &gz_st) == 0 && S_ISREG(gz_st.st_mode)) {
						full_path = rd + "/" + gz_rel;
						st = gz_st;
						content_encoding = "gzip";
						rel_str = gz_rel;
					}
				}
			}
		}
		if (!S_ISREG(st.st_mode)) {
			return static_forbidden();
		}

		// Build ETag from size + mtime.
		auto etag = static_file_etag(st.st_size, st.st_mtime);

		// Format Last-Modified. Thread-local cache keyed on mtime — a hot
		// directory typically serves many files sharing a handful of mtimes,
		// so strftime runs once per mtime value per std::thread.
		thread_local time_t last_mtime_cached = 0;
		thread_local std::string last_modified_cached;
		std::string last_modified;
		if (st.st_mtime == last_mtime_cached && !last_modified_cached.empty()) {
			last_modified = last_modified_cached;
		} else {
			last_modified = conflux::http::http_date(st.st_mtime);
			last_modified_cached = last_modified;
			last_mtime_cached = st.st_mtime;
		}

		// 304 Not Modified checks.
		if (auto const &inm = r.if_none_match; !inm.empty() && inm == etag) {
			conflux::http::Response resp;
			resp.status = kHttpNotModified;
			resp.status_text = "Not Modified";
			resp.content_type.clear();
			resp.set_text_body({});
			return resp;
		}
		if (auto const ims = r.if_modified_since; !ims.empty() && ims.size() < 64) {
			std::array<char, 64> ims_buf{};
			std::ranges::copy(ims, ims_buf.data());
			tm req_tm{};
			if (::strptime(ims_buf.data(), "%a, %d %b %Y %H:%M:%S GMT", &req_tm)) {
				req_tm.tm_isdst = 0;
				if (st.st_mtime <= ::timegm(&req_tm)) {
					conflux::http::Response resp;
					resp.status = kHttpNotModified;
					resp.status_text = "Not Modified";
					resp.content_type.clear();
					resp.set_text_body({});
					return resp;
				}
			}
		}

		// MIME type from extension (use original file_param, not .gz/.br path).
		auto ext_pos = file_param.rfind('.');
		std::string_view mime = "application/octet-stream";
		if (ext_pos != std::string::npos) {
			auto ext = std::string_view{file_param}.substr(ext_pos);
			if (ext == ".html" || ext == ".htm") {
				mime = "text/html; charset=utf-8";
			} else if (ext == ".css") {
				mime = "text/css; charset=utf-8";
			} else if (ext == ".js" || ext == ".mjs") {
				mime = "application/javascript; charset=utf-8";
			} else if (ext == ".json") {
				mime = "application/json";
			} else if (ext == ".xml") {
				mime = "application/xml";
			} else if (ext == ".txt") {
				mime = "text/plain; charset=utf-8";
			} else if (ext == ".svg") {
				mime = "image/svg+xml";
			} else if (ext == ".png") {
				mime = "image/png";
			} else if (ext == ".jpg" || ext == ".jpeg") {
				mime = "image/jpeg";
			} else if (ext == ".gif") {
				mime = "image/gif";
			} else if (ext == ".webp") {
				mime = "image/webp";
			} else if (ext == ".ico") {
				mime = "image/x-icon";
			} else if (ext == ".woff") {
				mime = "font/woff";
			} else if (ext == ".woff2") {
				mime = "font/woff2";
			} else if (ext == ".ttf") {
				mime = "font/ttf";
			} else if (ext == ".otf") {
				mime = "font/otf";
			} else if (ext == ".pdf") {
				mime = "application/pdf";
			} else if (ext == ".gz") {
				mime = "application/gzip";
			} else if (ext == ".zip") {
				mime = "application/zip";
			} else if (ext == ".wasm") {
				mime = "application/wasm";
			}
		}

		auto file_size = static_cast<std::size_t>(st.st_size);

		auto base_response = [&](int status, std::string_view status_text) {
			conflux::http::Response resp{
				.status = status,
				.status_text = std::string{status_text},
				.content_type = std::string{mime}};
			resp.headers["ETag"] = etag;
			resp.headers["Last-Modified"] = last_modified;
			resp.headers["Accept-Ranges"] = "bytes";
			if (!content_encoding.empty()) {
				resp.headers["Content-Encoding"] = content_encoding;
			}
			if (!static_options.cache_control.empty()) {
				resp.headers["Cache-Control"] = static_options.cache_control;
			}
			return resp;
		};

		// HEAD: return headers only (no body, but correct Content-Length).
		if (r.method == "HEAD") {
			auto resp = base_response(kHttpOk, "OK");
			resp.head_only = true;
			resp.content_length_hint = file_size;
			return resp;
		}

		// Zero-size file: skip mmap, return empty body directly.
		if (file_size == 0) {
			return base_response(kHttpOk, "OK");
		}

		// Parse Range header for partial content (only supported when no precompression applied).
		std::size_t range_start = 0;
		std::size_t range_end = file_size - 1;
		bool is_range_request = false;
		if (content_encoding.empty()) {
			auto const &range_hdr = r.range;
			if (!range_hdr.empty() && range_hdr.starts_with("bytes=")) {
				auto spec = std::string_view{range_hdr}.substr(6);
				auto dash = spec.find('-');
				if (dash != std::string_view::npos) {
					auto start_sv = spec.substr(0, dash);
					auto end_sv = spec.substr(dash + 1);
					std::size_t rs = 0;
					std::size_t re = file_size - 1;
					bool ok = true;
					bool satisfiable = false;
					auto parse_size = [](std::string_view s, std::size_t &out) {
						if (s.empty()) {
							return false;
						}
						auto const *first = s.data();
						auto const *last = std::ranges::next(s.data(), ssize(s));
						auto [p, ec] = std::from_chars(first, last, out);
						return ec == std::errc{} && p == last;
					};

					if (start_sv.empty()) {
						std::size_t suffix_len = 0;
						ok = parse_size(end_sv, suffix_len);
						if (ok && suffix_len > 0) {
							rs = suffix_len >= file_size ? 0 : file_size - suffix_len;
							re = file_size - 1;
							satisfiable = true;
						}
					} else {
						ok = parse_size(start_sv, rs);
						if (ok && !end_sv.empty()) {
							ok = parse_size(end_sv, re);
						}
						if (ok) {
							re = std::min(re, file_size - 1);
							satisfiable = rs < file_size && rs <= re;
						}
					}

					if (ok && satisfiable) {
						range_start = rs;
						range_end = re;
						is_range_request = true;
					} else if (ok) {
						// Range not satisfiable
						auto resp = conflux::http::Response{};
						resp.status = kHttpRangeNotSatisfiable;
						resp.status_text = "Range Not Satisfiable";
						resp.content_type = "text/plain; charset=utf-8";
						resp.headers["Content-Range"] = static_unsatisfied_content_range(file_size);
						return resp;
					}
				}
			}
		}

		if (static_options.file_cache.enabled && file_size <= static_options.file_cache.small_file_max_bytes) {
			auto make_cached_response = [&](http_detail::StaticCacheEntry const &entry) {
				if (is_range_request) {
					auto send_sz = range_end - range_start + 1;
					auto resp = conflux::http::Response{
						.status = kHttpPartialContent,
						.status_text = "Partial Content",
						.content_type = entry.mime};
					resp.headers["ETag"] = entry.etag;
					resp.headers["Last-Modified"] = entry.last_modified;
					resp.headers["Accept-Ranges"] = "bytes";
					resp.headers["Content-Range"] = static_content_range(range_start, range_end, file_size);
					if (!entry.content_encoding.empty()) {
						resp.headers["Content-Encoding"] = entry.content_encoding;
					}
					if (!static_options.cache_control.empty()) {
						resp.headers["Cache-Control"] = static_options.cache_control;
					}
					resp.set_text_body(entry.body.substr(range_start, send_sz));
					return resp;
				}
				auto resp = conflux::http::Response{.status = kHttpOk, .status_text = "OK", .content_type = entry.mime};
				resp.headers["ETag"] = entry.etag;
				resp.headers["Last-Modified"] = entry.last_modified;
				resp.headers["Accept-Ranges"] = "bytes";
				if (!entry.content_encoding.empty()) {
					resp.headers["Content-Encoding"] = entry.content_encoding;
				}
				if (!static_options.cache_control.empty()) {
					resp.headers["Cache-Control"] = static_options.cache_control;
				}
				resp.set_text_body(entry.body);
				return resp;
			};
			if (auto cached =
					static_cache.with_cached(full_path, content_encoding, st, [&](http_detail::StaticCacheEntry const &entry) {
						if (!is_range_request) {
							auto resp = conflux::http::Response{
								.status = kHttpOk,
								.status_text = "OK",
								.content_type = entry.mime};
							resp.headers["ETag"] = entry.etag;
							resp.headers["Last-Modified"] = entry.last_modified;
							resp.headers["Accept-Ranges"] = "bytes";
							if (!entry.content_encoding.empty()) {
								resp.headers["Content-Encoding"] = entry.content_encoding;
							}
							if (!static_options.cache_control.empty()) {
								resp.headers["Cache-Control"] = static_options.cache_control;
							}
							resp.set_text_body(entry.body);
							return resp;
						}
						return make_cached_response(entry);
					})) {
				return std::move(*cached);
			}
			UniqueFd fd{contained_static_open(root_fd, rel_str.c_str(), O_RDONLY | O_CLOEXEC)};
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
				.etag = etag,
				.last_modified = last_modified,
				.content_encoding = content_encoding,
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

		// Async uring path: when a FileReader is installed for this
		// std::thread (i.e. we are running on a ring std::thread, not offloaded to
		// a WorkPool) and no compressed std::variant was picked, open the
		// file via IORING_OP_OPENAT and return a deferred response that
		// carries a StreamedFile once the open CQE fires. Otherwise
		// fall back to the synchronous mmap path below.
		if (auto *fr = current_file_reader(); fr != nullptr && content_encoding.empty()) {
			auto dr = std::make_shared<conflux::http::DeferredResponse>();
			auto base =
				is_range_request ? base_response(kHttpPartialContent, "Partial Content") : base_response(kHttpOk, "OK");
			if (is_range_request) {
				base.headers["Content-Range"] = static_content_range(range_start, range_end, file_size);
			}
			auto const send_off = is_range_request ? range_start : std::size_t{0};
			auto const send_sz = is_range_request ? (range_end - range_start + 1) : file_size;
			do_serve_static_file(
				dr,
				std::move(base),
				send_off,
				send_sz,
				file_size,
				fr->async_openat2(
					root_fd,
					std::string{rel_str},
					open_how{
						.flags = static_cast<__u64>(O_RDONLY | O_CLOEXEC),
						.mode = 0,
						.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS}))
				.detach();
			return conflux::http::Response::deferred(std::move(dr));
		}

		auto lease = blocking_map_file_readonly(root_fd, std::string_view{rel_str});
		if (!lease) {
			return conflux::http::Response::internal_error();
		}

		if (is_range_request) {
			auto send_sz = range_end - range_start + 1;
			auto resp = base_response(kHttpPartialContent, "Partial Content");
			resp.headers["Content-Range"] = static_content_range(range_start, range_end, file_size);
			resp.set_mapped_file(
				std::make_shared<MappedBody>(
					MappedBody{.lease = std::move(*lease), .offset = range_start, .size = send_sz}));
			return resp;
		}

		auto resp = base_response(kHttpOk, "OK");
		resp.set_mapped_file(
			std::make_shared<MappedBody>(MappedBody{.lease = std::move(*lease), .offset = 0, .size = file_size}));
		return resp;
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
	FileReader *fr,
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
		conflux::http::Response resp;
		resp.status = existed ? kHttpNoContent : kHttpCreated;
		resp.status_text = existed ? "No Content" : "Created";
		dr->complete(std::move(resp));
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
	} catch (FileIoError const &e) {
		dr->complete(
			e.code().value() == ENOENT ? conflux::http::Response::not_found(*fp) :
										 conflux::http::Response::internal_error());
	} catch (...) { dr->complete(conflux::http::Response::internal_error()); }
}
