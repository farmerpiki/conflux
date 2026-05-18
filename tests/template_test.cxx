// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.templates;
import conflux.json;

namespace templates = conflux::templates;
using namespace templates;
static Environment make_env() {
	return Environment{"/nonexistent"};
}
// ---------------------------------------------------------------------------
// Basic rendering
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: plain text passthrough",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("hello world", "{}") == "hello world");
}
TEST_CASE(
	"template: expression substitution",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("Hello, {{ name }}!", R"({"name":"Alice"})") == "Hello, Alice!");
}
TEST_CASE(
	"template: nested object access",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ user.name }}", R"({"user":{"name":"Bob"}})") == "Bob");
}
TEST_CASE(
	"template: integer expression",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ count }}", R"({"count":42})") == "42");
}
// ---------------------------------------------------------------------------
// If / elif / else
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: if true branch",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% if ok %}yes{% endif %}", R"({"ok":true})");
	CHECK(result == "yes");
}
TEST_CASE(
	"template: if false skips body",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% if ok %}yes{% endif %}", R"({"ok":false})");
	CHECK(result.empty());
}
TEST_CASE(
	"template: if else",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% if ok %}yes{% else %}no{% endif %}", R"({"ok":false})");
	CHECK(result == "no");
}
TEST_CASE(
	"template: elif branch",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% if a %}A{% elif b %}B{% else %}C{% endif %}", R"({"a":false,"b":true})");
	CHECK(result == "B");
}
// ---------------------------------------------------------------------------
// For loops
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: for loop over A",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for x in items %}{{ x }},{% endfor %}", R"({"items":[1,2,3]})");
	CHECK(result == "1,2,3,");
}
TEST_CASE(
	"template: for loop over empty A",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for x in items %}{{ x }}{% endfor %}", R"({"items":[]})");
	CHECK(result.empty());
}
TEST_CASE(
	"template: for loop over object fields",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for k, v in obj.items() %}{{ k }}={{ v }};{% endfor %}", R"({"obj":{"a":1}})");
	CHECK(result == "a=1;");
}
TEST_CASE(
	"template: loop.first is true only for first iteration",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{% for x in items %}{% if loop.first %}[{% endif %}{{ x }}{% endfor %}",
		R"({"items":["a","b","c"]})");
	CHECK(result == "[abc");
}
TEST_CASE(
	"template: loop.last is true only for last iteration",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{% for x in items %}{{ x }}{% if loop.last %}]{% endif %}{% endfor %}",
		R"({"items":["a","b","c"]})");
	CHECK(result == "abc]");
}
TEST_CASE(
	"template: loop.index is 1-based",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for x in items %}{{ loop.index }}{% endfor %}", R"({"items":["a","b","c"]})");
	CHECK(result == "123");
}
TEST_CASE(
	"template: loop.index0 is 0-based",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for x in items %}{{ loop.index0 }}{% endfor %}", R"({"items":["a","b","c"]})");
	CHECK(result == "012");
}
TEST_CASE(
	"template: loop.length reports A size",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% for x in items %}{{ loop.length }}{% endfor %}", R"({"items":[1,2,3]})");
	CHECK(result == "333");
}
TEST_CASE(
	"template: nested for loop restores outer loop variable",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{% for i in outer %}{% for j in inner %}{{ loop.index0 }}{% endfor %}|{{ loop.index0 }}{% endfor %}",
		R"({"outer":["a","b"],"inner":["x","y"]})");
	// inner loop: index0 goes 0,1 → "01"
	// after inner, outer loop.index0 should be 0 then 1
	CHECK(result == "01|001|1");
}
// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: set variable",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% set x = 'hello' %}{{ x }}", "{}");
	CHECK(result == "hello");
}
// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: comment stripped",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("before{# comment #}after", "{}");
	CHECK(result == "beforeafter");
}
// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: upper filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ name | upper }}", R"({"name":"alice"})") == "ALICE");
}
TEST_CASE(
	"template: lower filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ name | lower }}", R"({"name":"ALICE"})") == "alice");
}
TEST_CASE(
	"template: length filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ items | length }}", R"({"items":[1,2,3]})") == "3");
}
TEST_CASE(
	"template: default filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ missing | default('fallback') }}", "{}") == "fallback");
}
// ---------------------------------------------------------------------------
// Missing key and undefined
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: missing key renders empty",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{{ missing }}", "{}");
	CHECK(result.empty());
}
// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: macro definition and call",
	"[template]") {
	auto env = make_env();
	auto result =
		env.render_string("{% macro greet(name) %}Hello, {{ name }}!{% endmacro %}{{ greet('World') }}", "{}");
	CHECK(result == "Hello, World!");
}
TEST_CASE(
	"template: macro with default parameter",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% macro greet(name='Guest') %}Hi, {{ name }}{% endmacro %}{{ greet() }}", "{}");
	CHECK(result == "Hi, Guest");
}
TEST_CASE(
	"template: macro keyword args and default filters",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{% macro badge(label='new'|upper, cls='tag') %}"
		"<{{ cls }}>{{ label }}</{{ cls }}>"
		"{% endmacro %}{{ badge(cls='pill') }}",
		"{}");
	CHECK(result == "<pill>NEW</pill>");
}
TEST_CASE(
	"template: compiled expression tree covers operators paths and filter args",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{{ missing | default(prefix ~ user.name|lower) }}:"
		"{{ items[1].title() }}:"
		"{{ user.name[1:4] }}:"
		"{% if user.active and 2 in nums %}yes{% endif %}",
		R"({"prefix":"hi-","user":{"name":"ALICE","active":true},"items":["zero","two"],"nums":[1,2,3]})");
	CHECK(result == "hi-alice:Two:LIC:yes");
}
// ---------------------------------------------------------------------------
// Expressions: arithmetic and comparisons
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: if with equality comparison",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string("{% if x == 5 %}five{% endif %}", R"({"x":5})");
	CHECK(result == "five");
}
TEST_CASE(
	"template: nested for with outer context",
	"[template]") {
	auto env = make_env();
	auto result = env.render_string(
		"{% for item in list %}{{ prefix }}{{ item }};{% endfor %}",
		R"({"prefix":"x","list":["a","b"]})");
	CHECK(result == "xa;xb;");
}
// ---------------------------------------------------------------------------
// Filters: join, replace, capitalize
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: join filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ items | join(', ') }}", R"({"items":["a","b","c"]})") == "a, b, c");
}
TEST_CASE(
	"template: replace filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ s | replace('o', '0') }}", R"({"s":"foo"})") == "f00");
}
TEST_CASE(
	"template: capitalize filter",
	"[template]") {
	auto env = make_env();
	CHECK(env.render_string("{{ s | capitalize }}", R"({"s":"hello world"})") == "Hello world");
}
// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_CASE(
	"template: render unknown named template throws",
	"[template]") {
	auto env = make_env();
	CHECK_THROWS_AS(env.render("no_such.html", "{}"), std::runtime_error);
}
// ---------------------------------------------------------------------------
// Template file loading: extends / block / include
// ---------------------------------------------------------------------------

