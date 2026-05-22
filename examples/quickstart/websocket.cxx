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

	return static_cast<int>(std::move(app).run({.port = 9096}));
}
