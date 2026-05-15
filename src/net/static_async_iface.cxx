export module conflux.net.http.static_async;

import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.net.http.server_types;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.response;

export conflux::work::root::Task<void> do_serve_static_file(
	SP<DeferredResponse> dr,
	HttpResponse base,
	SZ send_off,
	SZ send_sz,
	SZ total_size,
	conflux::work::root::Task<FileHandle> open_task);

export HttpResponse handle_static_get(
	S const &rd,
	int root_fd,
	StaticOptions const &static_options,
	StaticRequest const &r,
	StaticCacheStore &static_cache);

export HttpResponse handle_static_get_request(
	S const &rd,
	int root_fd,
	StaticOptions const &sopts,
	HttpRequestView const &req,
	StaticCacheStore &static_cache);

export HttpResponse handle_static_put(
	S const &rd,
	int root_fd,
	StaticOptions const &sopts,
	HttpRequestView const &req,
	StaticCacheStore &static_cache);

export HttpResponse handle_static_delete(
	S const &rd,
	int root_fd,
	StaticOptions const &sopts,
	HttpRequestView const &req,
	StaticCacheStore &static_cache);
