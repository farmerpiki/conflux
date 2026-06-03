module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

std::string Environment::Impl::render_expr_node(
	ExprNode const &node,
	TmplValue &context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	std::unordered_map<std::string, MacroBinding> *macros,
	int depth) const {
	if (node.compiled.macro_call && macros) {
		auto const &call = *node.compiled.macro_call;
		auto it = macros->find(call.name);
		if (it != macros->end()) {
			std::vector<CompiledExprPtr> pos_args;
			std::unordered_map<std::string, CompiledExprPtr> kw_args;
			pos_args.reserve(call.args.size());
			for (auto const &arg: call.args) {
				if (arg.keyword) {
					kw_args[arg.name] = arg.compiled;
				} else {
					pos_args.push_back(arg.compiled);
				}
			}
			auto &[params, defaults, body] = it->second;
			auto saved = save_scope(context, params);
			for (std::size_t i = 0; i < params.size(); ++i) {
				if (i < pos_args.size() && pos_args[i]) {
					context.set(params[i], eval_expr(*pos_args[i], context));
				} else if (auto kit = kw_args.find(params[i]); kit != kw_args.end() && kit->second) {
					context.set(params[i], eval_expr(*kit->second, context));
				} else if (i < defaults.size() && !defaults[i].base.empty()) {
					context.set(params[i], eval_expr(defaults[i], context));
				} else {
					context.set(params[i], TmplValue{});
				}
			}
			auto rendered = render_nodes(body, context, active_cache, blocks, macros, depth + 1);
			restore_scope(context, saved);
			return rendered;
		}
	}
	return value_to_string(eval_expr(node.compiled, context));
}

std::string Environment::Impl::render_block_node(
	BlockNode const &node,
	TmplValue context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	std::unordered_map<std::string, MacroBinding> *macros,
	int depth) const {
	if (blocks) {
		auto it = blocks->find(node.name);
		if (it != blocks->end()) {
			return render_nodes(it->second, std::move(context), active_cache, blocks, macros, depth + 1);
		}
	}
	return render_nodes(node.body, std::move(context), active_cache, blocks, macros, depth + 1);
}

std::string Environment::Impl::render_include_node(
	IncludeNode const &node,
	TmplValue context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	int depth) const {
	auto it = active_cache.find(node.name);
	if (it == active_cache.end()) {
		throw std::runtime_error{std::format("template error: included template '{}' not found", node.name)};
	}
	return render_template(it->second, std::move(context), active_cache, blocks, depth + 1);
}

void Environment::Impl::render_set_node(
	SetNode const &node,
	TmplValue &context) const {
	auto val = eval_expr(node.compiled, context);
	if (context.is_object()) {
		context.set(node.var, std::move(val));
	}
}

std::string Environment::Impl::render_for_node(
	ForNode const &node,
	TmplValue &context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	std::unordered_map<std::string, MacroBinding> *macros,
	int depth) const {
	auto iter_val = eval_expr(node.compiled_iter, context);
	if (!iter_val.is_array()) {
		return {};
	}
	std::string out;
	auto saved = save_scope(context, node.vars);
	auto const *prev_loop = obj_find(context, "loop");
	std::optional<TmplValue> saved_loop = prev_loop ? std::optional<TmplValue>{*prev_loop} : std::nullopt;
	auto const &arr = iter_val.as_array();
	for (std::size_t i = 0; i < arr.size(); ++i) {
		if (node.vars.size() == 1) {
			context.set(node.vars[0], arr[i]);
		} else {
			auto const &item = arr[i];
			for (std::size_t j = 0; j < node.vars.size(); ++j) {
				if (item.is_array() && j < item.as_array().size()) {
					context.set(node.vars[j], item.as_array()[j]);
				} else {
					context.set(node.vars[j], TmplValue{});
				}
			}
		}
		TmplValue loop_obj{TmplValue::Object{}};
		loop_obj.set("index0", TmplValue{static_cast<std::int64_t>(i)});
		loop_obj.set("index", TmplValue{static_cast<std::int64_t>(i + 1)});
		loop_obj.set("first", TmplValue{i == 0});
		loop_obj.set("last", TmplValue{i == arr.size() - 1});
		loop_obj.set("length", TmplValue{static_cast<std::int64_t>(arr.size())});
		context.set("loop", std::move(loop_obj));
		out += render_nodes(node.body, context, active_cache, blocks, macros, depth + 1);
	}
	restore_scope(context, saved);
	if (saved_loop) {
		context.set("loop", *saved_loop);
	} else {
		context.erase("loop");
	}
	return out;
}

