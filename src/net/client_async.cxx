export module conflux.net.async_client;

import conflux.work;
import conflux.socket_io;
import conflux.net.client;
import conflux.net.http.request;

export namespace conflux::http {

[[nodiscard]] conflux::work::root::Task<ClientResult> async_send(
	HttpClient const &client,
	SocketTaskRing &ring,
	ClientRequest const &req);

[[nodiscard]] conflux::work::root::Task<ClientResult> send_async(
	HttpClient const &client,
	SocketTaskRing &ring,
	ClientRequest const &req);

} // namespace conflux::http
