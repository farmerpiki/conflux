export module conflux.net.async_client;

import conflux.work;
import conflux.work.uring_executor;
import conflux.socket_io;
import conflux.net.client;
import conflux.net.http.request;

export namespace conflux::http {

using conflux::socket_io::SocketTaskRing;

struct AsyncClientRunOptions {
	unsigned ring_entries = 256;
};

[[nodiscard]] conflux::work::root::Task<ClientResult>
async_send(HttpClient const &client, SocketTaskRing &ring, ClientRequest const &req);

[[nodiscard]] conflux::work::root::Task<ClientResult>
async_send(HttpClient const &client, conflux::work::UringExecutor &executor, ClientRequest const &req);

[[nodiscard]] ClientResult async_blocking_send(
	HttpClient const &client,
	ClientRequest const &request,
	AsyncClientRunOptions opts = {});

[[nodiscard]] ClientResult async_blocking_send(
	HttpClient const &client,
	ClientRequest &&request,
	AsyncClientRunOptions opts = {});

} // namespace conflux::http
