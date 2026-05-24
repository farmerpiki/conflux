import conflux.curated;

int main() {
	static_assert(requires { typename WorkPool; }, "conflux_api_surface_curated_unexpected_workpool_visible");
}
