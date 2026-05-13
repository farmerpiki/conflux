module;
#include <ctime>
export module conflux.net.router;


import std;
import conflux.types;
import conflux.net.http.types;
import conflux.work;
import conflux.utils;
import conflux.net.config;
import conflux.socket_io;
export import conflux.net.http.server_types;
export import conflux.net.http.realtime;
export import conflux.net.http.static_files;
export import conflux.net.http.response;
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

struct Segment {
	S value;
	bool is_param; // true → {name} single-segment capture
	bool is_wildcard; // true → {*name} greedy tail capture (must be last segment)
};
V<Segment> parse_pattern(
	SV pattern) {
	V<Segment> segs;
	SZ pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == SV::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({S{part.substr(2, part.size() - 3)}, false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({S{part.substr(1, part.size() - 2)}, true, false});
		} else {
			segs.push_back({S{part}, false, false});
		}

		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}
bool match_segments(
	V<Segment> const &pattern,
	SV path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		SZ pos = 0;
		HttpFieldsView tmp;
		for (SZ i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == SV::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(part));
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == SV::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		tmp.emplace_back_owned(S{pattern.back().value}, url_decode_path(path.substr(pos)));
		out_params = move(tmp);
		return true;
	}

	V<SV> parts;
	SZ pos = 0;
	while (true) {
		auto next = path.find('/', pos);
		parts.push_back((next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos));
		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}

	if (parts.size() != pattern.size()) {
		return false;
	}

	HttpFieldsView tmp;
	for (SZ i = 0; i < pattern.size(); ++i) {
		if (pattern[i].is_param) {
			tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(parts[i]));
		} else if (pattern[i].value != parts[i]) {
			return false;
		}
	}
	out_params = move(tmp);
	return true;
}
// Metadata for a single registered route, exposed by Router::route_infos().
export struct RouteInfo {
	S method;
	S path_pattern; // OpenAPI-style path e.g. /users/{id}
	V<S> path_params; // captured parameter names in order
};
// Reconstruct an OpenAPI path S from a parsed Segment V.
// The first segment is always an empty literal (artifact of the leading '/');
// skip it so the result starts with a single '/'.
inline S segments_to_pattern(
	V<Segment> const &segs) {
	S out;
	bool first = true;
	for (auto const &seg: segs) {
		if (first && seg.value.empty() && !seg.is_param && !seg.is_wildcard) {
			first = false;
			continue; // skip leading empty segment
		}
		first = false;
		out += '/';
		if (seg.is_wildcard || seg.is_param) {
			out += '{';
			out += seg.value;
			out += '}';
		} else {
			out += seg.value;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}
export struct RequestContext {
	SocketTaskRing &ring;
};

export using NextHandler = CloneableFunction<HttpResponse(HttpRequestView const &)>;
export using MiddlewareFunction = CloneableFunction<HttpResponse(HttpRequestView const &, NextHandler const &)>;

export template<class R>
concept HandlerResult = same_as<R, HttpResponse> || same_as<R, conflux::work::root::Task<HttpResponse>>;

export template<class F>
concept ViewHandler = requires(std::decay_t<F> &fn, HttpRequestView const &req) {
	{ std::invoke(fn, req) } -> same_as<HttpResponse>;
};

export template<class F>
concept RequestHandler = requires(std::decay_t<F> &fn, HttpRequest const &req) {
	{ std::invoke(fn, req) } -> HandlerResult;
};

export template<class F>
concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

export template<class F>
concept ContextHandlerFunction = requires(std::decay_t<F> &fn, HttpRequest const &req, RequestContext const &ctx) {
	{ std::invoke(fn, req, ctx) } -> same_as<conflux::work::root::Task<HttpResponse>>;
};

export template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, HttpRequestView const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> same_as<HttpResponse>;
};

export template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, HttpRequest const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> same_as<HttpResponse>;
};

export template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F>;

