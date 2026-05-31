import conflux.net.http.native_json;

template<class Provider>
concept HasProviderTemplateResponseAlias = requires {
	conflux::http::codec::json::try_response<Provider>(42);
} || requires { conflux::http::codec::json::response_or_internal_error<Provider>(42); };

int main() {
	static_assert(
		HasProviderTemplateResponseAlias<conflux::http::codec::json::DefaultJsonProvider>,
		"conflux_http_json_unexpected_provider_template_response_alias_visible");
}
