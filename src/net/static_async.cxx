module;
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
export module conflux.net.http.static_async;

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.file_map;
import conflux.net.http.types;
import conflux.net.http.static_files;
import conflux.net.http.response;
import conflux.net.http.static_core;


namespace {

int contained_static_open(
	int root_fd,
	char const *relative,
	int flags,
	mode_t mode = 0) noexcept {
	open_how how{};
	how.flags = static_cast<__u64>(flags);
	how.mode = static_cast<__u64>(mode);
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
	return static_cast<int>(::syscall(SYS_openat2, root_fd, relative, &how, sizeof(how)));
}

[[nodiscard]] S static_html_escape(
	SV s) {
	S out;
	out.reserve(s.size());
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
	return out;
}

[[nodiscard]] S static_path_percent_encode(
	SV s) {
	S out;
	out.reserve(s.size());
	static constexpr char kHex[] = "0123456789ABCDEF";
	for (char const ch: s) {
		auto const c = static_cast<unsigned char>(ch);
		bool const safe = (c >= 'A' && c <= 'Z')
			|| (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9')
			|| c == '-'
			|| c == '_'
			|| c == '.'
			|| c == '~'
			|| c == '/';
		if (safe) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4U]);
			out.push_back(kHex[c & 0x0FU]);
		}
	}
	return out;
}

}

export conflux::work::root::Task<void> do_serve_static_file(
	SP<DeferredResponse> dr,
	HttpResponse base,
	SZ send_off,
	SZ send_sz,
	SZ total_size,
	conflux::work::root::Task<FileHandle> open_task);
export conflux::work::root::Task<void> do_save_static_file(
	FileReader *fr,
	SP<S> body_owned,
	SP<S> fp,
	bool existed,
	SP<StaticCacheStore> static_cache,
	SP<DeferredResponse> dr,
	int dir_fd,
	S rel_path);
export conflux::work::root::Task<void> do_delete_static_file(
	SP<DeferredResponse> dr,
	SP<S> fp,
	SP<StaticCacheStore> static_cache,
	conflux::work::root::Task<void> unlink_task);

