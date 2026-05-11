import std;
import conflux.types;
import conflux.templates;
namespace {

struct Stats {
	double median_ns;
	SZ iters;
	double total_ns;
};
template<typename F>
Stats measure(
	F &&fn,
	SZ warmup,
	SZ iters) {
	for (SZ i = 0; i < warmup; ++i) {
		fn();
	}
	V<double> samples;
	samples.reserve(iters);
	double total_ns = 0.0;
	for (SZ i = 0; i < iters; ++i) {
		auto t0 = chrono::steady_clock::now();
		fn();
		auto t1 = chrono::steady_clock::now();
		auto const dur = static_cast<double>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
		total_ns += dur;
		samples.push_back(dur);
	}
	sort(samples.begin(), samples.end());
	return {samples[iters / 2], iters, total_ns};
}
bool g_csv = false;
void report(
	SV name,
	Stats const &s) {
	if (g_csv) {
		println("{},{},{},{:.2f}", name, s.iters, static_cast<u64>(s.total_ns), s.median_ns);
	} else {
		println("[tmpl-bench] {:<50} {:>10.1f} ns", name, s.median_ns);
	}
}
// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Simple variable substitution: {{ name }}
constexpr SV kSimpleTmpl = "Hello, {{ name }}! You have {{ count }} messages.";
constexpr SV kSimpleCtx = R"({"name":"Alice","count":42})";

// Loop over 10 items
constexpr SV kLoopTmpl = R"(
{%-for item in items-%}
{{loop.index}}.{{item.title}}({{item.score}})
{%-endfor-%}
)";
constexpr SV kLoopCtx10 = R"({
"items":[
{"title":"Alpha","score":95},{"title":"Beta","score":87},
{"title":"Gamma","score":76},{"title":"Delta","score":91},
{"title":"Epsilon","score":83},{"title":"Zeta","score":78},
{"title":"Eta","score":89},{"title":"Theta","score":72},
{"title":"Iota","score":94},{"title":"Kappa","score":81}
]
})";
// Loop over 100 items
S make_loop_ctx_100() {
	S out = R"({"items":[)";
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
constexpr SV kCondTmpl = R"(
{%-if user.admin-%}
Admin:{{user.name}}
{%-elif user.active-%}
User:{{user.name}}({{user.role}})
{%-else-%}
Guest
{%-endif-%}
)";
constexpr SV kCondCtxTrue = R"({"user":{"admin":false,"active":true,"name":"Bob","role":"editor"}})";

// Filter chain: value | upper | replace(",", "") | default("n/a")
constexpr SV kFilterTmpl = R"({{ tags | join(",") | upper }})";
constexpr SV kFilterCtx = R"({"tags":["rust","cpp","python","go","zig"]})";

// HTML-like template with blocks, loops and filters (realistic web page fragment)
constexpr SV kPageTmpl = R"(
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
constexpr SV kPageCtx = R"({
"products":[
{"name":"Widget <A>","category":"TOOLS","price":"9.99","sale":true},
{"name":"Gadget B","category":"ELECTRONICS","price":"49.99","sale":false},
{"name":"Doohickey C","category":"TOOLS","price":"4.99","sale":true},
{"name":"Thingamajig D","category":"MISC","price":"14.99","sale":false},
{"name":"Whatsit E","category":"ELECTRONICS","price":"99.99","sale":true}
]
})";

// Macro definition and call
constexpr SV kMacroTmpl = R"(
{%-macro badge(label,cls)-%}
<span class="{{ cls }}">{{label}}</span>
{%-endmacro-%}
{{badge("New","tag-new")}}{{badge("Hot","tag-hot")}}{{badge("Sale","tag-sale")}}
)";
constexpr SV kMacroCtx = R"({})";

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	for (int i = 1; i < argc; ++i) {
		SV const a{argv[i]};
		if (a == "--bench-info") {
			std::print(
				"{}\n",
				R"({"name":"template","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
			return 0;
		}
		if (a == "--csv") {
			g_csv = true;
		}
	}
	if (g_csv) {
		println("variant,iterations,total_ns,ns_per_iter");
	} else {
		println("[tmpl-bench] Template engine benchmarks");
		println("[tmpl-bench] {}", S(60, '-'));
	}

	// Each sub-bench creates its own Environment to also measure cold-path
	// parse+render. The hot-path bench reuses a pre-parsed environment.

	// --- parse + render (cold, single render call) ---
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kSimpleTmpl), S(kSimpleCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: simple substitution", s);
	}
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kLoopTmpl), S(kLoopCtx10));
				(void)out;
			},
			20,
			500);
		report("parse+render: loop 10 items", s);
	}
	{
		S ctx100 = make_loop_ctx_100();
		auto s = measure(
			[&ctx100] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kLoopTmpl), ctx100);
				(void)out;
			},
			20,
			500);
		report("parse+render: loop 100 items", s);
	}
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kCondTmpl), S(kCondCtxTrue));
				(void)out;
			},
			20,
			500);
		report("parse+render: conditionals (true branch)", s);
	}
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kFilterTmpl), S(kFilterCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: filter chain (join+upper)", s);
	}
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kPageTmpl), S(kPageCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: page fragment (5 products)", s);
	}
	{
		auto s = measure(
			[] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kMacroTmpl), S(kMacroCtx));
				(void)out;
			},
			20,
			500);
		report("parse+render: macro define+call x3", s);
	}

	if (!g_csv) {
		println("[tmpl-bench] {}", S(60, '-'));
	}

	// --- render-only (hot path: template pre-parsed, context varies) ---
	{
		tmpl::Environment const env{"."};
		// warm parse
		(void)env.render_string(S(kSimpleTmpl), S(kSimpleCtx));
		// NOTE: render_string re-parses every call; use a file-loaded env for
		// true render-only. We measure render_string to keep it self-contained.
		// Still useful: shows parse overhead relative to render.
	}

	// render_string always re-parses. To isolate render cost, benchmark
	// render() with a loaded template file via load_all(). Instead, show
	// context-parse + render cost by varying only the context JSON.
	{
		tmpl::Environment env{"."};
		// Pre-parse by doing one call (render_string re-parses each call, so
		// we can't eliminate it — measure as-is and label accurately).
		S ctx100 = make_loop_ctx_100();
		auto s = measure(
			[&] {
				S const out = env.render_string(S(kLoopTmpl), ctx100);
				(void)out;
			},
			50,
			2000);
		report("render_string: loop 100 (shared env, re-parse)", s);
	}
	{
		S const ctx100 = make_loop_ctx_100();
		auto s = measure(
			[&] {
				tmpl::Environment const env{"."};
				S const out = env.render_string(S(kPageTmpl), S(kPageCtx));
				(void)out;
			},
			50,
			2000);
		report("render_string: page fragment (2000 iters)", s);
	}

	if (!g_csv) {
		println("[tmpl-bench] {}", S(60, '-'));
		println("[tmpl-bench] Done.");
	}
}
