export module conflux.net.http.static_async;

import conflux.types;
import std;
import conflux.work;
import conflux.file_io;
import conflux.net.http.server_types;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.response;

export namespace conflux::http {

conflux::work::root::Task<void> do_serve_static_file(
	std::shared_ptr<DeferredResponse> dr,
	Response base,
	std::size_t send_off,
	std::size_t send_sz,
	std::size_t total_size,
	conflux::work::root::Task<conflux::uring::FileHandle> open_task);

Response handle_static_get(
	std::string const &rd,
	int root_fd,
	StaticOptions const &static_options,
	detail::StaticRequest const &r,
	detail::StaticCacheStore &static_cache);

Response handle_static_get_request(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	RequestView const &req,
	detail::StaticCacheStore &static_cache);

Response handle_static_put(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	RequestView const &req,
	detail::StaticCacheStore &static_cache);

Response handle_static_delete(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	RequestView const &req,
	detail::StaticCacheStore &static_cache);

} // namespace conflux::http
