import conflux;
import std;

namespace {

struct Args {
	std::string mode = "websocket";
	std::uint16_t port = 0;
	std::string cert;
	std::string key;
};

[[nodiscard]] std::uint16_t parse_port(
	std::string_view value) {
	auto parsed = std::uint32_t{};
	auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
	if (ec != std::errc{} || ptr != value.data() + value.size() || parsed > 65535U) {
		throw std::runtime_error{"invalid --port"};
	}
	return static_cast<std::uint16_t>(parsed);
}

[[nodiscard]] Args parse_args(
	int argc,
	char **argv) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		std::string_view const arg{argv[i]};
		auto require_value = [&](std::string_view name) -> std::string_view {
			if (++i >= argc) {
				throw std::runtime_error{std::format("{} requires a value", name)};
			}
			return argv[i];
		};
		if (arg == "--mode") {
			args.mode = std::string{require_value(arg)};
		} else if (arg == "--port") {
			args.port = parse_port(require_value(arg));
		} else if (arg == "--cert") {
			args.cert = std::string{require_value(arg)};
		} else if (arg == "--key") {
			args.key = std::string{require_value(arg)};
		} else {
			throw std::runtime_error{std::format("unknown argument: {}", arg)};
		}
	}
	if (args.port == 0) {
		throw std::runtime_error{"--port is required"};
	}
	if (args.mode != "h2" && args.mode != "websocket") {
		throw std::runtime_error{"--mode must be h2 or websocket"};
	}
	if (args.mode == "h2" && (args.cert.empty() || args.key.empty())) {
		throw std::runtime_error{"h2 mode requires --cert and --key"};
	}
	return args;
}

} // namespace

int main(
	int argc,
	char **argv) try {
	namespace http = conflux::http;

	auto const args = parse_args(argc, argv);
	http::Config cfg = http::Config::test();
	cfg.port = args.port;
	cfg.startup_banner = false;
	if (args.mode == "h2") {
		cfg.cert_file = args.cert;
		cfg.key_file = args.key;
	}

	auto app = http::app(std::move(cfg));
	app.get("/", [] { return http::text("ok\n"); });
	app.get("/health", [] { return http::text("ok\n"); });
	app.ws("/ws", [](http::RequestView const &, http::WsConn &ws) {
		while (auto frame = ws.recv()) {
			if (frame->opcode == http::WsConn::Opcode::Text) {
				if (!ws.send_text(frame->payload)) {
					break;
				}
			} else if (frame->opcode == http::WsConn::Opcode::Binary) {
				if (!ws.send_binary(std::as_bytes(std::span{frame->payload}))) {
					break;
				}
			}
		}
	});

	return static_cast<int>(std::move(app).run({.port = args.port}));
} catch (std::exception const &e) {
	std::println("third-party conformance server: {}", e.what());
	return 2;
}
