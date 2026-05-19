module;
#include <ctime>
export module conflux.net.router;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router_match;
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

// Metadata for a single registered route, exposed by Router::route_infos().
export struct RouteInfo {
	std::string method;
	std::string path_pattern; // OpenAPI-style path e.g. /users/{id}
	std::vector<std::string> path_params; // captured parameter names in order
};
export struct RequestContext {
	SocketTaskRing &ring;
};

export using NextHandler = CloneableFunction<HttpResponse(HttpRequestView const &)>;
export using MiddlewareFunction = CloneableFunction<HttpResponse(HttpRequestView const &, NextHandler const &)>;

export template<class R>
concept HandlerResult = std::same_as<R, HttpResponse> || std::same_as<R, conflux::work::root::Task<HttpResponse>>;

export template<class F>
concept ViewHandler = requires(std::decay_t<F> &fn, HttpRequestView const &req) {
	{ std::invoke(fn, req) } -> std::same_as<HttpResponse>;
};

export template<class F>
concept RequestHandler = requires(std::decay_t<F> &fn, HttpRequest const &req) {
	{ std::invoke(fn, req) } -> HandlerResult;
};

export template<class F>
concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

export template<class F>
concept ContextHandlerFunction = requires(std::decay_t<F> &fn, HttpRequest const &req, RequestContext const &ctx) {
	{ std::invoke(fn, req, ctx) } -> std::same_as<conflux::work::root::Task<HttpResponse>>;
};

export template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, HttpRequestView const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<HttpResponse>;
};

export template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, HttpRequest const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<HttpResponse>;
};

export template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F>;

