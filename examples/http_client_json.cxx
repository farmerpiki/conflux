// HTTP client + typed JSON example: request builder body encoding and response decoding
// without performing network I/O.
import conflux.types;
import conflux.json;
import conflux.net.http.native_json;
import std;

namespace http = conflux::http;
namespace json = conflux::json;

struct CreateItem {
	S name;
	i64 quantity{};
};

template<>
struct JsonMembers<CreateItem> {
	static constexpr auto members() {
		return Tup{
			json_member("name", &CreateItem::name),
			json_member("quantity", &CreateItem::quantity),
		};
	}
	static constexpr SV type_name() { return "CreateItem"; }
};

struct Item {
	i64 id{};
	S name;
	i64 quantity{};
	bool accepted{};
};

template<>
struct JsonMembers<Item> {
	static constexpr auto members() {
		return Tup{
			json_member("id", &Item::id),
			json_member("name", &Item::name),
			json_member("quantity", &Item::quantity),
			json_member("accepted", &Item::accepted),
		};
	}
	static constexpr SV type_name() { return "Item"; }
};

static void print_bad_url() {
	auto bad = http::try_post_client_request("ftp://example.test/items");
	if (!bad) {
		std::println("bad url rejected: {}", bad.error().message);
	}
}

int main() {
	print_bad_url();

	auto builder = http::try_post_client_request("https://api.example.test/v1/items");
	if (!builder) {
		std::println("url parse failed: {}", builder.error().message);
		return 1;
	}

	CreateItem payload{.name = "buffer slab", .quantity = 8};
	http::json::set_body(*builder, payload);

	auto req = move(*builder)
		.query("trace", "local-json-demo")
		.accept_json()
		.user_agent("conflux-json-client-example/1")
		.bearer("example-token")
		.build();

	std::println("{} {}", req.method(), req.url().str());
	std::println("content-type: {}", req.headers()["content-type"]);
	std::println("accept: {}", req.headers()["accept"]);
	std::println("encoded body: {}", req.body());

	auto decoded_request = http::json::decode_body<CreateItem>(req);
	if (decoded_request) {
		std::println("decoded request: {} x{}", decoded_request->name, decoded_request->quantity);
	}

	auto decoded_response = json::boundary::decode_native<Item>(
		R"({"id":42,"name":"buffer slab","quantity":8,"accepted":true})");
	if (!decoded_response) {
		std::println("response decode failed: {}", decoded_response.error().message);
		return 1;
	}

	std::println(
		"decoded response: id={} name={} accepted={}",
		decoded_response->id,
		decoded_response->name,
		decoded_response->accepted);
}
