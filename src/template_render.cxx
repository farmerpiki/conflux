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
					bool macro_handled = false;
					if (n.compiled.macro_call && macros) {
						auto const &call = *n.compiled.macro_call;
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
							out += render_nodes(body, context, active_cache, blocks, macros, depth + 1);
							restore_scope(context, saved);
							macro_handled = true;
						}
					}
					if (!macro_handled) {
						out += value_to_string(eval_expr(n.compiled, context));
					}
				} else if constexpr (std::is_same_v<T, BlockNode>) {
					if (blocks) {
						auto it = blocks->find(n.name);
						if (it != blocks->end()) {
							out += render_nodes(it->second, context, active_cache, blocks, macros, depth + 1);
							return;
						}
					}
					out += render_nodes(n.body, context, active_cache, blocks, macros, depth + 1);
				} else if constexpr (std::is_same_v<T, ExtendsNode>) {
					// handled at template level
				} else if constexpr (std::is_same_v<T, IncludeNode>) {
					auto it = active_cache.find(n.name);
					if (it == active_cache.end()) {
						throw std::runtime_error{
							std::format("template error: included template '{}' not found", n.name)};
					}
					out += render_template(it->second, context, active_cache, blocks, depth + 1);
				} else if constexpr (std::is_same_v<T, SetNode>) {
					auto val = eval_expr(n.compiled, context);
					if (context.is_object()) {
						context.set(n.var, std::move(val));
					}
				} else if constexpr (std::is_same_v<T, ForNode>) {
					auto iter_val = eval_expr(n.compiled_iter, context);
					if (iter_val.is_array()) {
						auto saved = save_scope(context, n.vars);
						auto const *prev_loop = obj_find(context, "loop");
						std::optional<TmplValue> saved_loop =
							prev_loop ? std::optional<TmplValue>{*prev_loop} : std::nullopt;
						auto const &arr = iter_val.as_array();
						for (std::size_t i = 0; i < arr.size(); ++i) {
							if (n.vars.size() == 1) {
								context.set(n.vars[0], arr[i]);
							} else {
								auto const &item = arr[i];
								for (std::size_t j = 0; j < n.vars.size(); ++j) {
									if (item.is_array() && j < item.as_array().size()) {
										context.set(n.vars[j], item.as_array()[j]);
									} else {
										context.set(n.vars[j], TmplValue{});
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
							out += render_nodes(n.body, context, active_cache, blocks, macros, depth + 1);
						}
						restore_scope(context, saved);
						if (saved_loop) {
							context.set("loop", *saved_loop);
						} else {
							context.erase("loop");
						}
					}
				} else if constexpr (std::is_same_v<T, IfNode>) {
					for (auto &branch: n.branches) {
						if (branch.condition.empty()) {
							out += render_nodes(branch.body, context, active_cache, blocks, macros, depth + 1);
							break;
						}
						if (is_truthy(eval_expr(branch.compiled_condition, context))) {
							out += render_nodes(branch.body, context, active_cache, blocks, macros, depth + 1);
							break;
						}
					}
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					(*macros)[n.name] = {n.params, n.compiled_defaults, n.body};
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					auto it_tmpl = active_cache.find(n.file);
					if (it_tmpl == active_cache.end()) {
						throw std::runtime_error{std::format("template error: imported file '{}' not found", n.file)};
					}
					bool found = false;
					for (auto &sub: it_tmpl->second.nodes) {
						std::visit(
							[&](auto &&sn) {
								using ST = std::decay_t<decltype(sn)>;
								if constexpr (std::is_same_v<ST, MacroNode>) {
									if (sn.name == n.name) {
										(*macros)[n.alias] = {sn.params, sn.compiled_defaults, sn.body};
										found = true;
									}
								}
							},
							sub->data);
					}
					if (!found) {
						throw std::runtime_error{
							std::format("template error: macro '{}' not found in '{}'", n.name, n.file)};
					}
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