export class Router {
public:
	using Handler = NextHandler;
	using ContextHandler =
		CloneableFunction<conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &)>;
	using ContextMiddleware = CloneableFunction<
		conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &, ContextHandler const &)>;
	using SseHandler = CloneableFunction<void(HttpRequestView const &, SP<SseChannel>)>;
	// next is the downstream handler (or next middleware); call it to continue the chain.
	using Middleware = MiddlewareFunction;
	using WsHandler = CloneableFunction<void(HttpRequestView const &, WsConn &)>;
	using ErrorHandler = CloneableFunction<HttpResponse(HttpRequestView const &, exception const &)>;
	Router();
	explicit Router(Config const &cfg);
	~Router();
	Router(Router const &) = delete;
	Router &operator =(Router const &) = delete;
	Router(Router &&o) noexcept;
	Router &operator =(Router &&o) noexcept;
	// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks): false positive on CloneableFunction ownership.
	template<typename F>
	Router &add(
		SV method,
		SV path,
		F &&handler) {
		add_prepared(method, path, make_handler(forward<F>(handler)));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	Router &add_context(
		SV method,
		SV path,
		F &&handler) {
		add_context_prepared(method, path, ContextHandler{forward<F>(handler)});
		return *this;
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
	[[nodiscard]] bool has_context_routes() const noexcept;
	template<typename F>
	Router &get(
		SV path,
		F &&handler) {
		return add("GET", path, forward<F>(handler));
	}
	template<typename F>
	Router &post(
		SV path,
		F &&handler) {
		return add("POST", path, forward<F>(handler));
	}
	template<typename F>
	Router &put(
		SV path,
		F &&handler) {
		return add("PUT", path, forward<F>(handler));
	}
	template<typename F>
	Router &patch(
		SV path,
		F &&handler) {
		return add("PATCH", path, forward<F>(handler));
	}
	template<typename F>
	Router &del(
		SV path,
		F &&handler) {
		return add("DELETE", path, forward<F>(handler));
	}
	template<typename F>
	Router &options(
		SV path,
		F &&handler) {
		return add("OPTIONS", path, forward<F>(handler));
	}
	template<typename F>
	Router &use(
		F &&mw) {
		use_prepared(make_middleware(forward<F>(mw)));
		return *this;
	}
	template<typename F>
	Router &on_not_found(
		F &&handler) {
		set_not_found_handler(make_handler(forward<F>(handler)));
		return *this;
	}
	template<typename F>
	Router &on_error(
		F &&handler) {
		set_error_handler(make_error_handler(forward<F>(handler)));
		return *this;
	}
	// Return metadata for all registered routes (regular routes only).
	[[nodiscard]] V<RouteInfo> route_infos() const;
	template<typename F>
	Router &sse(
		SV path,
		F &&handler) {
		sse_prepared(path, make_sse_handler(forward<F>(handler)));
		return *this;
	}
	Router &set_work_pool(SP<WorkPool> pool);
	[[nodiscard]] SP<WorkPool> work_pool() const;
	Router &set_static_file_cache(StaticFileCacheConfig cfg);
	// Register a WebSocket upgrade handler. GET requests with a valid Upgrade: websocket
	// handshake are upgraded to WebSocket; the handler runs on the router's work pool.
	template<typename F>
	Router &ws(
		SV path,
		F &&handler) {
		return ws_prepared(path, make_ws_handler(forward<F>(handler)));
	}
	// Route group: scopes a set of routes under a path prefix with Opt group-local middleware.
	// Group middleware wraps only the routes registered inside the group callback;
	// it does NOT affect routes registered outside. The group callback receives a Group&.
	class Group {
	public:
		template<typename F>
		Group &use(
			F &&mw) {
			middlewares_.push_back(Router::make_middleware(forward<F>(mw)));
			return *this;
		}
		template<typename F>
		Group &add(
			SV method,
			SV path,
			F &&handler) {
			router_.add(method, prefix_ + S{path}, wrap(Router::make_handler(forward<F>(handler))));
			return *this;
		}
		template<typename F>
		Group &get(
			SV path,
			F &&handler) {
			return add("GET", path, forward<F>(handler));
		}
		template<typename F>
		Group &post(
			SV path,
			F &&handler) {
			return add("POST", path, forward<F>(handler));
		}
		template<typename F>
		Group &put(
			SV path,
			F &&handler) {
			return add("PUT", path, forward<F>(handler));
		}
		template<typename F>
		Group &patch(
			SV path,
			F &&handler) {
			return add("PATCH", path, forward<F>(handler));
		}
		template<typename F>
		Group &del(
			SV path,
			F &&handler) {
			return add("DELETE", path, forward<F>(handler));
		}
		template<typename F>
		Group &options(
			SV path,
			F &&handler) {
			return add("OPTIONS", path, forward<F>(handler));
		}

	private:
		friend class Router;
		Group(
			Router &router,
			S prefix)
			: router_(router)
			, prefix_(move(prefix))
			, middlewares_{} {}
		// Apply group middlewares around h (innermost first, so first-registered is outermost).
		// Capture mw by value: the Group object is destroyed after router.group() returns,
		// so capturing by reference would dangle.
		[[nodiscard]] Handler wrap(
			Handler h) const {
			for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
				auto mw = middlewares_[static_cast<SZ>(i)]; // copy: Group is destroyed after group() returns
				h = [mw = move(mw), n = move(h)](HttpRequestView const &r) { return mw(r, n); };
			}
			return h;
		}
		Router &router_;
		S prefix_;
		V<Middleware> middlewares_;
	};
	template<typename F>
	Router &group(
		SV prefix,
		F &&fn) {
		Group g{*this, S{prefix}};
		forward<F>(fn)(g);
		return *this;
	}
	// Serve static files from root_dir for GET/HEAD requests under url_prefix.
	// url_prefix must not end with '/'. Files are served at url_prefix/{*file}.
	// Path traversal ("..") is rejected with 403.
	// ETag based on size+mtime; Range requests (206 Partial Content) supported.
	// Pre-compressed sidecar files (.gz, .br) served when client accepts them.
	Router &serve_static(
		SV url_prefix,
		S root_dir,
		StaticOptions const &sopts = {});
	[[nodiscard]] HttpResponse dispatch(HttpRequest const &req) const;
	[[nodiscard]] HttpResponse dispatch(HttpRequestView const &req) const;
	[[nodiscard]] Opt<HttpResponse> dispatch_async(
		HttpRequest const &req,
		RequestContext const &ctx) const;

private:
	struct Impl;
	UP<Impl> impl_;
	void add_prepared(SV method, SV path, Handler handler);
	void add_context_prepared(SV method, SV path, ContextHandler handler);
	void use_prepared(Middleware mw);
	void set_not_found_handler(Handler handler);
	void set_error_handler(ErrorHandler handler);
	void sse_prepared(SV path, SseHandler handler);
	Router &ws_prepared(SV path, WsHandler handler);
	static void launch_sse_handler(
		SP<WorkPool> const &pool,
		SseHandler handler,
		HttpRequest matched,
		SP<SseChannel> const &channel);
	[[nodiscard]] Handler wrap_middlewares(Handler h) const;
	[[nodiscard]] static HttpResponse run_async_http_task(
		conflux::work::root::Task<HttpResponse> task);
	template<class>
	static constexpr bool kDependentFalse = false;
	template<typename F>
	static Handler make_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequestView const &>;
			if constexpr (same_as<Ret, HttpResponse>) {
				return Handler{forward<F>(fn)};
			} else if constexpr (same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				static_assert(
					kDependentFalse<Fn>,
					"Async handlers must take HttpRequest const&, not HttpRequestView const& — "
					"the view can dangle after coroutine suspension");
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler taking HttpRequestView const& must return HttpResponse (sync only)");
			}
		} else if constexpr (std::invocable<Fn &, HttpRequest const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequest const &>;
			if constexpr (same_as<Ret, HttpResponse>) {
				return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned);
				}};
			} else if constexpr (same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return run_async_http_task(invoke(wrapped, owned));
				}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler returning HttpRequest const& must return HttpResponse or root::Task<HttpResponse>");
			}
		} else {
			static_assert(kDependentFalse<Fn>, "Handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static Middleware make_middleware(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, Handler const &>) {
			return Middleware{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, Handler const &>) {
			return Middleware{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, Handler const &next) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, next);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Middleware must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static SseHandler make_sse_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, SP<SseChannel>>) {
			return SseHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, SP<SseChannel>>) {
			return SseHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, SP<SseChannel> ch) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, move(ch));
			}};
		} else {
			static_assert(kDependentFalse<Fn>, "SSE handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static WsHandler make_ws_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, WsConn &>) {
			return WsHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, WsConn &>) {
			return WsHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, WsConn &ws) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, ws);
			}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"WebSocket handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static ErrorHandler make_error_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, exception const &>) {
			return ErrorHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, exception const &>) {
			return ErrorHandler{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, exception const &ex) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, ex);
				}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"Error handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}
};
// Returns a middleware that formats each request as:
//   [ISO8601] METHOD path status bytes elapsed_ms
// and passes the formatted line to `sink`. Thread-safety of `sink` is
// the caller's responsibility.
export Router::Middleware make_access_log_middleware(
	Fn<void(S const &)> sink) {
	return [sink = move(sink)](HttpRequestView const &req, Router::Handler const &next) {
		auto const t0 = chrono::steady_clock::now();
		auto resp = next(req);
		auto const elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

		auto const now = chrono::system_clock::now();
		auto const tt = chrono::system_clock::to_time_t(now);
		A<char, 32> ts_buf{};
		SV ts{};
		if (strftime(ts_buf.data(), ts_buf.size(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt)) > 0) {
			ts = ts_buf.data();
		}

		sink(format("[{}] {} {} {} {} {}ms", ts, req.method, req.path, resp.status, resp.text_body().size(), elapsed));
		return resp;
	};
}
