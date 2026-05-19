import conflux.http;
import std;

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	app.get("/", [] { return http::text("connect to /ws\n"); });
	app.ws("/ws", [](http::RequestView const &, http::WsConn &ws) {
		while (auto frame = ws.recv()) {
			if (frame->opcode == http::WsConn::Opcode::Text) {
				if (!ws.send_text(frame->payload)) {
					break;
				}
			}
		}
	});

	return http::run(std::move(app), {.port = 9096}) == http::RunStatus::stopped_normally ? 0 : 1;
}
