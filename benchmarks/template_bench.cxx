import std;
import conflux.types;
import conflux.templates;
import conflux.json;
namespace {

struct Stats {
	double median_ns;
	std::size_t iters;
	double total_ns;
};
template<typename F>
Stats measure(
	F &&fn,
	std::size_t warmup,
	std::size_t iters) {
	for (std::size_t i = 0; i < warmup; ++i) {
		fn();
	}
	std::vector<double> samples;
	samples.reserve(iters);
	double total_ns = 0.0;
	for (std::size_t i = 0; i < iters; ++i) {
		auto t0 = std::chrono::steady_clock::now();
		fn();
		auto t1 = std::chrono::steady_clock::now();
		auto const dur = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		total_ns += dur;
		samples.push_back(dur);
	}
	sort(samples.begin(), samples.end());
	return {samples[iters / 2], iters, total_ns};
}
bool g_csv = false;
bool g_json = false;
void report(
	std::string_view name,
	Stats const &s) {
	if (g_json) {
		auto const ns_per_iter = s.total_ns / static_cast<double>(s.iters);
		std::println(
			R"({{"config":"default","variant":"{}","iterations":{},"total_ns":{},"ns_per_iter":{:.2f}}})",
			name,
			s.iters,
			static_cast<std::uint64_t>(s.total_ns),
			ns_per_iter);
	} else if (g_csv) {
		std::println("{},{},{},{:.2f}", name, s.iters, static_cast<std::uint64_t>(s.total_ns), s.median_ns);
	} else {
		std::println("[tmpl-bench] {:<50} {:>10.1f} ns", name, s.median_ns);
	}
}
// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Simple variable substitution: {{ name }}
constexpr std::string_view kSimpleTmpl = "Hello, {{ name }}! You have {{ count }} messages.";
constexpr std::string_view kSimpleCtx = R"({"name":"Alice","count":42})";

// Loop over 10 items
constexpr std::string_view kLoopTmpl = R"(
{%-for item in items-%}
{{loop.index}}.{{item.title}}({{item.score}})
{%-endfor-%}
)";
constexpr std::string_view kLoopCtx10 = R"({
"items":[
{"title":"Alpha","score":95},{"title":"Beta","score":87},
{"title":"Gamma","score":76},{"title":"Delta","score":91},
{"title":"Epsilon","score":83},{"title":"Zeta","score":78},
{"title":"Eta","score":89},{"title":"Theta","score":72},
{"title":"Iota","score":94},{"title":"Kappa","score":81}
]
})";
// Loop over 100 items
std::string make_loop_ctx_100() {
	std::string out = R"({"items":[)";
	for (int i = 0; i < 100; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(R"({{"title":"Item{}","score":{}}})", i, 50 + i % 50);
	}
	out += "]}";
	return out;
}
// Nested conditionals
constexpr std::string_view kCondTmpl = R"(
{%-if user.admin-%}
Admin:{{user.name}}
{%-elif user.active-%}
User:{{user.name}}({{user.role}})
{%-else-%}
Guest
{%-endif-%}
)";
constexpr std::string_view kCondCtxTrue = R"({"user":{"admin":false,"active":true,"name":"Bob","role":"editor"}})";

// Filter chain: value | upper | replace(",", "") | default("n/a")
constexpr std::string_view kFilterTmpl = R"({{ tags | join(",") | upper }})";
constexpr std::string_view kFilterCtx = R"({"tags":["rust","cpp","python","go","zig"]})";

// HTML-like template with blocks, loops and filters (realistic web page fragment)
constexpr std::string_view kPageTmpl = R"(
<ul>
{%-for p in products-%}
<li class="{{ p.category | lower }}">
<strong>{{p.name|e}}</strong>—${{p.price}}
{%-if p.sale%}<span class="sale">SALE</span>{%-endif%}
</li>
{%-endfor-%}
</ul>
<p>Total:{{products|length}}items</p>
)";
constexpr std::string_view kPageCtx = R"({
"products":[
{"name":"Widget <std::array>","category":"TOOLS","price":"9.99","sale":true},
{"name":"Gadget B","category":"ELECTRONICS","price":"49.99","sale":false},
{"name":"Doohickey C","category":"TOOLS","price":"4.99","sale":true},
{"name":"Thingamajig D","category":"MISC","price":"14.99","sale":false},
{"name":"Whatsit E","category":"ELECTRONICS","price":"99.99","sale":true}
]
})";

// Macro definition and call
constexpr std::string_view kMacroTmpl = R"(
{%-macro badge(label,cls)-%}
<span class="{{ cls }}">{{label}}</span>
{%-endmacro-%}
{{badge("New","tag-new")}}{{badge("Hot","tag-hot")}}{{badge("Sale","tag-sale")}}
)";
constexpr std::string_view kMacroCtx = R"({})";

constexpr std::string_view kExprHeavyTmpl = R"(
{%-for p in products-%}
{{ (prefix ~ p.name) | replace(" ", "_") | lower }}:{{ p.category.lower() }}:{{ p.name[0:3] }}:{%-if p.sale and p.category in sale_categories-%}Y{%-else-%}N{%-endif-%};
{%-endfor-%}
)";
constexpr std::string_view kExprHeavyCtx = R"({
"prefix":"sku-",
"sale_categories":["TOOLS","ELECTRONICS"],
"products":[
{"name":"Widget Alpha","category":"TOOLS","sale":true},
{"name":"Gadget Beta","category":"ELECTRONICS","sale":false},
{"name":"Doohickey Gamma","category":"TOOLS","sale":true},
{"name":"Thing Delta","category":"MISC","sale":true},
{"name":"Whatsit Epsilon","category":"ELECTRONICS","sale":true}
]
})";

