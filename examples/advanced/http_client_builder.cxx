// HTTP client construction example: fallible URL parsing, query encoding,
// case-insensitive fields, request-builder ergonomics, timeouts, and chunked
// body decoding without performing network I/O.
import conflux.net.http.client;
import conflux.net.http_server_helpers;
import conflux.types;
import std;

namespace http = conflux::http;

static void print_url_parse(
	std::string_view raw) {
	auto parsed = http::Url::parse(raw);
	if (!parsed) {
		std::println("url parse failed for '{}': {}", raw, parsed.error().message);
		return;
	}
	std::println(
		"url parsed: scheme={} host={} port={} path={} query={}",
		parsed->scheme,
		parsed->host,
		parsed->port,
		parsed->path,
		parsed->query);
}

int main() {
	print_url_parse("https://example.com/api/search");
	print_url_parse("ftp://example.com/nope");

	auto url = *http::Url::parse("https://api.example.test/v1/items");
	url.set_query_param("q", "fast json & http");
	url.set_query_param("limit", "25");

	conflux::http::HttpFields defaults{true};
	defaults.set("User-Agent", "conflux-example/1");
	defaults.append("Accept", "application/json");
	defaults.append("Accept", "text/plain");
	std::println("accept values: {}", defaults.values("accept").size());

	conflux::http::HttpFields form;
	form.set("name", "Ada Lovelace");
	form.set("role", "admin/operator");

	auto req = http::ClientRequest::post(url.str())
				   .headers(defaults)
				   .query("trace", "local demo")
				   .bearer("example-token")
				   .if_none_match(R"("cached-etag")")
				   .timeouts({
					   .resolve = std::chrono::milliseconds{750},
					   .connect = std::chrono::milliseconds{750},
					   .tls = std::chrono::milliseconds{1000},
					   .write = std::chrono::milliseconds{1500},
					   .first_byte = std::chrono::milliseconds{2000},
					   .between_bytes = std::chrono::milliseconds{2000},
				   })
				   .follow_redirects(3)
				   .body_form(form)
				   .build();

	std::println("{} {}", req.method(), req.url().str());
	std::println("content-type: {}", req.headers()["content-type"]);
	std::println("authorization: {}", req.headers()["authorization"]);
	std::println("body: {}", req.body());
	std::println("redirect limit: {} verify_peer={}", req.max_redirects(), req.verify_peer());

	std::string body;
	auto consumed = decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", 64, 8, body);
	std::println("chunked decode: {}", consumed > 0 ? body : "<invalid>");
}