export HttpResponse handle_static_get(
	S const &rd,
	int root_fd,
	StaticOptions const &static_options,
	StaticRequest const &r,
	SP<StaticCacheStore> const &static_cache)
{
			try {
				S file_param = r.file_param;
				auto full_path = rd + file_param;
				SV rel_path = SV{file_param};
				if (rel_path.starts_with('/')) {
					rel_path.remove_prefix(1);
				}
				S rel_str{rel_path};

				struct ::stat st{};
				int const probe_fd = rel_str.empty() ? contained_static_open(root_fd, ".", O_PATH | O_CLOEXEC | O_DIRECTORY) :
													   contained_static_open(root_fd, rel_str.c_str(), O_PATH | O_CLOEXEC);
				if (probe_fd < 0) {
					return HttpResponse::not_found(file_param);
				}
				if (::fstat(probe_fd, &st) != 0) {
					::close(probe_fd);
					return HttpResponse::not_found(file_param);
				}
				::close(probe_fd);

				if (S_ISDIR(st.st_mode)) {
					auto index_rel = rel_str.empty() ? S{"index.html"} : rel_str + "/index.html";
					int const idx_fd = contained_static_open(root_fd, index_rel.c_str(), O_PATH | O_CLOEXEC);
					if (idx_fd >= 0) {
						::fstat(idx_fd, &st);
						::close(idx_fd);
						full_path = rd + "/" + index_rel;
						file_param += "/index.html";
						rel_str = index_rel;
					} else if (static_options.directory_listing) {
						int const dfd =
							rel_str.empty() ?
								contained_static_open(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
								contained_static_open(root_fd, rel_str.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
						if (dfd < 0) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto *dir = ::fdopendir(dfd);
						if (dir == nullptr) {
							::close(dfd);
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						S html = format(
							"<html><head><title>Index of {}</title></head>"
							"<body><h1>Index of {}</h1><ul>",
							static_html_escape(file_param),
							static_html_escape(file_param));
						if (!file_param.empty() && file_param != "/") {
							html += "<li><a href=\"../\">..</a></li>";
						}
						struct ::dirent *ent{};
						V<S> names;
						while ((ent = ::readdir(dir)) != nullptr) {
							// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
							SV const n{ent->d_name};
							if (n == "." || n == "..") {
								continue;
							}
							names.emplace_back(n);
						}
						::closedir(dir);
						ranges::sort(names);
						for (auto const &name: names) {
							html +=
								format("<li><a href=\"{}\">{}</a></li>", static_path_percent_encode(name), static_html_escape(name));
						}
						html += "</ul></body></html>";
						return HttpResponse::html(move(html));
					} else {
						return HttpResponse::html(
							"<html><body><h1>403 Forbidden</h1></body></html>",
							kHttpForbidden,
							"Forbidden");
					}
				}

				// Pre-compressed sidecar: try .br then .gz.
				S content_encoding;
				if (static_options.precompressed) {
					auto const &accept_enc = r.accept_encoding;
					auto encoding_accepted = [&](SV token) -> bool {
						SV const ae{accept_enc};
						SZ pos = 0;
						while (pos < ae.size()) {
							auto comma = ae.find(',', pos);
							SV entry = ae.substr(pos, comma == SV::npos ? SV::npos : comma - pos);
							pos = comma == SV::npos ? ae.size() : comma + 1;
							while (!entry.empty() && entry.front() == ' ') {
								entry.remove_prefix(1);
							}
							while (!entry.empty() && entry.back() == ' ') {
								entry.remove_suffix(1);
							}
							auto semi = entry.find(';');
							SV coding = entry.substr(0, semi);
							while (!coding.empty() && coding.back() == ' ') {
								coding.remove_suffix(1);
							}
							bool const match =
								coding.size() == token.size() && ranges::equal(coding, token, [](char a, char b) {
									return (static_cast<unsigned char>(a) | 0x20)
										== (static_cast<unsigned char>(b) | 0x20);
								});
							if (!match) {
								continue;
							}
							if (semi == SV::npos) {
								return true;
							}
							auto params = entry.substr(semi + 1);
							auto q_pos = params.find("q=");
							if (q_pos == SV::npos) {
								q_pos = params.find("Q=");
							}
							if (q_pos == SV::npos) {
								return true;
							}
							auto qval = params.substr(q_pos + 2);
							auto qend = qval.find_first_of(", ;");
							if (qend != SV::npos) {
								qval = qval.substr(0, qend);
							}
							return qval != "0" && qval != "0." && qval != "0.0" && qval != "0.00" && qval != "0.000";
						}
						return false;
					};
					if (encoding_accepted("br")) {
						auto br_rel = rel_str + ".br";
						int const br_fd = contained_static_open(root_fd, br_rel.c_str(), O_PATH | O_CLOEXEC);
						if (br_fd >= 0) {
							struct ::stat br_st{};
							if (::fstat(br_fd, &br_st) == 0) {
								full_path = rd + "/" + br_rel;
								st = br_st;
								content_encoding = "br";
								rel_str = br_rel;
							}
							::close(br_fd);
						}
					}
					if (content_encoding.empty() && encoding_accepted("gzip")) {
						auto gz_rel = rel_str + ".gz";
						int const gz_fd = contained_static_open(root_fd, gz_rel.c_str(), O_PATH | O_CLOEXEC);
						if (gz_fd >= 0) {
							struct ::stat gz_st{};
							if (::fstat(gz_fd, &gz_st) == 0) {
								full_path = rd + "/" + gz_rel;
								st = gz_st;
								content_encoding = "gzip";
								rel_str = gz_rel;
							}
							::close(gz_fd);
						}
					}
				}

				// Build ETag from size + mtime.
				auto etag = format("\"{:x}-{:x}\"", st.st_size, st.st_mtime);

				// Format Last-Modified. Thread-local cache keyed on mtime — a hot
				// directory typically serves many files sharing a handful of mtimes,
				// so strftime runs once per mtime value per thread.
				thread_local time_t last_mtime_cached = 0;
				thread_local S last_modified_cached;
				S last_modified;
				if (st.st_mtime == last_mtime_cached && !last_modified_cached.empty()) {
					last_modified = last_modified_cached;
				} else {
					tm tm_val{};
					::gmtime_r(&st.st_mtime, &tm_val);
					A<char, 64> buf{};
					if (strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm_val) > 0) {
						last_modified = buf.data();
						last_modified_cached = last_modified;
						last_mtime_cached = st.st_mtime;
					}
				}

				// 304 Not Modified checks.
				if (auto const &inm = r.if_none_match; !inm.empty() && inm == etag) {
					HttpResponse resp;
					resp.status = kHttpNotModified;
					resp.status_text = "Not Modified";
					resp.content_type.clear();
					resp.set_text_body({});
					return resp;
				}
				if (auto const &ims = r.if_modified_since; !ims.empty()) {
					tm req_tm{};
					if (::strptime(ims.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &req_tm)) {
						req_tm.tm_isdst = 0;
						if (st.st_mtime <= ::timegm(&req_tm)) {
							HttpResponse resp;
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
				SV mime = "application/octet-stream";
				if (ext_pos != S::npos) {
					auto ext = SV{file_param}.substr(ext_pos);
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

				auto file_size = static_cast<SZ>(st.st_size);

				auto base_response = [&](int status, SV status_text) {
					HttpResponse resp{.status = status, .status_text = S{status_text}, .content_type = S{mime}};
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
				SZ range_start = 0;
				SZ range_end = file_size - 1;
				bool is_range_request = false;
				if (content_encoding.empty()) {
					auto const &range_hdr = r.range;
					if (!range_hdr.empty() && range_hdr.starts_with("bytes=")) {
						auto spec = SV{range_hdr}.substr(6);
						auto dash = spec.find('-');
						if (dash != SV::npos) {
							auto start_sv = spec.substr(0, dash);
							auto end_sv = spec.substr(dash + 1);
							SZ rs = 0;
							SZ re = file_size - 1;
							bool ok = true;
							if (!start_sv.empty()) {
								auto [p, ec] =
									from_chars(start_sv.data(), ranges::next(start_sv.data(), ssize(start_sv)), rs);
								if (ec != errc{}) {
									ok = false;
								}
							}
							if (!end_sv.empty()) {
								if (start_sv.empty()) {
									// suffix range: "-N" = last N bytes; not implemented
									ok = false;
								} else {
									auto [p, ec] =
										from_chars(end_sv.data(), ranges::next(end_sv.data(), ssize(end_sv)), re);
									if (ec != errc{}) {
										ok = false;
									}
								}
							} else if (start_sv.empty()) {
								ok = false; // both empty: malformed
							}
							if (ok && rs <= re && re < file_size) {
								range_start = rs;
								range_end = re;
								is_range_request = true;
							} else if (ok) {
								// Range not satisfiable
								auto resp = HttpResponse{};
								resp.status = kHttpRangeNotSatisfiable;
								resp.status_text = "Range Not Satisfiable";
								resp.content_type = "text/plain; charset=utf-8";
								resp.headers["Content-Range"] = format("bytes */{}", file_size);
								return resp;
							}
						}
					}
				}

				if (static_options.file_cache.enabled && file_size <= static_options.file_cache.small_file_max_bytes) {
					auto const cache_key = full_path + "|" + content_encoding;
					auto make_cached_response = [&](StaticCacheEntry const &entry) {
						if (is_range_request) {
							auto send_sz = range_end - range_start + 1;
							auto resp = HttpResponse{
								.status = kHttpPartialContent,
								.status_text = "Partial Content",
								.content_type = entry.mime};
							resp.headers["ETag"] = entry.etag;
							resp.headers["Last-Modified"] = entry.last_modified;
							resp.headers["Accept-Ranges"] = "bytes";
							resp.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
							if (!entry.content_encoding.empty()) {
								resp.headers["Content-Encoding"] = entry.content_encoding;
							}
							if (!static_options.cache_control.empty()) {
								resp.headers["Cache-Control"] = static_options.cache_control;
							}
							resp.set_text_body(entry.body.substr(range_start, send_sz));
							return resp;
						}
						auto resp = HttpResponse{.status = kHttpOk, .status_text = "OK", .content_type = entry.mime};
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
					if (auto cached = static_cache->get(cache_key, st)) {
						return make_cached_response(*cached);
					}
					int const fd = contained_static_open(root_fd, rel_str.c_str(), O_RDONLY | O_CLOEXEC);
					if (fd < 0) {
						return HttpResponse::not_found(file_param);
					}
					S body(file_size, '\0');
					SZ off = 0;
					while (off < body.size()) {
						ssize_t const n = ::read(fd, body.data() + off, body.size() - off);
						if (n < 0) {
							if (errno == EINTR) {
								continue;
							}
							::close(fd);
							return HttpResponse::internal_error();
						}
						if (n == 0) {
							break;
						}
						off += static_cast<SZ>(n);
					}
					::close(fd);
					if (off != body.size()) {
						body.resize(off);
					}
					StaticCacheEntry entry{
						.body = move(body),
						.mime = S{mime},
						.etag = etag,
						.last_modified = last_modified,
						.content_encoding = content_encoding,
						.size = st.st_size,
						.mtime = st.st_mtime,
						.dev = st.st_dev,
						.ino = st.st_ino};
					auto resp = make_cached_response(entry);
					static_cache->put(cache_key, move(entry), static_options.file_cache.max_total_bytes);
					return resp;
				}

				// Async uring path: when a FileReader is installed for this
				// thread (i.e. we are running on a ring thread, not offloaded to
				// a WorkPool) and no compressed variant was picked, open the
				// file via IORING_OP_OPENAT and return a deferred response that
				// carries a StreamedFile once the open CQE fires. Otherwise
				// fall back to the synchronous mmap path below.
				if (auto *fr = current_file_reader(); fr != nullptr && !r.tls && content_encoding.empty()) {
					auto dr = make_shared<DeferredResponse>();
					auto base = is_range_request ? base_response(kHttpPartialContent, "Partial Content") :
												   base_response(kHttpOk, "OK");
					if (is_range_request) {
						base.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
					}
					auto const send_off = is_range_request ? range_start : SZ{0};
					auto const send_sz = is_range_request ? (range_end - range_start + 1) : file_size;
					do_serve_static_file(
						dr,
						move(base),
						send_off,
						send_sz,
						file_size,
						fr->openat2_async(
							root_fd,
							S{rel_str},
							open_how{
								.flags = static_cast<__u64>(O_RDONLY | O_CLOEXEC),
								.mode = 0,
								.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS}))
						.detach();
					return HttpResponse::deferred(move(dr));
				}

				auto lease = map_file_readonly_sync(root_fd, SV{rel_str});
				if (!lease) {
					return HttpResponse::internal_error();
				}

				if (is_range_request) {
					auto send_sz = range_end - range_start + 1;
					auto resp = base_response(kHttpPartialContent, "Partial Content");
					resp.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
					resp.set_mapped_file(
						make_shared<MappedBody>(
							MappedBody{.lease = move(*lease), .offset = range_start, .size = send_sz}));
					return resp;
				}

				auto resp = base_response(kHttpOk, "OK");
				resp.set_mapped_file(
					make_shared<MappedBody>(MappedBody{.lease = move(*lease), .offset = 0, .size = file_size}));
				return resp;
			} catch (...) { return HttpResponse::internal_error(); }
		}

conflux::work::root::Task<void> do_serve_static_file(
	SP<DeferredResponse> dr,
	HttpResponse base,
	SZ send_off,
	SZ send_sz,
	SZ total_size,
	conflux::work::root::Task<FileHandle> open_task) {
	try {
		auto fh = co_await move(open_task);
		base.set_streamed_file(
			make_shared<StreamedFile>(StreamedFile{
				.handle = make_shared<FileHandle>(move(fh)),
				.send_offset = send_off,
				.send_size = send_sz,
				.total_size = total_size}));
		dr->complete(move(base));
	} catch (...) { dr->complete(HttpResponse::not_found("async open failed")); }
}
conflux::work::root::Task<void> do_save_static_file(
	FileReader *fr,
	SP<S> body_owned,
	SP<S> fp,
	bool existed,
	SP<StaticCacheStore> static_cache,
	SP<DeferredResponse> dr,
	int dir_fd,
	S rel_path) {
	try {
		co_await fr->atomic_write_async(dir_fd, move(rel_path), as_bytes(span{*body_owned}));
		static_cache->evict_all_encodings(*fp);
		HttpResponse resp;
		resp.status = existed ? kHttpNoContent : kHttpCreated;
		resp.status_text = existed ? "No Content" : "Created";
		dr->complete(move(resp));
	} catch (...) { dr->complete(HttpResponse::internal_error()); }
}
conflux::work::root::Task<void> do_delete_static_file(
	SP<DeferredResponse> dr,
	SP<S> fp,
	SP<StaticCacheStore> static_cache,
	conflux::work::root::Task<void> unlink_task) {
	try {
		co_await move(unlink_task);
		static_cache->evict_all_encodings(*fp);
		dr->complete(HttpResponse::no_content());
	} catch (FileIoError const &e) {
		dr->complete(e.code().value() == ENOENT ? HttpResponse::not_found(*fp) : HttpResponse::internal_error());
	} catch (...) { dr->complete(HttpResponse::internal_error()); }
}
