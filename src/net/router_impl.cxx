module;
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#if defined(CONFLUX_STDSIMD)
extern "C" {
void conflux_ws_unmask_stdsimd(unsigned char *data, __SIZE_TYPE__ n, unsigned char const *mask4);
}
#endif

module conflux.net.router;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.crypto;
import conflux.work;
import conflux.file_io;
import conflux.utils;
import conflux.net.config;
import conflux.socket_io;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif


struct Router::Impl {
		struct Route {
			S method{};
			V<Segment> pattern{};
			Handler handler{};
		};
		struct SseRoute {
			V<Segment> pattern{};
			SseHandler handler{};
		};
		struct ContextRoute {
			S method{};
			V<Segment> pattern{};
			ContextHandler handler{};
		};
		V<Route> routes{};
		V<SseRoute> sse_routes{};
		V<ContextRoute> context_routes{};
		V<Middleware> middlewares{};
		Handler not_found_handler{};
		ErrorHandler error_handler{};
		SP<WorkPool> work_pool{make_shared<WorkPool>()};
		StaticFileCacheConfig static_file_cache{};
	};


namespace {

struct StaticCacheEntry {
	S body;
	S mime;
	S etag;
	S last_modified;
	S content_encoding;
	off_t size{};
	time_t mtime{};
	dev_t dev{};
	ino_t ino{};
	u64 tick{};
};
struct StaticCacheStore {
	mutex mtx;
	UM<S, StaticCacheEntry> entries;
	SZ total_bytes{};
	u64 tick{};
	[[nodiscard]] Opt<StaticCacheEntry> get(
		S const &key,
		struct ::stat const &st) {
		SL const lk{mtx};
		auto it = entries.find(key);
		if (it == entries.end()) {
			return nullopt;
		}
		auto &e = it->second;
		if (e.size != st.st_size || e.mtime != st.st_mtime || e.dev != st.st_dev || e.ino != st.st_ino) {
			total_bytes -= e.body.size();
			entries.erase(it);
			return nullopt;
		}
		e.tick = ++tick;
		return e;
	}
	void put(
		S key,
		StaticCacheEntry entry,
		SZ max_total_bytes) {
		SL const lk{mtx};
		if (entry.body.size() > max_total_bytes) {
			return;
		}
		if (auto it = entries.find(key); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
		while (total_bytes + entry.body.size() > max_total_bytes && !entries.empty()) {
			auto victim = ranges::min_element(entries, {}, [](auto const &kv) { return kv.second.tick; });
			total_bytes -= victim->second.body.size();
			entries.erase(victim);
		}
		entry.tick = ++tick;
		total_bytes += entry.body.size();
		entries.emplace(move(key), move(entry));
	}
	void evict(
		S const &key) {
		SL const lk{mtx};
		if (auto it = entries.find(key); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
	}
	void evict_all_encodings(
		S const &path) {
		evict(path + "|");
		evict(path + "|br");
		evict(path + "|gzip");
	}
};

}

conflux::work::root::Task<void> do_serve_file(
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
conflux::work::root::Task<void> do_save_file(
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
conflux::work::root::Task<void> do_delete_file(
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

Router::Router()
	: impl_(make_unique<Impl>()) {}

Router::Router(Config const &cfg)
	: impl_(make_unique<Impl>()) {
	impl_->static_file_cache = cfg.static_file_cache;
}

Router::~Router() {}

Router::Router(Router &&o) noexcept
	: impl_(move(o.impl_)) {}

Router &Router::operator =(Router &&o) noexcept {
	impl_ = move(o.impl_);
	return *this;
}

void Router::add_prepared(
	SV method,
	SV path,
	Handler handler) {
	impl_->routes.push_back({S{method}, parse_pattern(path), move(handler)});
}

void Router::add_context_prepared(
	SV method,
	SV path,
	ContextHandler handler) {
	impl_->context_routes.push_back({S{method}, parse_pattern(path), move(handler)});
}

void Router::use_prepared(Middleware mw) {
	impl_->middlewares.push_back(move(mw));
}

void Router::set_not_found_handler(Handler handler) {
	impl_->not_found_handler = move(handler);
}

void Router::set_error_handler(ErrorHandler handler) {
	impl_->error_handler = move(handler);
}

void Router::sse_prepared(
	SV path,
	SseHandler handler) {
	impl_->sse_routes.push_back({parse_pattern(path), move(handler)});
}

[[nodiscard]] bool Router::has_context_routes() const noexcept {
	return !impl_->context_routes.empty();
}

Router &Router::set_work_pool(SP<WorkPool> pool) {
	impl_->work_pool = move(pool);
	return *this;
}

[[nodiscard]] SP<WorkPool> Router::work_pool() const {
	return impl_->work_pool;
}

Router &Router::set_static_file_cache(StaticFileCacheConfig cfg) {
	impl_->static_file_cache = cfg;
	return *this;
}

Router &Router::ws_prepared(
	SV path,
	WsHandler handler) {
	add_prepared("GET", path, Handler{[h = move(handler)](HttpRequestView const &req) mutable -> HttpResponse {
		if (!ws_detail::is_valid_handshake(req)) {
			return HttpResponse::bad_request();
		}
		auto key = trim(req.headers["sec-websocket-key"]);
		auto up = make_shared<WsUpgrade>();
		up->accept_key = ws_detail::ws_accept_key(key);
		up->handler = h;
		HttpResponse r{.status = 101, .status_text = "Switching Protocols"};
		r.set_ws_upgrade(move(up));
		return r;
	}});
	return *this;
}


[[nodiscard]] V<RouteInfo> Router::route_infos() const {
		V<RouteInfo> result;
		result.reserve(impl_->routes.size());
		for (auto const &route: impl_->routes) {
			RouteInfo info;
			info.method = route.method;
			info.path_pattern = segments_to_pattern(route.pattern);
			for (auto const &seg: route.pattern) {
				if (seg.is_param || seg.is_wildcard) {
					info.path_params.push_back(seg.value);
				}
			}
			result.push_back(move(info));
		}
		return result;
	}

[[nodiscard]] HttpResponse Router::run_async_http_task(
	conflux::work::root::Task<HttpResponse> task) {
		auto deferred = make_shared<DeferredResponse>();
		auto jh = make_shared<conflux::work::root::TaskJoinHandle<HttpResponse>>(
			conflux::work::root::into_join_handle(move(task)));
		deferred->attach_cancel(jh->control());
		jh->control().set_on_ready_or_run([deferred, jh]() noexcept {
			try {
				auto outcome = conflux::work::root::join(move(*jh));
				if (outcome.is_success()) {
					deferred->complete(move(outcome).success().value);
				} else {
					deferred->complete(HttpResponse::internal_error());
				}
			} catch (exception const &ex) { deferred->complete(HttpResponse::internal_error(ex.what())); } catch (...) {
				deferred->complete(HttpResponse::internal_error());
			}
		});
		return HttpResponse::deferred(move(deferred));
	}

void Router::launch_sse_handler(
	SP<WorkPool> const &pool,
	SseHandler handler,
	HttpRequest matched,
	SP<SseChannel> const &channel) {
		if (!pool->enqueue([h = move(handler), matched = move(matched), channel]() mutable {
				HttpRequestView const matched_view{matched};
				h(matched_view, channel);
				channel->close();
			})) {
			channel->close();
		}
	}

[[nodiscard]] Router::Handler Router::wrap_middlewares(
	Handler h) const {
		return [this, h = move(h)](HttpRequestView const &req) -> HttpResponse {
			struct Step {
				Router::Impl const *impl_;
				Handler const *h_;
				size_t idx_{0};
				HttpResponse call(
					HttpRequestView const &r) {
					if (idx_ == impl_->middlewares.size()) {
						return (*h_)(r);
					}
					auto const &mw = impl_->middlewares[idx_++];
					return mw(r, [this](HttpRequestView const &rr) -> HttpResponse { return call(rr); });
				}
			};
			Step s{impl_.get(), &h};
			return s.call(req);
		};
	}

Router &Router::serve_static(
	SV url_prefix,
	S root_dir,
	StaticOptions const &sopts) {
		// Strip trailing slash from root_dir.
		while (!root_dir.empty() && root_dir.back() == '/') {
			root_dir.pop_back();
		}

		auto pattern = S{url_prefix} + "/{*file}";
		auto effective_sopts = sopts;
		if (!effective_sopts.file_cache.enabled) {
			effective_sopts.file_cache = impl_->static_file_cache;
		}
		struct StaticReq {
			S file_param;
			S method;
			S accept_encoding;
			S if_none_match;
			S if_modified_since;
			S range;
			bool tls{};
		};
		auto static_cache = make_shared<StaticCacheStore>();

		auto do_work = [static_cache](S const &rd, int root_fd, StaticOptions const &static_options, StaticReq const &r)
			-> HttpResponse {
			try {
				S file_param = r.file_param;
				auto full_path = rd + file_param;
				SV rel_path = SV{file_param};
				if (rel_path.starts_with('/')) {
					rel_path.remove_prefix(1);
				}
				S rel_str{rel_path};

				struct ::stat st{};
				int const probe_fd = rel_str.empty() ? contained_open(root_fd, ".", O_PATH | O_CLOEXEC | O_DIRECTORY) :
													   contained_open(root_fd, rel_str.c_str(), O_PATH | O_CLOEXEC);
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
					int const idx_fd = contained_open(root_fd, index_rel.c_str(), O_PATH | O_CLOEXEC);
					if (idx_fd >= 0) {
						::fstat(idx_fd, &st);
						::close(idx_fd);
						full_path = rd + "/" + index_rel;
						file_param += "/index.html";
						rel_str = index_rel;
					} else if (static_options.directory_listing) {
						int const dfd =
							rel_str.empty() ?
								contained_open(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
								contained_open(root_fd, rel_str.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
							html_escape(file_param),
							html_escape(file_param));
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
								format("<li><a href=\"{}\">{}</a></li>", path_percent_encode(name), html_escape(name));
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
						int const br_fd = contained_open(root_fd, br_rel.c_str(), O_PATH | O_CLOEXEC);
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
						int const gz_fd = contained_open(root_fd, gz_rel.c_str(), O_PATH | O_CLOEXEC);
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
					int const fd = contained_open(root_fd, rel_str.c_str(), O_RDONLY | O_CLOEXEC);
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
					do_serve_file(
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
		};

		auto root_dir_fd = make_shared<RootDirFd>(root_dir.c_str());
		auto rd = move(root_dir);
		auto normalize_path = [](SV raw) -> Opt<S> {
			S const fp{raw};
			V<S> parts;
			SZ pos = 0;
			bool bad = false;
			while (pos < fp.size()) {
				auto next = fp.find('/', pos);
				auto seg = (next == S::npos) ? SV{fp}.substr(pos) : SV{fp}.substr(pos, next - pos);
				if (seg.find('\0') != SV::npos) {
					bad = true;
					break;
				}
				if (seg == "..") {
					if (parts.empty()) {
						bad = true;
						break;
					}
					parts.pop_back();
				} else if (!seg.empty() && seg != ".") {
					parts.emplace_back(seg);
				}
				if (next == S::npos) {
					break;
				}
				pos = next + 1;
			}
			if (bad) {
				return nullopt;
			}
			S result;
			for (auto const &p: parts) {
				result += '/';
				result += p;
			}
			return result;
		};

		// NOLINTNEXTLINE(bugprone-exception-escape): lambda already handles failures via top-level try/catch.
		get(pattern,
			[rd, root_dir_fd, sopts = effective_sopts, do_work, normalize_path](
				HttpRequestView const &req) -> HttpResponse {
				try {
					auto norm = normalize_path(req.params["file"]);
					if (!norm) {
						return HttpResponse::html(
							"<html><body><h1>403 Forbidden</h1></body></html>",
							kHttpForbidden,
							"Forbidden");
					}

					StaticReq sreq{
						.file_param = move(*norm),
						.method = S{req.method},
						.accept_encoding = S{req.headers["accept-encoding"]},
						.if_none_match = S{std::as_const(req.headers)["if-none-match"]},
						.if_modified_since = S{std::as_const(req.headers)["if-modified-since"]},
						.range = S{req.headers["range"]},
						.tls = req.is_tls,
					};

					auto const rfd = root_dir_fd->fd;
					if (sopts.offload_pool) {
						auto dr = make_shared<DeferredResponse>();
						auto ok =
							sopts.offload_pool->enqueue([rd, rfd, sopts, sreq = move(sreq), do_work, dr]() mutable {
								try {
									dr->complete(do_work(rd, rfd, sopts, sreq));
								} catch (...) { dr->complete(HttpResponse::internal_error()); }
							});
						if (!ok) {
							return HttpResponse::internal_error("offload queue full");
						}
						return HttpResponse::deferred(move(dr));
					}

					return do_work(rd, rfd, sopts, sreq);
				} catch (...) { return HttpResponse::internal_error(); }
			});

		if (effective_sopts.allow_put) {
			// NOLINTNEXTLINE(bugprone-exception-escape)
			put(pattern,
				[rd, root_dir_fd, sopts = effective_sopts, static_cache, normalize_path](
					HttpRequestView const &req) -> HttpResponse {
					try {
						auto norm = normalize_path(req.params["file"]);
						if (!norm) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto full_path = rd + *norm;
						SV rel_sv = SV{*norm};
						if (rel_sv.starts_with('/')) {
							rel_sv.remove_prefix(1);
						}
						S rel{rel_sv};

						int const probe = contained_open(root_dir_fd->fd, rel.c_str(), O_PATH | O_CLOEXEC);
						bool const existed = probe >= 0;
						if (probe >= 0) {
							::close(probe);
						}

						if (auto *fr = current_file_reader(); fr != nullptr) {
							auto body_owned = make_shared<S>(req.body);
							auto dr = make_shared<DeferredResponse>();
							auto fp = make_shared<S>(full_path);
							do_save_file(fr, body_owned, fp, existed, static_cache, dr, root_dir_fd->fd, S{rel})
								.detach();
							return HttpResponse::deferred(move(dr));
						}

						if (sopts.offload_pool) {
							auto dr = make_shared<DeferredResponse>();
							auto body_owned = make_shared<S>(req.body);
							auto rfd = root_dir_fd->fd;
							auto ok = sopts.offload_pool->enqueue([full_path = move(full_path),
																   rel = move(rel),
																   rfd,
																   body_owned,
																   existed,
																   static_cache,
																   dr]() mutable {
								auto r = write_text_file_atomic_at_sync(rfd, SV{rel}, SV{*body_owned});
								if (!r) {
									dr->complete(HttpResponse::internal_error());
									return;
								}
								static_cache->evict_all_encodings(full_path);
								HttpResponse resp;
								resp.status = existed ? kHttpNoContent : kHttpCreated;
								resp.status_text = existed ? "No Content" : "Created";
								dr->complete(move(resp));
							});
							if (!ok) {
								return HttpResponse::internal_error("offload queue full");
							}
							return HttpResponse::deferred(move(dr));
						}

						if (!write_text_file_atomic_at_sync(root_dir_fd->fd, SV{rel}, SV{req.body})) {
							return HttpResponse::internal_error();
						}
						static_cache->evict_all_encodings(full_path);
						HttpResponse resp;
						resp.status = existed ? kHttpNoContent : kHttpCreated;
						resp.status_text = existed ? "No Content" : "Created";
						return resp;
					} catch (...) { return HttpResponse::internal_error(); }
				});
		}

		if (effective_sopts.allow_delete) {
			// NOLINTNEXTLINE(bugprone-exception-escape)
			del(pattern,
				[rd, root_dir_fd, sopts = effective_sopts, static_cache, normalize_path](
					HttpRequestView const &req) -> HttpResponse {
					try {
						auto norm = normalize_path(req.params["file"]);
						if (!norm) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto full_path = rd + *norm;
						SV rel_sv = SV{*norm};
						if (rel_sv.starts_with('/')) {
							rel_sv.remove_prefix(1);
						}
						S rel{rel_sv};

						int const probe = contained_open(root_dir_fd->fd, rel.c_str(), O_PATH | O_CLOEXEC);
						if (probe < 0) {
							return errno == ENOENT ? HttpResponse::not_found(*norm) : HttpResponse::forbidden();
						}
						::close(probe);

						if (auto *fr = current_file_reader(); fr != nullptr) {
							auto dr = make_shared<DeferredResponse>();
							auto fp = make_shared<S>(full_path);
							do_delete_file(dr, fp, static_cache, fr->unlink_async(root_dir_fd->fd, rel)).detach();
							return HttpResponse::deferred(move(dr));
						}

						if (sopts.offload_pool) {
							auto dr = make_shared<DeferredResponse>();
							auto rfd = root_dir_fd->fd;
							auto ok = sopts.offload_pool->enqueue(
								[full_path = move(full_path), rel = move(rel), rfd, static_cache, dr]() mutable {
									try {
										if (::unlinkat(rfd, rel.c_str(), 0) != 0) {
											dr->complete(
												errno == ENOENT ? HttpResponse::not_found(full_path) :
																  HttpResponse::internal_error());
											return;
										}
										static_cache->evict_all_encodings(full_path);
										dr->complete(HttpResponse::no_content());
									} catch (...) { dr->complete(HttpResponse::internal_error()); }
								});
							if (!ok) {
								return HttpResponse::internal_error("offload queue full");
							}
							return HttpResponse::deferred(move(dr));
						}

						if (::unlinkat(root_dir_fd->fd, rel.c_str(), 0) != 0) {
							return errno == ENOENT ? HttpResponse::not_found(*norm) : HttpResponse::internal_error();
						}
						static_cache->evict_all_encodings(full_path);
						return HttpResponse::no_content();
					} catch (...) { return HttpResponse::internal_error(); }
				});
		}

		return *this;
	}

[[nodiscard]] HttpResponse Router::dispatch(
	HttpRequest const &req) const {
		HttpRequestView const req_view{req};
		return dispatch(req_view);
	}

[[nodiscard]] HttpResponse Router::dispatch(
	HttpRequestView const &req) const {
		// HEAD is dispatched as GET; response body is suppressed before sending.
		bool const is_head = (req.method == "HEAD");

		// Strip query S before matching.
		auto path_sv = SV{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}

		// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
		Handler inner = [this, path_sv, is_head](HttpRequestView const &r) -> HttpResponse {
			try {
				HttpFieldsView matched_params;

				// Regular routes first.
				for (auto const &route: impl_->routes) {
					if (route.method != r.method && !(is_head && route.method == "GET")) {
						continue;
					}
					matched_params.clear();
					if (match_segments(route.pattern, path_sv, matched_params)) {
						auto all_params = r.params;
						for (auto const &[k, v]: matched_params) {
							if (!all_params.get(k)) {
								all_params.emplace_back(k, v);
							}
						}
						// HEAD matched to a GET route: present as GET so handlers are HEAD-transparent.
						SV const effective_method = (is_head && route.method == "GET") ? SV{"GET"} : r.method;
						HttpRequestView const matched_view{
							effective_method,
							r.path,
							r.version,
							r.remote_addr,
							r.is_tls,
							move(all_params),
							r.headers,
							r.query,
							r.form,
							r.cookies,
							r.files,
							r.body};
						try {
							auto resp = route.handler(matched_view);
							if (is_head) {
								resp.head_only = true;
							}
							return resp;
						} catch (exception const &ex) {
							return impl_->error_handler ? impl_->error_handler(matched_view, ex) :
														  HttpResponse::internal_error(ex.what());
						} catch (...) {
							return impl_->error_handler ? impl_->error_handler(matched_view, RE{"unknown exception"}) :
														  HttpResponse::internal_error();
						}
					}
				}

				// SSE routes (GET only).
				if (r.method == "GET") {
					for (auto const &route: impl_->sse_routes) {
						matched_params.clear();
						if (match_segments(route.pattern, path_sv, matched_params)) {
							auto channel = make_shared<SseChannel>();
							HttpRequest matched = r.to_owned();
							for (auto &[k, v]: matched_params) {
								matched.params.emplace_back(S{k}, S{v});
							}
							launch_sse_handler(impl_->work_pool, route.handler, move(matched), channel);
							return HttpResponse::sse(move(channel));
						}
					}
				}

				if (impl_->not_found_handler) {
					return impl_->not_found_handler(r);
				}
				return HttpResponse::not_found(path_sv);
			} catch (...) { return HttpResponse::internal_error(); }
		};

		return wrap_middlewares(move(inner))(req);
	}

[[nodiscard]] Opt<HttpResponse> Router::dispatch_async(
	HttpRequest const &req,
	RequestContext const &ctx) const {
		if (impl_->context_routes.empty()) {
			return nullopt;
		}
		bool const is_head = (req.method == "HEAD");
		SV path_sv{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}
		HttpFieldsView matched_params;
		for (auto const &route: impl_->context_routes) {
			if (route.method != req.method && !(is_head && route.method == "GET")) {
				continue;
			}
			matched_params.clear();
			if (match_segments(route.pattern, path_sv, matched_params)) {
				HttpRequest call_req = req;
				for (auto const &[k, v]: matched_params) {
					if (!call_req.params.get(k)) {
						call_req.params.emplace_back(S{k}, S{v});
					}
				}
				return run_async_http_task(route.handler(call_req, ctx));
			}
		}
		return nullopt;
	}
