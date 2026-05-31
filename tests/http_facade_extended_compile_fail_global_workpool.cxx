import conflux.http.extended;

int main() {
	static_assert(requires { typename WorkPool; }, "conflux_http_extended_unexpected_workpool_global_visible");
}
