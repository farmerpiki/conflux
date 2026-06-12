module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

// ---------------------------------------------------------------------------
// Environment public interface
// ---------------------------------------------------------------------------

static TemplateSourceLocation template_location(
	std::string const &name,
	std::string const &path = {}) {
	return TemplateSourceLocation{.template_name = name, .path = path};
}
static void add_template_diag(
	TemplateBuildReport &report,
	TemplateDiagnosticPhase phase,
	TemplateSourceLocation location,
	std::string code,
	std::string message,
	std::vector<TemplateSourceLocation> stack = {},
	std::string check_label = {}) {
	report.diagnostics.push_back(
		TemplateDiagnostic{
			.severity = TemplateDiagnosticSeverity::error,
			.phase = phase,
			.location = std::move(location),
			.stack = std::move(stack),
			.check_label = std::move(check_label),
			.code = std::move(code),
			.message = std::move(message),
		});
}
static bool node_list_has_top_level_macro(
	NodeList const &nodes,
	std::string_view name) {
	for (auto const &node: nodes) {
		bool found = false;
		std::visit(
			[&](auto const &n) {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, MacroNode>) {
					found = n.name == name;
				}
			},
			node->data);
		if (found) {
			return true;
		}
	}
	return false;
}
template<class Fn>
static void for_each_direct_node_dep(
	NodeList const &nodes,
	Fn &&fn) {
	for (auto const &node: nodes) {
		std::visit(
			[&](auto const &n) {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, IncludeNode>) {
					fn(n.name);
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					fn(n.file);
				} else if constexpr (std::is_same_v<T, BlockNode>) {
					for_each_direct_node_dep(n.body, fn);
				} else if constexpr (std::is_same_v<T, ForNode>) {
					for_each_direct_node_dep(n.body, fn);
				} else if constexpr (std::is_same_v<T, IfNode>) {
					for (auto const &branch: n.branches) {
						for_each_direct_node_dep(branch.body, fn);
					}
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					for_each_direct_node_dep(n.body, fn);
				}
			},
			node->data);
	}
}
template<class Fn>
static void for_each_direct_template_dep(
	Template const &tmpl,
	Fn &&fn) {
	if (!tmpl.extends_name.empty()) {
		fn(tmpl.extends_name);
	}
	for_each_direct_node_dep(tmpl.nodes, fn);
}
bool Environment::Impl::extension_allowed(
	std::filesystem::path const &path) const {
	auto ext = path.extension().string();
	return std::ranges::contains(options.extensions, ext);
}
void Environment::Impl::validate_links(
	std::unordered_map<std::string, Template> const &candidate,
	TemplateBuildReport &report) const {
	std::function<void(std::string const &, NodeList const &)> validate_nodes;
	validate_nodes = [&](std::string const &owner, NodeList const &nodes) {
		for (auto const &node: nodes) {
			std::visit(
				[&](auto const &n) {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, IncludeNode>) {
						if (!candidate.contains(n.name)) {
							add_template_diag(
								report,
								TemplateDiagnosticPhase::link,
								template_location(owner),
								"include_not_found",
								std::format("include '{}' was not loaded", n.name),
								{template_location(owner), template_location(n.name)});
						}
					} else if constexpr (std::is_same_v<T, FromImportNode>) {
						auto it = candidate.find(n.file);
						if (it == candidate.end()) {
							add_template_diag(
								report,
								TemplateDiagnosticPhase::link,
								template_location(owner),
								"import_file_not_found",
								std::format("import file '{}' was not loaded", n.file),
								{template_location(owner), template_location(n.file)});
						} else if (!node_list_has_top_level_macro(it->second.nodes, n.name)) {
							add_template_diag(
								report,
								TemplateDiagnosticPhase::link,
								template_location(owner),
								"import_macro_not_found",
								std::format("macro '{}' was not found in '{}'", n.name, n.file),
								{template_location(owner), template_location(n.file)});
						}
					} else if constexpr (std::is_same_v<T, BlockNode>) {
						validate_nodes(owner, n.body);
					} else if constexpr (std::is_same_v<T, ForNode>) {
						validate_nodes(owner, n.body);
					} else if constexpr (std::is_same_v<T, IfNode>) {
						for (auto const &branch: n.branches) {
							validate_nodes(owner, branch.body);
						}
					} else if constexpr (std::is_same_v<T, MacroNode>) {
						validate_nodes(owner, n.body);
					}
				},
				node->data);
		}
	};
	for (auto const &[name, tmpl]: candidate) {
		if (!tmpl.extends_name.empty() && !candidate.contains(tmpl.extends_name)) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::link,
				template_location(name),
				"extends_not_found",
				std::format("parent template '{}' was not loaded", tmpl.extends_name),
				{template_location(name), template_location(tmpl.extends_name)});
		}
		validate_nodes(name, tmpl.nodes);
	}
	std::unordered_map<std::string, std::uint8_t> visit_state;
	std::vector<std::string> stack;
	std::function<void(std::string const &)> dfs;
	dfs = [&](std::string const &name) {
		auto state_it = visit_state.find(name);
		if (state_it != visit_state.end()) {
			if (state_it->second == 1) {
				std::vector<TemplateSourceLocation> diag_stack;
				bool in_cycle = false;
				for (auto const &frame: stack) {
					if (frame == name) {
						in_cycle = true;
					}
					if (in_cycle) {
						diag_stack.push_back(template_location(frame));
					}
				}
				diag_stack.push_back(template_location(name));
				add_template_diag(
					report,
					TemplateDiagnosticPhase::link,
					template_location(name),
					"dependency_cycle",
					std::format("template dependency cycle reaches '{}'", name),
					std::move(diag_stack));
			}
			return;
		}
		visit_state[name] = 1;
		stack.push_back(name);
		auto it = candidate.find(name);
		if (it != candidate.end()) {
			std::vector<std::string> deps;
			for_each_direct_template_dep(it->second, [&](std::string const &dep) {
				if (candidate.contains(dep)) {
					deps.push_back(dep);
				}
			});
			for (auto const &dep: deps) {
				dfs(dep);
			}
		}
		stack.pop_back();
		visit_state[name] = 2;
	};
	for (auto const &[name, _]: candidate) {
		dfs(name);
	}
}
std::expected<std::unordered_map<std::string, Template>, TemplateBuildReport>
Environment::Impl::build_cache_from_directory() const {
	TemplateBuildReport report;
	std::unordered_map<std::string, Template> parsed;
	std::filesystem::path const dir{template_dir};
	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"directory_not_found",
			std::format("template directory '{}' does not exist", dir.string()));
		return std::unexpected{std::move(report)};
	}
	if (!std::filesystem::is_directory(dir, ec)) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"not_directory",
			std::format("template path '{}' is not a directory", dir.string()));
		return std::unexpected{std::move(report)};
	}
	std::vector<std::filesystem::path> files;
	try {
		for (auto const &entry: std::filesystem::directory_iterator(dir)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			if (!extension_allowed(entry.path())) {
				continue;
			}
			files.push_back(entry.path());
		}
	} catch (std::exception const &e) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"directory_scan_failed",
			std::format("failed to scan template directory '{}': {}", dir.string(), e.what()));
		return std::unexpected{std::move(report)};
	}
	std::ranges::sort(files, std::less<>{}, [](std::filesystem::path const &path) -> auto const & {
		return path.native();
	});
	report.templates_seen = files.size();
	for (auto const &path: files) {
		auto name = path.filename().string();
		auto contents = conflux::file_io_sync::blocking_read_text_file(path.string());
		if (!contents) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::io,
				template_location(name, path.string()),
				"read_failed",
				std::format("failed to read template '{}': {}", path.string(), contents.error().what()));
			continue;
		}
		try {
			parsed[name] = parse(name, *contents);
			++report.templates_compiled;
		} catch (std::exception const &e) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::parse,
				template_location(name, path.string()),
				"parse_failed",
				std::format("failed to parse template '{}': {}", name, e.what()));
		}
	}
	if (!report.ok()) {
		return std::unexpected{std::move(report)};
	}
	validate_links(parsed, report);
	if (!report.ok()) {
		return std::unexpected{std::move(report)};
	}
	return parsed;
}
void Environment::Impl::check_render(
	std::unordered_map<std::string, Template> const &candidate,
	std::span<TemplateRenderCheckCase const> cases,
	TemplateRenderCheckOptions opts,
	TemplateBuildReport &report) const {
	std::unordered_set<std::string> covered;
	for (auto const &check: cases) {
		covered.insert(check.template_name);
		auto it = candidate.find(check.template_name);
		if (it == candidate.end()) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::render_check,
				template_location(check.template_name),
				"render_check.template_not_found",
				std::format("render check template '{}' was not loaded", check.template_name),
				{},
				check.label);
			continue;
		}
		try {
			auto out = render_template(it->second, check.context, candidate);
			if (opts.max_output_bytes != 0 && out.size() > opts.max_output_bytes) {
				add_template_diag(
					report,
					TemplateDiagnosticPhase::render_check,
					template_location(check.template_name),
					"render_check.output_too_large",
					std::format(
						"render check output for '{}' was {} bytes, above limit {}",
						check.template_name,
						out.size(),
						opts.max_output_bytes),
					{},
					check.label);
			}
		} catch (std::exception const &e) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::render_check,
				template_location(check.template_name),
				"render_check.render_failed",
				std::format("render check failed for '{}': {}", check.template_name, e.what()),
				{},
				check.label);
		}
	}
	if (opts.require_all_templates_covered) {
		for (auto const &[name, tmpl]: candidate) {
			if (!tmpl.extends_name.empty()) {
				continue;
			}
			if (!covered.contains(name)) {
				add_template_diag(
					report,
					TemplateDiagnosticPhase::render_check,
					template_location(name),
					"render_check.coverage_missing",
					std::format("top-level template '{}' was not covered by a render check", name));
			}
		}
	}
}
std::expected<void, TemplateBuildReport> Environment::Impl::reload_all_checked(
	std::span<TemplateRenderCheckCase const> cases,
	TemplateRenderCheckOptions opts) {
	auto candidate = build_cache_from_directory();
	if (!candidate) {
		return std::unexpected{std::move(candidate.error())};
	}
	TemplateBuildReport render_report;
	render_report.templates_seen = candidate->size();
	render_report.templates_compiled = candidate->size();
	check_render(*candidate, cases, opts, render_report);
	if (!render_report.ok()) {
		return std::unexpected{std::move(render_report)};
	}
	{
		std::unique_lock const lk{cache_mtx};
		cache = std::move(*candidate);
	}
	return {};
}
void Environment::Impl::reload_path(
	std::string const &path) {
	std::filesystem::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	(void)reload_all_checked();
}
void Environment::Impl::remove_path(
	std::string const &path) {
	std::filesystem::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	(void)reload_all_checked();
}
Environment::Environment(
	std::string const &template_dir)
	: impl_(std::make_unique<Impl>()) {
	impl_->template_dir = template_dir;
}
Environment::Environment(
	std::string const &template_dir,
	EnvironmentOptions options)
	: impl_(std::make_unique<Impl>()) {
	impl_->template_dir = template_dir;
	impl_->options = std::move(options);
}
Environment::~Environment() = default;
Environment::Environment(Environment &&) noexcept = default;
Environment &Environment::operator =(Environment &&) noexcept = default;
void Environment::load_all() {
	blocking_load_all();
}
void Environment::blocking_load_all() {
	auto res = blocking_load_all_checked();
	if (!res) {
		throw TemplateBuildError{std::move(res.error())};
	}
}
void Environment::blocking_reload_all() {
	auto res = blocking_reload_all_checked();
	if (!res) {
		throw TemplateBuildError{std::move(res.error())};
	}
}
std::expected<void, TemplateBuildReport> Environment::blocking_load_all_checked() {
	return impl_->reload_all_checked();
}
std::expected<void, TemplateBuildReport> Environment::blocking_reload_all_checked() {
	return impl_->reload_all_checked();
}
std::expected<void, TemplateRenderCheckReport> Environment::blocking_reload_all_checked(
	std::span<TemplateRenderCheckCase const> cases,
	TemplateRenderCheckOptions opts) {
	return impl_->reload_all_checked(cases, opts);
}
std::string Environment::render(
	std::string const &name,
	std::string const &json_ctx) const {
	auto parsed_doc = conflux::json::parse(json_ctx);
	TmplValue ctx = parsed_doc ? node_to_tmpl(parsed_doc->root()) : TmplValue{TmplValue::Object{}};
	return render(name, ctx);
}
std::string Environment::render(
	std::string const &name,
	TmplValue const &ctx) const {
	std::shared_lock const lk{impl_->cache_mtx};
	auto it = impl_->cache.find(name);
	if (it == impl_->cache.end()) {
		throw std::runtime_error{"template not found: " + name};
	}
	return impl_->render_template(it->second, ctx, impl_->cache);
}
std::string Environment::render(
	std::string const &name,
	json::NodeRef ctx) const {
	return render(name, node_to_tmpl(ctx));
}
std::string Environment::render_string(
	std::string const &source,
	std::string const &json_ctx) const {
	auto parsed_doc = conflux::json::parse(json_ctx);
	TmplValue ctx = parsed_doc ? node_to_tmpl(parsed_doc->root()) : TmplValue{TmplValue::Object{}};
	return render_string(source, ctx);
}
std::string Environment::render_string(
	std::string const &source,
	TmplValue const &ctx) const {
	auto tmpl = impl_->parse("<std::string>", source);
	std::shared_lock const lk{impl_->cache_mtx};
	return impl_->render_template(tmpl, ctx, impl_->cache);
}
std::string Environment::render_string(
	std::string const &source,
	json::NodeRef ctx) const {
	return render_string(source, node_to_tmpl(ctx));
}

} // namespace conflux::templates