struct RouteVerbAccessors {
	template<typename Self, typename F>
	Self &get(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("GET", path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &post(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("POST", path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &put(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("PUT", path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &patch(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("PATCH", path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &del(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("DELETE", path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &options(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add("OPTIONS", path, std::forward<F>(handler));
	}
};

export class Router : public RouteVerbAccessors {
public:
	using Handler = NextHandler;
	using ContextHandler =
		CloneableFunction<conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &)>;
	using ContextMiddleware = CloneableFunction<
		conflux::work::root::Task<HttpResponse>(HttpRequest const &, RequestContext const &, ContextHandler const &)>;
	using SseHandler = CloneableFunction<void(HttpRequestView const &, std::shared_ptr<SseChannel>)>;
	// next is the downstream handler (or next middleware); call it to continue the chain.
	using Middleware = MiddlewareFunction;
	using WsHandler = CloneableFunction<void(HttpRequestView const &, WsConn &)>;
	using ErrorHandler = CloneableFunction<HttpResponse(HttpRequestView const &, std::exception const &)>;
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
		std::string_view method,
		std::string_view path,
		F &&handler) {
		add_prepared(method, path, make_handler(std::forward<F>(handler)));
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &add_context(
		std::string_view method,
		std::string_view path,
		F &&handler) {
		add_context_prepared(method, path, ContextHandler{std::forward<F>(handler)});
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &get_context(
		std::string_view path,
		F &&handler) {
		return add_context("GET", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &post_context(
		std::string_view path,
		F &&handler) {
		return add_context("POST", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &put_context(
		std::string_view path,
		F &&handler) {
		return add_context("PUT", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &patch_context(
		std::string_view path,
		F &&handler) {
		return add_context("PATCH", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &del_context(
		std::string_view path,
		F &&handler) {
		return add_context("DELETE", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &options_context(
		std::string_view path,
		F &&handler) {
		return add_context("OPTIONS", path, std::forward<F>(handler));
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
	[[nodiscard]] bool has_context_routes() const noexcept;
	template<typename F>
	Router &use(
		F &&mw) {
		use_prepared(make_middleware(std::forward<F>(mw)));
		return *this;
	}
	template<typename F>
	Router &on_not_found(
		F &&handler) {
		set_not_found_handler(make_handler(std::forward<F>(handler)));
		return *this;
	}
	template<typename F>
	Router &on_error(
		F &&handler) {
		set_error_handler(make_error_handler(std::forward<F>(handler)));
		return *this;
	}
	// Return metadata for all registered routes (regular routes only).
	[[nodiscard]] std::vector<RouteInfo> route_infos() const;
	template<typename F>
	Router &sse(
		std::string_view path,
		F &&handler) {
		sse_prepared(path, make_sse_handler(std::forward<F>(handler)));
		return *this;
	}
	Router &set_work_pool(std::shared_ptr<WorkPool> pool);
	[[nodiscard]] std::shared_ptr<WorkPool> work_pool() const;
	Router &set_static_file_cache(StaticFileCacheConfig cfg);
	// Register a WebSocket upgrade handler. GET requests with a valid Upgrade: websocket
	// handshake are upgraded to WebSocket; the handler runs on the router's work pool.
	template<typename F>
	Router &ws(
		std::string_view path,
		F &&handler) {
		return ws_prepared(path, make_ws_handler(std::forward<F>(handler)));
	}
	// Route group: scopes a set of routes under a path prefix with Opt group-local middleware.
	// Group middleware wraps only the routes registered inside the group callback;
	// it does NOT affect routes registered outside. The group callback receives a Group&.
	class Group : public RouteVerbAccessors {
	public:
		template<typename F>
		Group &use(
			F &&mw) {
			middlewares_.push_back(Router::make_middleware(std::forward<F>(mw)));
			return *this;
		}
		template<typename F>
		Group &add(
			std::string_view method,
			std::string_view path,
			F &&handler) {
			std::string full_path;
			full_path.reserve(prefix_.size() + path.size());
			full_path += prefix_;
			full_path.append(path.data(), path.size());
			router_.add(method, full_path, wrap(Router::make_handler(std::forward<F>(handler))));
			return *this;
		}

	private:
		friend class Router;
		Group(
			Router &router,
			std::string prefix)
			: router_(router)
			, prefix_(std::move(prefix))
			, middlewares_{} {}
		// Apply group middlewares around h (innermost first, so first-registered is outermost).
		// Capture mw by value: the Group object is destroyed after router.group() returns,
		// so capturing by reference would dangle.
		[[nodiscard]] Handler wrap(
			Handler h) const {
			for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
				auto mw = middlewares_[static_cast<std::size_t>(i)]; // copy: Group is destroyed after group() returns
				h = [mw = std::move(mw), n = std::move(h)](HttpRequestView const &r) { return mw(r, n); };
			}
			return h;
		}
		Router &router_;
		std::string prefix_;
		std::vector<Middleware> middlewares_;
	};
	template<typename F>
	Router &group(
		std::string_view prefix,
		F &&fn) {
		Group g{*this, std::string{prefix}};
		std::forward<F>(fn)(g);
		return *this;
	}
	// Serve static files from root_dir for GET/HEAD requests under url_prefix.
	// url_prefix must not end with '/'. Files are served at url_prefix/{*file}.
	// Path traversal ("..") is rejected with 403.
	// ETag based on size+mtime; Range requests (206 Partial Content) supported.
	// Pre-compressed sidecar files (.gz, .br) served when client accepts them.
	Router &serve_static(std::string_view url_prefix, std::string root_dir, StaticOptions const &sopts = {});
	[[nodiscard]] HttpResponse dispatch(HttpRequest const &req) const;
	[[nodiscard]] HttpResponse dispatch(HttpRequestView const &req) const;
	[[nodiscard]] std::optional<HttpResponse> dispatch_context(HttpRequest const &req, RequestContext const &ctx) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	void add_prepared(std::string_view method, std::string_view path, Handler handler);
	void add_context_prepared(std::string_view method, std::string_view path, ContextHandler handler);
	void use_prepared(Middleware mw);
	void set_not_found_handler(Handler handler);
	void set_error_handler(ErrorHandler handler);
	void sse_prepared(std::string_view path, SseHandler handler);
	Router &ws_prepared(std::string_view path, WsHandler handler);
	static void launch_sse_handler(
		std::shared_ptr<WorkPool> const &pool,
		SseHandler handler,
		HttpRequest matched,
		std::shared_ptr<SseChannel> const &channel);
	[[nodiscard]] static HttpResponse defer_http_task(conflux::work::root::Task<HttpResponse> task);
	[[nodiscard]] HttpResponse run_middlewares(HttpRequestView const &req, Handler const &inner) const;
	[[nodiscard]] static HttpResponse run_async_http_task(conflux::work::root::Task<HttpResponse> task);
	template<class>
	static constexpr bool kDependentFalse = false;
	template<typename F>
	static Handler make_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequestView const &>;
			if constexpr (std::same_as<Ret, HttpResponse>) {
				return Handler{std::forward<F>(fn)};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
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
			if constexpr (std::same_as<Ret, HttpResponse>) {
				return Handler{[wrapped = Fn(std::forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned);
				}};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				return Handler{[wrapped = Fn(std::forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return defer_http_task(std::invoke(wrapped, owned));
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
		if constexpr (requires(Fn &middleware, HttpRequestView const &req, Handler const &next) {
						  {
							  std::invoke(middleware, req, next)
						  } -> std::same_as<conflux::work::root::Task<HttpResponse>>;
					  }) {
			static_assert(
				kDependentFalse<Fn>,
				"Async middleware is not normalized yet; middleware must return HttpResponse. Use a sync "
				"middleware, or offload/defer inside a route handler.");
		} else if constexpr (requires(Fn &middleware, HttpRequest const &req, Handler const &next) {
								 {
									 std::invoke(middleware, req, next)
								 } -> std::same_as<conflux::work::root::Task<HttpResponse>>;
							 }) {
			static_assert(
				kDependentFalse<Fn>,
				"Async middleware is not normalized yet; middleware must return HttpResponse. Use a sync "
				"middleware, or offload/defer inside a route handler.");
		} else if constexpr (std::invocable<Fn &, HttpRequestView const &, Handler const &>) {
			return Middleware{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, Handler const &>) {
			return Middleware{
				[wrapped =
					 Fn(std::forward<F>(fn))](HttpRequestView const &req, Handler const &next) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned, next);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Middleware must accept HttpRequestView const& or HttpRequest const&");
		}
	}
	template<typename F>
	static SseHandler make_sse_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, std::shared_ptr<SseChannel>>) {
			return SseHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, std::shared_ptr<SseChannel>>) {
			return SseHandler{
				[wrapped =
					 Fn(std::forward<F>(fn))](HttpRequestView const &req, std::shared_ptr<SseChannel> ch) mutable {
					auto owned = req.to_owned();
					std::invoke(wrapped, owned, std::move(ch));
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
			return WsHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, WsConn &>) {
			return WsHandler{[wrapped = Fn(std::forward<F>(fn))](HttpRequestView const &req, WsConn &ws) mutable {
				auto owned = req.to_owned();
				std::invoke(wrapped, owned, ws);
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
		if constexpr (std::invocable<Fn &, HttpRequestView const &, std::exception const &>) {
			return ErrorHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, std::exception const &>) {
			return ErrorHandler{
				[wrapped = Fn(std::forward<F>(fn))](HttpRequestView const &req, std::exception const &ex) mutable
					-> HttpResponse {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned, ex);
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
	std::function<void(std::string const &)> sink) {
	return [sink = std::move(sink)](HttpRequestView const &req, Router::Handler const &next) {
		auto const t0 = std::chrono::steady_clock::now();
		auto resp = next(req);
		auto const elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

		auto const now = std::chrono::system_clock::now();
		auto const tt = std::chrono::system_clock::to_time_t(now);
		std::array<char, 32> ts_buf{};
		std::string_view ts{};
		if (strftime(ts_buf.data(), ts_buf.size(), "%Y-%m-%dT%H:%M:%S", gmtime(&tt)) > 0) {
			ts = ts_buf.data();
		}

		sink(
			std::format(
				"[{}] {} {} {} {} {}ms",
				ts,
				req.method,
				req.path,
				resp.status,
				resp.text_body().size(),
				elapsed));
		return resp;
	};
}
