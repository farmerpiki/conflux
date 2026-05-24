// Intentionally invalid: file helper is extended HTTP API.
import std;
import conflux.http;

int main() {
	(void)http::file("index.html");
}
