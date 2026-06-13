module;
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>

export module conflux.http.extended;

export import conflux.http;
import conflux.work;
import std;
import conflux.net.app.defer;
import conflux.file_io_sync;
import conflux.types;
import conflux.net.http.response;

export namespace conflux::http {

using conflux::errnum;
using WorkPool = conflux::work::WorkPool;
using WorkPoolOptions = conflux::work::WorkPoolOptions;
using WorkPoolQueueMode = conflux::work::WorkPoolQueueMode;
using WorkPoolQueueStats = conflux::work::WorkPoolQueueStats;
using Router = std::remove_reference_t<decltype(router(std::declval<App &>()))>;
using conflux::http::defer;
template<class T>
using Task = conflux::work::Task<T>;
using Next = Router::Handler;
using AsyncNext = Router::AsyncNext;
inline constexpr std::size_t kBlockingFileResponseMaxBytes = std::size_t{16} * 1024 * 1024;

#if !defined(CONFLUX_INTERFACE_HEADER)
template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, conflux::http::RequestView const &req, Next const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, conflux::http::OwnedRequest const &req, Next const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept AsyncMiddleware = requires(
	std::decay_t<F> &fn,
	conflux::http::RequestView const &req,
	RequestContext const &ctx,
	AsyncNext const &next) {
	{ std::invoke(fn, req, ctx, next) } -> std::same_as<conflux::work::root::Task<Response>>;
};

template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F> || AsyncMiddleware<F>;
#endif

[[nodiscard]] Response blocking_file_response(
	std::filesystem::path const &path,
	std::string content_type = "application/octet-stream",
	std::size_t max_bytes = kBlockingFileResponseMaxBytes) {
	auto path_string = path.string();
	auto raw_fd = ::open(path_string.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (raw_fd < 0) {
		auto const err = errno;
		if (err == ENOENT || err == ENOTDIR) {
			return Response::not_found("file not found");
		}
		if (err == ELOOP || err == EINVAL) {
			return Response::forbidden("file is outside the allowed path");
		}
		return Response::internal_error("failed to read file");
	}
	auto file = conflux::file_io_sync::UniqueFd{raw_fd};
	auto stat = conflux::file_io_sync::blocking_fstat(file.fd());
	if (!stat) {
		return Response::internal_error("failed to read file");
	}
	if (!S_ISREG(stat->mode)) {
		return Response::forbidden("file is not a regular file");
	}
	auto body = conflux::file_io_sync::blocking_read_all_fd(file.fd(), max_bytes);
	if (!body) {
		auto const err = errnum(body);
		if (err == EFBIG) {
			return Response::content_too_large();
		}
		return Response::internal_error("failed to read file");
	}
	return Response::with_body(std::move(*body), std::move(content_type));
}

[[nodiscard]] Response blocking_file_response(
	std::filesystem::path const &root,
	std::filesystem::path const &relative_path,
	std::string content_type = "application/octet-stream",
	std::size_t max_bytes = kBlockingFileResponseMaxBytes) {
	auto root_string = root.string();
	auto root_fd = conflux::file_io_sync::UniqueFd{::open(root_string.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
	if (!root_fd) {
		auto const err = errno;
		if (err == ENOENT || err == ENOTDIR) {
			return Response::not_found("file not found");
		}
		return Response::internal_error("failed to read file");
	}
	auto relative = relative_path.generic_string();
	auto file = conflux::file_io_sync::blocking_openat_contained(root_fd.fd(), relative, O_RDONLY | O_NONBLOCK);
	if (!file) {
		auto const err = errnum(file);
		if (err == ENOENT || err == ENOTDIR) {
			return Response::not_found("file not found");
		}
		if (err == ELOOP || err == EXDEV || err == EINVAL) {
			return Response::forbidden("file is outside the allowed root");
		}
		return Response::internal_error("failed to read file");
	}
	auto stat = conflux::file_io_sync::blocking_fstat(file->fd());
	if (!stat) {
		return Response::internal_error("failed to read file");
	}
	if (!S_ISREG(stat->mode)) {
		return Response::forbidden("file is not a regular file");
	}
	auto body = conflux::file_io_sync::blocking_read_all_fd(file->fd(), max_bytes);
	if (!body) {
		auto const err = errnum(body);
		if (err == EFBIG) {
			return Response::content_too_large();
		}
		return Response::internal_error("failed to read file");
	}
	return Response::with_body(std::move(*body), std::move(content_type));
}

[[nodiscard]] Next openapi_handler(
	App const &app,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto spec = app.openapi_spec(title, version);
	return [spec = std::move(spec)](conflux::http::RequestView const &) -> Response { return Response::json(spec); };
}

template<typename F>
App &use_async(
	App &app,
	F &&middleware) {
	return app.use(std::forward<F>(middleware));
}

template<typename F>
	requires(std::invocable<F &> && IntoResponse<std::invoke_result_t<F &>>)
[[nodiscard]] Response offload(
	std::shared_ptr<WorkPool> const &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

template<typename F>
	requires(std::invocable<F &> && IntoResponse<std::invoke_result_t<F &>>)
[[nodiscard]] Response offload(
	WorkPool &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

} // namespace conflux::http