namespace {

struct TmplDir {
	char path[64]{};
	TmplDir() {
		std::snprintf(path, sizeof(path), "/tmp/conflux_tmpl_XXXXXX");
		REQUIRE(::mkdtemp(path) != nullptr);
	}
	~TmplDir() { ::rmdir(path); }
	void write(
		std::string_view name,
		std::string_view content) const {
		auto full = std::string{path} + "/" + std::string{name};
		int const fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		[[maybe_unused]] auto n = ::write(fd, content.data(), content.size());
		::close(fd);
	}
	void rm(
		std::string_view name) const {
		auto full = std::string{path} + "/" + std::string{name};
		::unlink(full.c_str());
	}
	[[nodiscard]] Environment make() const { return Environment{std::string{path}}; }
};

} // namespace
TEST_CASE(
	"template: include inlines another template",
	"[template]") {
	TmplDir dir;
	dir.write("part.html", "world");
	dir.write("main.html", "hello {% include 'part.html' %}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("main.html", "{}") == "hello world");

	dir.rm("part.html");
	dir.rm("main.html");
}
TEST_CASE(
	"template: extends with block override",
	"[template]") {
	TmplDir dir;
	dir.write("base.html", "<html>{% block title %}Default{% endblock %}</html>");
	dir.write("child.html", "{% extends 'base.html' %}{% block title %}Custom{% endblock %}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("child.html", "{}") == "<html>Custom</html>");

	dir.rm("base.html");
	dir.rm("child.html");
}
TEST_CASE(
	"template: extends uses base block when child does not override",
	"[template]") {
	TmplDir dir;
	dir.write("base.html", "[{% block body %}fallback{% endblock %}]");
	dir.write("child.html", "{% extends 'base.html' %}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("child.html", "{}") == "[fallback]");

	dir.rm("base.html");
	dir.rm("child.html");
}
TEST_CASE(
	"template: include with context variables",
	"[template]") {
	TmplDir dir;
	dir.write("greet.html", "Hello, {{ name }}!");
	dir.write("main.html", "{% include 'greet.html' %}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("main.html", R"({"name":"Alice"})") == "Hello, Alice!");

	dir.rm("greet.html");
	dir.rm("main.html");
}
TEST_CASE(
	"template: missing include fails checked load before publication",
	"[template]") {
	TmplDir dir;
	dir.write("main.html", "{% include 'missing.html' %}");

	auto env = dir.make();
	auto report = env.blocking_load_all_checked();
	REQUIRE_FALSE(report);
	REQUIRE_FALSE(report.error().diagnostics.empty());
	CHECK(report.error().diagnostics.front().phase == TemplateDiagnosticPhase::link);
	CHECK(report.error().diagnostics.front().code == "include_not_found");
	CHECK_THROWS_AS(env.render("main.html", "{}"), std::runtime_error);

	dir.rm("main.html");
}
TEST_CASE(
	"template: failed reload keeps old cache",
	"[template]") {
	TmplDir dir;
	dir.write("part.html", "world");
	dir.write("main.html", "hello {% include 'part.html' %}");

	auto env = dir.make();
	env.blocking_load_all();
	CHECK(env.render("main.html", "{}") == "hello world");

	dir.rm("part.html");
	auto report = env.blocking_reload_all_checked();
	REQUIRE_FALSE(report);
	CHECK(report.error().diagnostics.front().code == "include_not_found");
	CHECK(env.render("main.html", "{}") == "hello world");

	dir.rm("main.html");
}
TEST_CASE(
	"template: render accepts parsed json context",
	"[template]") {
	TmplDir dir;
	dir.write("main.html", "Hello, {{ user.name }}!");

	auto env = dir.make();
	env.blocking_load_all();
	auto doc = conflux::json::parse(R"({"user":{"name":"Alice"}})");
	REQUIRE(doc);
	CHECK(env.render("main.html", doc->root()) == "Hello, Alice!");
	CHECK(env.render_string("{{ user.name }}", doc->root()) == "Alice");

	dir.rm("main.html");
}
TEST_CASE(
	"template: from import macro",
	"[template]") {
	TmplDir dir;
	dir.write("macros.html", "{% macro greet(name) %}Hello, {{ name }}!{% endmacro %}");
	dir.write("main.html", "{% from 'macros.html' import greet %}{{ greet('World') }}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("main.html", "{}") == "Hello, World!");

	dir.rm("macros.html");
	dir.rm("main.html");
}
TEST_CASE(
	"template: from import macro with alias",
	"[template]") {
	TmplDir dir;
	dir.write("macros.html", "{% macro greet(name) %}Hi {{ name }}{% endmacro %}");
	dir.write("main.html", "{% from 'macros.html' import greet as say_hi %}{{ say_hi('Bob') }}");

	auto env = dir.make();
	env.load_all();
	CHECK(env.render("main.html", "{}") == "Hi Bob");

	dir.rm("macros.html");
	dir.rm("main.html");
}
