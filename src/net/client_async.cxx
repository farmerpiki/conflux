export module conflux.net.async_client;

import conflux.work;
import conflux.socket_io;
import conflux.net.client;
import conflux.net.http.request;

export namespace conflux::http {

[[nodiscard]] conflux::work::root::Task<HttpResult> async_send(
	HttpClient const &client,
	SocketTaskRing &ring,
	HttpRequest const &req);

[[nodiscard]] conflux::work::root::Task<HttpResult> send_async(
	HttpClient const &client,
	SocketTaskRing &ring,
	HttpRequest const &req);

} // namespace conflux::http