std::string Environment::Impl::render_if_node(
	IfNode const &node,
	TmplValue context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	std::unordered_map<std::string, MacroBinding> *macros,
	int depth) const {
	for (auto &branch: node.branches) {
		if (branch.condition.empty()) {
			return render_nodes(branch.body, std::move(context), active_cache, blocks, macros, depth + 1);
		}
		if (is_truthy(eval_expr(branch.compiled_condition, context))) {
			return render_nodes(branch.body, std::move(context), active_cache, blocks, macros, depth + 1);
		}
	}
	return {};
}

void Environment::Impl::register_macro_node(
	MacroNode const &node,
	std::unordered_map<std::string, MacroBinding> &macros) {
	macros[node.name] = {node.params, node.compiled_defaults, node.body};
}

void Environment::Impl::import_macro_node(
	FromImportNode const &node,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, MacroBinding> &macros) {
	auto it_tmpl = active_cache.find(node.file);
	if (it_tmpl == active_cache.end()) {
		throw std::runtime_error{std::format("template error: imported file '{}' not found", node.file)};
	}
	bool found = false;
	for (auto &sub: it_tmpl->second.nodes) {
		std::visit(
			[&](auto &&sn) {
				using ST = std::decay_t<decltype(sn)>;
				if constexpr (std::is_same_v<ST, MacroNode>) {
					if (sn.name == node.name) {
						macros[node.alias] = {sn.params, sn.compiled_defaults, sn.body};
						found = true;
					}
				}
			},
			sub->data);
	}
	if (!found) {
		throw std::runtime_error{std::format("template error: macro '{}' not found in '{}'", node.name, node.file)};
	}
}

// NOLINTNEXTLINE(misc-no-recursion)
std::string Environment::Impl::render_nodes(
	NodeList const &nodes,
	TmplValue context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *blocks,
	std::unordered_map<std::string, MacroBinding> *macros,
	int depth) const {
	if (depth > kMaxTemplateDepth) {
		throw std::runtime_error{"template render recursion depth exceeded"};
	}
	std::string out;
	std::unordered_map<std::string, MacroBinding> local_macros;
	if (macros == nullptr) {
		macros = &local_macros;
	}

	for (auto &node: nodes) {
		std::visit(
			[&](auto &n) {
				using T = std::decay_t<decltype(n)>;

				if constexpr (std::is_same_v<T, TextNode>) {
					out += n.text;
				} else if constexpr (std::is_same_v<T, ExprNode>) {
					out += render_expr_node(n, context, active_cache, blocks, macros, depth);
				} else if constexpr (std::is_same_v<T, BlockNode>) {
					out += render_block_node(n, context, active_cache, blocks, macros, depth);
				} else if constexpr (std::is_same_v<T, ExtendsNode>) {
				} else if constexpr (std::is_same_v<T, IncludeNode>) {
					out += render_include_node(n, context, active_cache, blocks, depth);
				} else if constexpr (std::is_same_v<T, SetNode>) {
					render_set_node(n, context);
				} else if constexpr (std::is_same_v<T, ForNode>) {
					out += render_for_node(n, context, active_cache, blocks, macros, depth);
				} else if constexpr (std::is_same_v<T, IfNode>) {
					out += render_if_node(n, context, active_cache, blocks, macros, depth);
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					register_macro_node(n, *macros);
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					import_macro_node(n, active_cache, *macros);
				}
			},
			node->data);
	}
	return out;
}
// NOLINTNEXTLINE(misc-no-recursion)
std::string Environment::Impl::render_template(
	Template const &tmpl,
	TmplValue context,
	std::unordered_map<std::string, Template> const &active_cache,
	std::unordered_map<std::string, NodeList> const *child_blocks,
	int depth) const {
	if (depth > kMaxTemplateDepth) {
		throw std::runtime_error{"template render recursion depth exceeded"};
	}
	if (!tmpl.extends_name.empty()) {
		auto it = active_cache.find(tmpl.extends_name);
		if (it == active_cache.end()) {
			throw std::runtime_error{"template not found: " + tmpl.extends_name};
		}

		for (auto &node: tmpl.nodes) {
			std::visit(
				[&](auto &n) {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, SetNode>) {
						auto val = eval_expr(n.compiled, context);
						if (context.is_object()) {
							context.set(n.var, std::move(val));
						}
					}
				},
				node->data);
		}

		auto merged = it->second.blocks;
		for (auto &[name, body]: tmpl.blocks) {
			merged[name] = body;
		}
		if (child_blocks != nullptr) {
			for (auto &[name, body]: *child_blocks) {
				merged[name] = body;
			}
		}

		return render_template(it->second, std::move(context), active_cache, &merged, depth + 1);
	}

	return render_nodes(tmpl.nodes, std::move(context), active_cache, child_blocks, nullptr, depth);
}

} // namespace conflux::templates
