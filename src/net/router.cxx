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
import conflux.net.path;
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
export namespace conflux::http {

class RequestRingRef {
	void *ptr_{};

public:
	RequestRingRef() noexcept = default;
	template<class Ring>
	explicit RequestRingRef(
		Ring &ring) noexcept
		: ptr_{std::addressof(ring)} {}
	template<class Ring>
	[[nodiscard]] Ring &as() const noexcept {
		return *static_cast<Ring *>(ptr_);
	}
	template<class Ring>
	[[nodiscard]] operator Ring &() const noexcept {
		return as<Ring>();
	}
	[[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

struct RequestContext {
	RequestRingRef ring;
};

} // namespace conflux::http

export using NextHandler = CloneableFunction<Response(RequestView const &)>;
export using MiddlewareFunction = CloneableFunction<Response(RequestView const &, NextHandler const &)>;
export using ContextNextHandler =
	CloneableFunction<conflux::work::root::Task<Response>(RequestView const &, conflux::http::RequestContext const &)>;

export template<class R>
concept HandlerResult = std::same_as<R, Response> || std::same_as<R, conflux::work::root::Task<Response>>;

export template<class F>
concept ViewHandler = requires(std::decay_t<F> &fn, RequestView const &req) {
	{ std::invoke(fn, req) } -> HandlerResult;
};

export template<class F>
concept RequestHandler = requires(std::decay_t<F> &fn, Request const &req) {
	{ std::invoke(fn, req) } -> HandlerResult;
};

export template<class F>
concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

export template<class F>
concept ContextHandlerFunction =
	requires(std::decay_t<F> &fn, RequestView const &req, conflux::http::RequestContext const &ctx) {
		{ std::invoke(fn, req, ctx) } -> std::same_as<conflux::work::root::Task<Response>>;
	};

export template<class F>
concept ContextMiddlewareFunction = requires(
	std::decay_t<F> &fn,
	RequestView const &req,
	conflux::http::RequestContext const &ctx,
	ContextNextHandler const &next) {
	{ std::invoke(fn, req, ctx, next) } -> std::same_as<conflux::work::root::Task<Response>>;
};

export template<class F>
concept AsyncMiddleware = ContextMiddlewareFunction<F>;

export template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, RequestView const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

export template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, Request const &req, NextHandler const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

export template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F> || AsyncMiddleware<F>;

export namespace conflux::http {

enum class HttpMethod : std::uint8_t {
	get,
	post,
	put,
	patch,
	delete_,
	options,
};

[[nodiscard]] constexpr std::string_view http_method_name(
	HttpMethod method) noexcept {
	switch (method) {
	case HttpMethod::get    : return "GET";
	case HttpMethod::post   : return "POST";
	case HttpMethod::put    : return "PUT";
	case HttpMethod::patch  : return "PATCH";
	case HttpMethod::delete_: return "DELETE";
	case HttpMethod::options: return "OPTIONS";
	}
	return "GET";
}

} // namespace conflux::http

struct RouteVerbAccessors {
	template<typename Self, typename F>
	Self &get(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::get, path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &post(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::post, path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &put(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::put, path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &patch(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::patch, path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &del(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::delete_, path, std::forward<F>(handler));
	}
	template<typename Self, typename F>
	Self &options(
		this Self &self,
		std::string_view path,
		F &&handler) {
		return self.add(conflux::http::HttpMethod::options, path, std::forward<F>(handler));
	}
};

export class Router : public RouteVerbAccessors {
public:
	using Handler = NextHandler;
	using ContextHandler = ContextNextHandler;
	using ContextMiddleware = CloneableFunction<conflux::work::root::Task<
		Response>(RequestView const &, conflux::http::RequestContext const &, ContextHandler const &)>;
	using AsyncNext = ContextHandler;
	using SseHandler = CloneableFunction<void(RequestView const &, std::shared_ptr<conflux::http::SseChannel>)>;
	// next is the downstream handler (or next middleware); call it to continue the chain.
	using Middleware = MiddlewareFunction;
	using WsHandler = CloneableFunction<void(RequestView const &, conflux::http::WsConn &)>;
	using ErrorHandler = CloneableFunction<Response(RequestView const &, std::exception const &)>;
	Router();
	explicit Router(conflux::http::Config const &cfg);
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
	Router &add(
		conflux::http::HttpMethod method,
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
	Router &add_context_with_timeout(
		std::string_view method,
		std::string_view path,
		std::shared_ptr<std::chrono::milliseconds> timeout,
		F &&handler) {
		add_context_prepared(method, path, std::move(timeout), ContextHandler{std::forward<F>(handler)});
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &add_context(
		conflux::http::HttpMethod method,
		std::string_view path,
		F &&handler) {
		add_context_prepared(method, path, ContextHandler{std::forward<F>(handler)});
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &add_context_with_timeout(
		conflux::http::HttpMethod method,
		std::string_view path,
		std::shared_ptr<std::chrono::milliseconds> timeout,
		F &&handler) {
		add_context_prepared(method, path, std::move(timeout), ContextHandler{std::forward<F>(handler)});
		return *this;
	}
	template<conflux::http::HttpMethod Method, typename F>
		requires ContextHandlerFunction<F>
	Router &add_method_context(
		std::string_view path,
		F &&handler) {
		return add_context(Method, path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &get_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::get>(path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &post_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::post>(path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &put_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::put>(path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &patch_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::patch>(path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &del_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::delete_>(path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	Router &options_context(
		std::string_view path,
		F &&handler) {
		return add_method_context<conflux::http::HttpMethod::options>(path, std::forward<F>(handler));
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
	[[nodiscard]] bool has_context_routes() const noexcept;
	template<typename F>
	Router &use(
		F &&mw) {
		if constexpr (ContextMiddlewareFunction<F>) {
			use_context_prepared(make_context_middleware(std::forward<F>(mw)));
		} else {
			use_prepared(make_middleware(std::forward<F>(mw)));
		}
		return *this;
	}
	template<typename F>
	Router &use_async(
		F &&mw) {
		use_context_prepared(make_context_middleware(std::forward<F>(mw)));
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
	// Return metadata for all registered route entry points.
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
	Router &set_static_file_cache(conflux::http::StaticFileCacheConfig cfg);
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
			if constexpr (ContextMiddlewareFunction<F>) {
				context_middlewares_.push_back(Router::make_context_middleware(std::forward<F>(mw)));
			} else {
				middlewares_.push_back(Router::make_middleware(std::forward<F>(mw)));
			}
			return *this;
		}
		template<typename F>
		Group &use_async(
			F &&mw) {
			context_middlewares_.push_back(Router::make_context_middleware(std::forward<F>(mw)));
			return *this;
		}
		template<typename F>
		Group &add(
			std::string_view method,
			std::string_view path,
			F &&handler) {
			auto full_path = conflux::http::detail::join_route_path(prefix_, path);
			router_.add(method, full_path, wrap(Router::make_handler(std::forward<F>(handler))));
			return *this;
		}
		template<typename F>
		Group &add(
			conflux::http::HttpMethod method,
			std::string_view path,
			F &&handler) {
			auto full_path = conflux::http::detail::join_route_path(prefix_, path);
			router_.add(method, full_path, wrap(Router::make_handler(std::forward<F>(handler))));
			return *this;
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &add_context(
			std::string_view method,
			std::string_view path,
			F &&handler) {
			auto full_path = conflux::http::detail::join_route_path(prefix_, path);
			router_.add_context(method, full_path, wrap_context(Router::ContextHandler{std::forward<F>(handler)}));
			return *this;
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &add_context(
			conflux::http::HttpMethod method,
			std::string_view path,
			F &&handler) {
			auto full_path = conflux::http::detail::join_route_path(prefix_, path);
			router_.add_context(method, full_path, wrap_context(Router::ContextHandler{std::forward<F>(handler)}));
			return *this;
		}
		template<conflux::http::HttpMethod Method, typename F>
			requires ContextHandlerFunction<F>
		Group &add_method_context(
			std::string_view path,
			F &&handler) {
			return add_context(Method, path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &get_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::get>(path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &post_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::post>(path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &put_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::put>(path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &patch_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::patch>(path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &del_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::delete_>(path, std::forward<F>(handler));
		}
		template<typename F>
			requires ContextHandlerFunction<F>
		Group &options_context(
			std::string_view path,
			F &&handler) {
			return add_method_context<conflux::http::HttpMethod::options>(path, std::forward<F>(handler));
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
				h = [mw = std::move(mw), n = std::move(h)](RequestView const &r) { return mw(r, n); };
			}
			return h;
		}
		[[nodiscard]] ContextHandler wrap_context(ContextHandler h) const;
		Router &router_;
		std::string prefix_;
		std::vector<Middleware> middlewares_;
		std::vector<ContextMiddleware> context_middlewares_;
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
	[[nodiscard]] Response dispatch(Request const &req) const;
	[[nodiscard]] Response dispatch(RequestView const &req) const;
	[[nodiscard]] std::optional<Response>
	dispatch_context(RequestView const &req, conflux::http::RequestContext const &ctx) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	void add_prepared(std::string_view method, std::string_view path, Handler handler);
	void add_prepared(conflux::http::HttpMethod method, std::string_view path, Handler handler);
	void add_context_prepared(std::string_view method, std::string_view path, ContextHandler handler);
	void add_context_prepared(conflux::http::HttpMethod method, std::string_view path, ContextHandler handler);
	void add_context_prepared(
		std::string_view method,
		std::string_view path,
		std::shared_ptr<std::chrono::milliseconds> timeout,
		ContextHandler handler);
	void add_context_prepared(
		conflux::http::HttpMethod method,
		std::string_view path,
		std::shared_ptr<std::chrono::milliseconds> timeout,
		ContextHandler handler);
	void use_prepared(Middleware mw);
	void use_context_prepared(ContextMiddleware mw);
	void set_not_found_handler(Handler handler);
	void set_error_handler(ErrorHandler handler);
	void sse_prepared(std::string_view path, SseHandler handler);
	Router &ws_prepared(std::string_view path, WsHandler handler);
	static void launch_sse_handler(
		std::shared_ptr<WorkPool> const &pool,
		SseHandler handler,
		Request matched,
		std::shared_ptr<conflux::http::SseChannel> const &channel);
	[[nodiscard]] static Response defer_http_task(conflux::work::root::Task<Response> task);
	[[nodiscard]] Response run_middlewares(RequestView const &req, Handler const &inner) const;
	[[nodiscard]] std::optional<Response> run_context_middlewares(
		RequestView const &req,
		conflux::http::RequestContext const &ctx,
		ContextHandler const &inner) const;
	[[nodiscard]] static Response run_async_http_task(conflux::work::root::Task<Response> task);
	template<class>
	static constexpr bool kDependentFalse = false;
	template<typename F>
	static Handler make_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, RequestView const &>) {
			using Ret = std::invoke_result_t<Fn &, RequestView const &>;
			if constexpr (std::same_as<Ret, Response>) {
				return Handler{std::forward<F>(fn)};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<Response>>) {
				return Handler{[wrapped = Fn(std::forward<F>(fn))](RequestView const &req) mutable -> Response {
					auto invoke_view = [](Fn &handler, RequestView view) -> conflux::work::root::Task<Response> {
						co_return co_await std::invoke(handler, view);
					};
					return defer_http_task(invoke_view(wrapped, RequestView{req}));
				}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler taking RequestView const& must return Response or root::Task<Response>");
			}
		} else if constexpr (std::invocable<Fn &, Request const &>) {
			using Ret = std::invoke_result_t<Fn &, Request const &>;
			if constexpr (std::same_as<Ret, Response>) {
				return Handler{[wrapped = Fn(std::forward<F>(fn))](RequestView const &req) mutable -> Response {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned);
				}};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<Response>>) {
				return Handler{[wrapped = Fn(std::forward<F>(fn))](RequestView const &req) mutable -> Response {
					auto owned = req.to_owned();
					auto invoke_owned = [](Fn &handler, Request owned_req) -> conflux::work::root::Task<Response> {
						co_return co_await std::invoke(handler, owned_req);
					};
					return defer_http_task(invoke_owned(wrapped, std::move(owned)));
				}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler returning Request const& must return Response or root::Task<Response>");
			}
		} else {
			static_assert(kDependentFalse<Fn>, "Handler must accept RequestView const& or Request const&");
		}
	}
	template<typename F>
	static Middleware make_middleware(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (requires(
						  Fn &middleware,
						  RequestView const &req,
						  conflux::http::RequestContext const &ctx,
						  ContextHandler const &next) {
						  {
							  std::invoke(middleware, req, ctx, next)
						  } -> std::same_as<conflux::work::root::Task<Response>>;
					  }) {
			static_assert(
				kDependentFalse<Fn>,
				"Async middleware must be registered with Router::use or Router::use_async so it can run through "
				"the context dispatch path");
		} else if constexpr (requires(
								 Fn &middleware,
								 Request const &req,
								 conflux::http::RequestContext const &ctx,
								 ContextHandler const &next) {
								 {
									 std::invoke(middleware, req, ctx, next)
								 } -> std::same_as<conflux::work::root::Task<Response>>;
							 }) {
			static_assert(
				kDependentFalse<Fn>,
				"Async middleware must be registered with Router::use or Router::use_async so it can run through "
				"the context dispatch path");
		} else if constexpr (std::invocable<Fn &, RequestView const &, Handler const &>) {
			return Middleware{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, Request const &, Handler const &>) {
			return Middleware{
				[wrapped = Fn(std::forward<F>(fn))](RequestView const &req, Handler const &next) mutable -> Response {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned, next);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Middleware must accept RequestView const& or Request const&");
		}
	}
	template<typename F>
	static ContextMiddleware make_context_middleware(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (requires(
						  Fn &middleware,
						  RequestView const &req,
						  conflux::http::RequestContext const &ctx,
						  ContextHandler const &next) {
						  {
							  std::invoke(middleware, req, ctx, next)
						  } -> std::same_as<conflux::work::root::Task<Response>>;
					  }) {
			return ContextMiddleware{
				[wrapped = Fn(std::forward<F>(fn))](
					RequestView const &req,
					conflux::http::RequestContext const &ctx,
					ContextHandler const &next) mutable -> conflux::work::root::Task<Response> {
					auto invoke_view = [](Fn &middleware,
										  RequestView view,
										  conflux::http::RequestContext request_ctx,
										  ContextHandler downstream) -> conflux::work::root::Task<Response> {
						co_return co_await std::invoke(middleware, view, request_ctx, downstream);
					};
					return invoke_view(wrapped, RequestView{req}, ctx, next);
				}};
		} else if constexpr (requires(
								 Fn &middleware,
								 Request const &req,
								 conflux::http::RequestContext const &ctx,
								 ContextHandler const &next) {
								 {
									 std::invoke(middleware, req, ctx, next)
								 } -> std::same_as<conflux::work::root::Task<Response>>;
							 }) {
			return ContextMiddleware{
				[wrapped = Fn(std::forward<F>(fn))](
					RequestView const &req,
					conflux::http::RequestContext const &ctx,
					ContextHandler const &next) mutable -> conflux::work::root::Task<Response> {
					auto owned = req.to_owned();
					co_return co_await std::invoke(wrapped, owned, ctx, next);
				}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"Async middleware must accept RequestView const&, RequestContext const&, and Router::AsyncNext const&");
		}
	}
	template<typename F>
	static SseHandler make_sse_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, RequestView const &, std::shared_ptr<conflux::http::SseChannel>>) {
			return SseHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, Request const &, std::shared_ptr<conflux::http::SseChannel>>) {
			return SseHandler{[wrapped = Fn(std::forward<F>(fn))](
								  RequestView const &req,
								  std::shared_ptr<conflux::http::SseChannel> ch) mutable {
				auto owned = req.to_owned();
				std::invoke(wrapped, owned, std::move(ch));
			}};
		} else {
			static_assert(kDependentFalse<Fn>, "SSE handler must accept RequestView const& or Request const&");
		}
	}
	template<typename F>
	static WsHandler make_ws_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, RequestView const &, conflux::http::WsConn &>) {
			return WsHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, Request const &, conflux::http::WsConn &>) {
			return WsHandler{
				[wrapped = Fn(std::forward<F>(fn))](RequestView const &req, conflux::http::WsConn &ws) mutable {
					auto owned = req.to_owned();
					std::invoke(wrapped, owned, ws);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "WebSocket handler must accept RequestView const& or Request const&");
		}
	}
	template<typename F>
	static ErrorHandler make_error_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, RequestView const &, std::exception const &>) {
			return ErrorHandler{std::forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, Request const &, std::exception const &>) {
			return ErrorHandler{
				[wrapped =
					 Fn(std::forward<F>(fn))](RequestView const &req, std::exception const &ex) mutable -> Response {
					auto owned = req.to_owned();
					return std::invoke(wrapped, owned, ex);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Error handler must accept RequestView const& or Request const&");
		}
	}
};
// Returns a middleware that formats each request as:
//   [ISO8601] METHOD path status bytes elapsed_ms
// and passes the formatted line to `sink`. Thread-safety of `sink` is
// the caller's responsibility.
export Router::Middleware make_access_log_middleware(
	std::function<void(std::string const &)> sink) {
	return [sink = std::move(sink)](RequestView const &req, Router::Handler const &next) {
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
