// Template engine + HTTP example: render strings and files from JSON context.
//
// Build and run: build/release-clang-libcxx/conflux_template_pages_example
// Try:
//   curl http://localhost:9102/
//   curl http://localhost:9102/users
import conflux.net.http.server;
import conflux.templates;
import conflux.types;
import std;

static void write_template(
	std::filesystem::path const &path,
	std::string_view body) {
	std::ofstream out(path, std::ios::binary);
	out << body;
}

int main() {
	auto dir = std::filesystem::temp_directory_path() / "conflux_template_pages";
	std::filesystem::create_directories(dir);

	write_template(
		dir / "layout.html",
		"<html><body>"
		"<nav><a href='/'>home</a> <a href='/users'>users</a></nav>"
		"{% block body %}{% endblock %}"
		"</body></html>");
	write_template(
		dir / "home.html",
		"{% extends 'layout.html' %}"
		"{% block body %}<h1>{{ title | upper }}</h1><p>{{ message }}</p>{% endblock %}");
	write_template(
		dir / "users.html",
		"{% extends 'layout.html' %}"
		"{% block body %}<h1>Users</h1><ul>"
		"{% for user in users %}"
		"<li>{{ loop.index }}. {{ user.name }} — {{ user.role | default('viewer') }}</li>"
		"{% endfor %}"
		"</ul>{% endblock %}");

	auto env = std::make_shared<conflux::templates::Environment>(dir.string());
	env->blocking_load_all();

	namespace http = conflux::http;
	auto app = http::App::default_server();
	app.get("/", [env](HttpRequest const &) {
		return HttpResponse::html(
			env->render("home.html", R"({"title":"conflux templates","message":"Rendered from a JSON context."})"));
	});
	app.get("/users", [env](HttpRequest const &) {
		return HttpResponse::html(env->render(
			"users.html",
			R"({"users":[{"name":"Ada","role":"admin"},{"name":"Linus"},{"name":"Grace","role":"operator"}]})"));
	});
	app.get("/inline", [env](HttpRequest const &) {
		return HttpResponse::text(env->render_string(
			"{% for x in items %}{{ x | capitalize }}{% if loop.index != loop.length %}, {% endif %}{% endfor %}",
			R"({"items":["alpha","beta","gamma"]})"));
	});

	std::println("template pages listening on http://localhost:9102/");
	auto const status = std::move(app).run({.port = 9102});
	return status == RunStatus::stopped_normally ? 0 : 1;
}