std::filesystem::path make_template_dir(
	std::string_view name,
	std::string_view source) {
	auto dir = std::filesystem::temp_directory_path()
		/ format("conflux_template_bench_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	std::filesystem::create_directories(dir);
	std::ofstream out{dir / std::string{name}};
	out << source;
	return dir;
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	for (int i = 1; i < argc; ++i) {
		std::string_view const a{argv[i]};
		if (a == "--bench-info") {
			std::print(
				"{}\n",
				R"({"name":"template","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
			return 0;
		}
		if (a == "--csv") {
			g_csv = true;
		}
		if (a == "--json") {
			g_json = true;
		}
	}
	if (g_json) {
		// NDJSON mode is consumed by scripts/bench_record.sh.
	} else if (g_csv) {
		std::println("variant,iterations,total_ns,ns_per_iter");
	} else {
		std::println("[tmpl-bench] Template engine benchmarks");
		std::println("[tmpl-bench] {}", std::string(60, '-'));
	}

	// Each sub-bench creates its own Environment to also measure cold-path
	// parse+render. The hot-path bench reuses a pre-parsed environment.

	// --- parse + render (cold, single render call) ---
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kSimpleTmpl), std::string(kSimpleCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: simple substitution", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kLoopTmpl), std::string(kLoopCtx10));
				(void)out;
			},
			20,
			500);
		report("parse+render: loop 10 items", s);
	}
	{
		std::string ctx100 = make_loop_ctx_100();
		auto s = measure(
			[&ctx100] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kLoopTmpl), ctx100);
				(void)out;
			},
			20,
			500);
		report("parse+render: loop 100 items", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kCondTmpl), std::string(kCondCtxTrue));
				(void)out;
			},
			20,
			500);
		report("parse+render: conditionals (true branch)", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kFilterTmpl), std::string(kFilterCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: filter chain (join+upper)", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kPageTmpl), std::string(kPageCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: page fragment (5 products)", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kMacroTmpl), std::string(kMacroCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: macro define+call x3", s);
	}
	{
		auto s = measure(
			[] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kExprHeavyTmpl), std::string(kExprHeavyCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: expression-heavy page", s);
	}

	if (!g_csv && !g_json) {
		std::println("[tmpl-bench] {}", std::string(60, '-'));
	}

	// --- render-only (hot path: template pre-parsed, context varies) ---
	{
		conflux::templates::Environment const env{"."};
		// warm parse
		(void)env.render_string(std::string(kSimpleTmpl), std::string(kSimpleCtx));
		// NOTE: render_string re-parses every call; use a file-loaded env for
		// true render-only. We measure render_string to keep it self-contained.
		// Still useful: shows parse overhead relative to render.
	}

	// render_string always re-parses. To isolate render cost, benchmark
	// render() with a loaded template file via load_all(). Instead, show
	// context-parse + render cost by varying only the context JSON.
	{
		conflux::templates::Environment env{"."};
		// Pre-parse by doing one call (render_string re-parses each call, so
		// we can't eliminate it — measure as-is and label accurately).
		std::string ctx100 = make_loop_ctx_100();
		auto s = measure(
			[&] {
				std::string const out = env.render_string(std::string(kLoopTmpl), ctx100);
				(void)out;
			},
			50,
			2000);
		report("render_string: loop 100 (shared env, re-parse)", s);
	}
	{
		auto s = measure(
			[&] {
				conflux::templates::Environment const env{"."};
				std::string const out = env.render_string(std::string(kPageTmpl), std::string(kPageCtx));
				(void)out;
			},
			50,
			2000);
		report("render_string: page fragment (2000 iters)", s);
	}
	{
		auto parsed = conflux::json::parse(std::string_view{kPageCtx});
		if (!parsed) {
			std::println("failed to parse benchmark context");
			return 1;
		}
		conflux::templates::Environment env{"."};
		auto s = measure(
			[&] {
				std::string const out = env.render_string(std::string(kPageTmpl), parsed->root());
				(void)out;
			},
			50,
			2000);
		report("render_string: page fragment (parsed ctx)", s);
	}
	{
		auto parsed = conflux::json::parse(std::string_view{kPageCtx});
		if (!parsed) {
			std::println("failed to parse benchmark context");
			return 1;
		}
		auto dir = make_template_dir("page.html", kPageTmpl);
		conflux::templates::Environment env{dir.string()};
		env.blocking_load_all();
		auto s = measure(
			[&] {
				std::string const out = env.render("page.html", parsed->root());
				(void)out;
			},
			50,
			5000);
		std::filesystem::remove_all(dir);
		report("render cached: page fragment (parsed ctx)", s);
	}
	{
		auto parsed = conflux::json::parse(std::string_view{kExprHeavyCtx});
		if (!parsed) {
			std::println("failed to parse benchmark context");
			return 1;
		}
		auto dir = make_template_dir("expr.html", kExprHeavyTmpl);
		conflux::templates::Environment env{dir.string()};
		env.blocking_load_all();
		auto s = measure(
			[&] {
				std::string const out = env.render("expr.html", parsed->root());
				(void)out;
			},
			50,
			5000);
		std::filesystem::remove_all(dir);
		report("render cached: expression-heavy page (parsed ctx)", s);
	}

	if (!g_csv && !g_json) {
		std::println("[tmpl-bench] {}", std::string(60, '-'));
		std::println("[tmpl-bench] Done.");
	}
}
