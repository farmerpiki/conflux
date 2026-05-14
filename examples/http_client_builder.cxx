// HTTP client construction example: fallible URL parsing, query encoding,
// case-insensitive fields, request-builder ergonomics, timeouts, and chunked
// body decoding without performing network I/O.
import conflux.net.http;
import conflux.net.http_server_helpers;
import conflux.types;
import std;

namespace http = conflux::http;

static void print_url_parse(SV raw) {
	auto parsed = http::Url::parse(raw);
	if (!parsed) {
		println("url parse failed for '{}': {}", raw, parsed.error().message);
		return;
	}
	println("url parsed: scheme={} host={} port={} path={} query={}", parsed->scheme, parsed->host, parsed->port, parsed->path, parsed->query);
}

int main() {
	print_url_parse("https://example.com/api/search");
	print_url_parse("ftp://example.com/nope");

	auto url = *http::Url::parse("https://api.example.test/v1/items");
	url.set_query_param("q", "fast json & http");
	url.set_query_param("limit", "25");

	HttpFields defaults{true};
	defaults.set("User-Agent", "conflux-example/1");
	defaults.append("Accept", "application/json");
	defaults.append("Accept", "text/plain");
	println("accept values: {}", defaults.values("accept").size());

	HttpFields form;
	form.set("name", "Ada Lovelace");
	form.set("role", "admin/operator");

	auto req = http::HttpRequest::post(url.str())
		.headers(defaults)
		.query("trace", "local demo")
		.bearer("example-token")
		.if_none_match(R"("cached-etag")")
		.timeouts({
			.resolve = chrono::milliseconds{750},
			.connect = chrono::milliseconds{750},
			.tls = chrono::milliseconds{1000},
			.write = chrono::milliseconds{1500},
			.first_byte = chrono::milliseconds{2000},
			.between_bytes = chrono::milliseconds{2000},
		})
		.follow_redirects(3)
		.body_form(form)
		.build();

	println("{} {}", req.method(), req.url().str());
	println("content-type: {}", req.headers()["content-type"]);
	println("authorization: {}", req.headers()["authorization"]);
	println("body: {}", req.body());
	println("redirect limit: {} verify_peer={}", req.max_redirects(), req.verify_peer());

	S body;
	auto consumed = decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", 64, 8, body);
	println("chunked decode: {}", consumed > 0 ? body : "<invalid>");
}
