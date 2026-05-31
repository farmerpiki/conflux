export module conflux.net.http.static_async;

import conflux.types;
import std;
import conflux.work;
import conflux.file_io;
import conflux.net.http.server_types;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.response;

export conflux::work::root::Task<void> do_serve_static_file(
	std::shared_ptr<conflux::http::DeferredResponse> dr,
	conflux::http::Response base,
	std::size_t send_off,
	std::size_t send_sz,
	std::size_t total_size,
	conflux::work::root::Task<FileHandle> open_task);

export conflux::http::Response handle_static_get(
	std::string const &rd,
	int root_fd,
	StaticOptions const &static_options,
	StaticRequest const &r,
	StaticCacheStore &static_cache);

export conflux::http::Response handle_static_get_request(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	StaticCacheStore &static_cache);

export conflux::http::Response handle_static_put(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	StaticCacheStore &static_cache);

export conflux::http::Response handle_static_delete(
	std::string const &rd,
	int root_fd,
	StaticOptions const &sopts,
	conflux::http::RequestView const &req,
	StaticCacheStore &static_cache);
