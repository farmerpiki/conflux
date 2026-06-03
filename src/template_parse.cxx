module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

using conflux::utils::trim;

static std::vector<CompiledExpr> compile_expr_list(
	std::vector<std::string> const &exprs) {
	std::vector<CompiledExpr> out;
	out.reserve(exprs.size());
	std::ranges::transform(exprs, std::back_inserter(out), compile_expr);
	return out;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class TokenType : std::uint8_t {
	Text,
	Expr,
	Tag,
	Comment,
};
struct Token {
	TokenType type;
	std::string content;
};
struct ParseState {
	std::vector<Token> const &tokens;
	std::size_t pos = 0;
	[[nodiscard]] Token const &cur() const { return tokens[pos]; }
	[[nodiscard]] bool done() const { return pos >= tokens.size(); }
	void advance() { ++pos; }
};
using ParseNodesFn = std::function<NodeList(std::vector<std::string> const &, int)>;
static std::vector<Token> tokenize(
	std::string const &source) {
	std::vector<Token> tokens;
	std::size_t pos = 0;

	while (pos < source.size()) {
		auto next_expr = source.find("{{", pos);
		auto next_tag = source.find("{%", pos);
		auto next_comment = source.find("{#", pos);

		auto next = std::min({next_expr, next_tag, next_comment});
		if (next == std::string::npos) {
			tokens.push_back({TokenType::Text, source.substr(pos)});
			break;
		}

		if (next > pos) {
			tokens.push_back({TokenType::Text, source.substr(pos, next - pos)});
		}

		if (next == next_expr) {
			auto end = source.find("}}", next + 2);
			if (end == std::string::npos) {
				tokens.push_back({TokenType::Text, source.substr(next)});
				break;
			}
			tokens.push_back({TokenType::Expr, std::string{trim(source.substr(next + 2, end - next - 2))}});
			pos = end + 2;
		} else if (next == next_tag) {
			auto end = source.find("%}", next + 2);
			if (end == std::string::npos) {
				tokens.push_back({TokenType::Text, source.substr(next)});
				break;
			}
			auto content = source.substr(next + 2, end - next - 2);
			bool const trim_left = !content.empty() && content.front() == '-';
			bool const trim_right = !content.empty() && content.back() == '-';
			if (trim_left) {
				content = content.substr(1);
			}
			if (trim_right) {
				content = content.substr(0, content.size() - 1);
			}
			if (trim_left && !tokens.empty() && tokens.back().type == TokenType::Text) {
				auto &t = tokens.back().content;
				while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\n' || t.back() == '\r')) {
					t.pop_back();
				}
			}
			tokens.push_back({TokenType::Tag, std::string{trim(content)}});
			pos = end + 2;
			if (trim_right) {
				while (pos < source.size()
					   && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' || source[pos] == '\r')) {
					++pos;
				}
			}
		} else {
			auto end = source.find("#}", next + 2);
			if (end == std::string::npos) {
				tokens.push_back({TokenType::Text, source.substr(next)});
				break;
			}
			tokens.push_back({TokenType::Comment, source.substr(next + 2, end - next - 2)});
			pos = end + 2;
		}
	}
	return tokens;
}
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool starts_with(
	std::string const &s,
	char const *prefix) {
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}
static std::string extract_string_arg(
	std::string const &tag) {
	auto q1 = tag.find('"');
	if (q1 != std::string::npos) {
		auto q2 = tag.find('"', q1 + 1);
		if (q2 != std::string::npos) {
			return tag.substr(q1 + 1, q2 - q1 - 1);
		}
	}
	q1 = tag.find('\'');
	if (q1 != std::string::npos) {
		auto q2 = tag.find('\'', q1 + 1);
		if (q2 != std::string::npos) {
			return tag.substr(q1 + 1, q2 - q1 - 1);
		}
	}
	auto sp = tag.find(' ');
	return sp != std::string::npos ? std::string{trim(tag.substr(sp + 1))} : "";
}
struct ParsedForTag {
	std::vector<std::string> vars;
	std::string iter_expr;
};
static ParsedForTag parse_for_tag(
	std::string const &tag) {
	auto in_pos = tag.find(" in ");
	if (in_pos == std::string::npos) {
		throw std::runtime_error{"template parse error: missing 'in' in for tag"};
	}
	auto var_part = std::string{trim(tag.substr(4, in_pos - 4))};
	auto iter_expr = std::string{trim(tag.substr(in_pos + 4))};
	std::vector<std::string> vars;
	std::string_view vp{var_part};
	while (!vp.empty()) {
		auto cp = vp.find(',');
		auto vtok = (cp == std::string_view::npos) ? vp : vp.substr(0, cp);
		vars.push_back(std::string{trim(vtok)});
		if (cp == std::string_view::npos) {
			break;
		}
		vp.remove_prefix(cp + 1);
	}
	if (vars.empty()) {
		vars.push_back(var_part);
	}
	return {
		.vars = std::move(vars),
		.iter_expr = std::move(iter_expr),
	};
}
struct ParsedSetTag {
	std::string var;
	std::string expr;
};
static ParsedSetTag parse_set_tag(
	std::string const &tag) {
	auto eq = tag.find('=');
	if (eq == std::string::npos) {
		throw std::runtime_error{std::format("template parse error: set tag missing '=': {}", tag)};
	}
	return {
		.var = std::string{trim(tag.substr(4, eq - 4))},
		.expr = std::string{trim(tag.substr(eq + 1))},
	};
}
struct ParsedMacroTag {
	std::string name;
	std::vector<std::string> params;
	std::vector<std::string> defaults;
};
static ParsedMacroTag parse_macro_tag(
	std::string const &tag) {
	auto paren = tag.find('(');
	ParsedMacroTag parsed;
	if (paren != std::string::npos) {
		parsed.name = std::string{trim(tag.substr(6, paren - 6))};
		auto close = tag.find(')', paren);
		if (close != std::string::npos) {
			auto raw = split_args(tag.substr(paren + 1, close - paren - 1));
			for (auto &p: raw) {
				auto eq = p.find('=');
				if (eq != std::string::npos) {
					parsed.params.push_back(std::string{trim(p.substr(0, eq))});
					parsed.defaults.push_back(std::string{trim(p.substr(eq + 1))});
				} else {
					parsed.params.push_back(std::string{trim(p)});
					parsed.defaults.push_back("");
				}
			}
		}
	} else {
		parsed.name = std::string{trim(tag.substr(6))};
	}
	return parsed;
}
static FromImportNode parse_from_import_tag(
	std::string const &tag) {
	auto rest = std::string{trim(tag.substr(5))};
	std::string file;
	if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
		char const qc = rest.front();
		auto end = rest.find(qc, 1);
		if (end != std::string::npos) {
			file = rest.substr(1, end - 1);
			rest = std::string{trim(rest.substr(end + 1))};
		}
	}
	if (starts_with(rest, "import ")) {
		rest = std::string{trim(rest.substr(7))};
	}
	std::string nm;
	std::string alias;
	auto as_pos = rest.find(" as ");
	if (as_pos != std::string::npos) {
		nm = std::string{trim(rest.substr(0, as_pos))};
		alias = std::string{trim(rest.substr(as_pos + 4))};
	} else {
		nm = std::string{trim(rest)};
		alias = nm;
	}
	return {
		.file = std::move(file),
		.name = std::move(nm),
		.alias = std::move(alias),
	};
}
static NodePtr parse_block_node(
	std::string const &tag,
	ParseState &state,
	ParseNodesFn const &parse_nodes,
	Template &tmpl,
	int depth) {
	auto block_name = std::string{trim(tag.substr(6))};
	state.advance();
	auto body = parse_nodes({"endblock"}, depth + 1);
	state.advance();
	tmpl.blocks[block_name] = body;
	return std::make_shared<Node>(BlockNode{block_name, body});
}
static NodePtr parse_for_node(
	std::string const &tag,
	ParseState &state,
	ParseNodesFn const &parse_nodes,
	int depth) {
	auto parsed = parse_for_tag(tag);
	state.advance();
	auto body = parse_nodes({"endfor"}, depth + 1);
	state.advance();
	return std::make_shared<Node>(ForNode{parsed.vars, parsed.iter_expr, compile_expr(parsed.iter_expr), body});
}
static NodePtr parse_if_node(
	std::string const &tag,
	ParseState &state,
	ParseNodesFn const &parse_nodes,
	int depth) {
	IfNode if_node;
	auto cond = std::string{trim(tag.substr(3))};
	state.advance();
	auto body = parse_nodes({"elif", "else", "endif"}, depth + 1);
	if_node.branches.push_back({cond, compile_expr(cond), body});

	while (!state.done()) {
		auto &t = state.cur().content;
		if (t == "endif") {
			state.advance();
			break;
		}
		if (starts_with(t, "elif ")) {
			auto c = std::string{trim(t.substr(5))};
			state.advance();
			auto b = parse_nodes({"elif", "else", "endif"}, depth + 1);
			if_node.branches.push_back({c, compile_expr(c), b});
		} else if (t == "else") {
			state.advance();
			auto b = parse_nodes({"endif"}, depth + 1);
			if_node.branches.push_back({"", {}, b});
			state.advance();
			break;
		} else {
			break;
		}
	}
	return std::make_shared<Node>(Node{if_node});
}
static NodePtr parse_macro_node(
	std::string const &tag,
	ParseState &state,
	ParseNodesFn const &parse_nodes,
	int depth) {
	auto parsed = parse_macro_tag(tag);
	state.advance();
	auto body = parse_nodes({"endmacro"}, depth + 1);
	state.advance();
	return std::make_shared<Node>(
		MacroNode{parsed.name, parsed.params, parsed.defaults, compile_expr_list(parsed.defaults), body});
}
// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

Template Environment::Impl::parse(
	std::string const &name,
	std::string const &source) const {
	auto tokens = tokenize(source);
	Template tmpl;
	tmpl.name = name;
	ParseState state{tokens};

	ParseNodesFn parse_nodes;
	parse_nodes = [&](std::vector<std::string> const &end_tags, int depth) -> NodeList {
		if (depth > kMaxTemplateDepth) {
			throw std::runtime_error{"template parse recursion depth exceeded"};
		}
		NodeList nodes;
		auto const fail_missing_end = [&] {
			if (!end_tags.empty()) {
				throw std::runtime_error{
					std::format("template parse error: missing end tag (std::expected one of '{}')", end_tags.front())};
			}
		};
		while (!state.done()) {
			auto &tok = state.cur();

			if (tok.type == TokenType::Text) {
				nodes.push_back(std::make_shared<Node>(Node{TextNode{tok.content}}));
				state.advance();
				continue;
			}
			if (tok.type == TokenType::Comment) {
				state.advance();
				continue;
			}
			if (tok.type == TokenType::Expr) {
				nodes.push_back(
					std::make_shared<Node>(Node{
						ExprNode{tok.content, compile_expr(tok.content)}
                }));
				state.advance();
				continue;
			}

			auto &tag = tok.content;

			for (auto &et: end_tags) {
				if (tag == et || starts_with(tag, (et + " ").c_str())) {
					return nodes;
				}
			}

			if (starts_with(tag, "extends ")) {
				tmpl.extends_name = extract_string_arg(tag);
				nodes.push_back(std::make_shared<Node>(Node{ExtendsNode{tmpl.extends_name}}));
				state.advance();
			} else if (starts_with(tag, "block ")) {
				nodes.push_back(parse_block_node(tag, state, parse_nodes, tmpl, depth));
			} else if (starts_with(tag, "for ")) {
				nodes.push_back(parse_for_node(tag, state, parse_nodes, depth));
			} else if (starts_with(tag, "if ")) {
				nodes.push_back(parse_if_node(tag, state, parse_nodes, depth));
			} else if (starts_with(tag, "set ")) {
				auto parsed = parse_set_tag(tag);
				nodes.push_back(
					std::make_shared<Node>(Node{
						SetNode{parsed.var, parsed.expr, compile_expr(parsed.expr)}
                }));
				state.advance();
			} else if (starts_with(tag, "include ")) {
				auto inc_name = extract_string_arg(tag);
				nodes.push_back(std::make_shared<Node>(Node{IncludeNode{inc_name}}));
				state.advance();
			} else if (starts_with(tag, "macro ")) {
				nodes.push_back(parse_macro_node(tag, state, parse_nodes, depth));
			} else if (starts_with(tag, "from ")) {
				nodes.push_back(std::make_shared<Node>(Node{parse_from_import_tag(tag)}));
				state.advance();
			} else {
				throw std::runtime_error{std::format("template parse error: unknown tag '{}'", tag)};
			}
		}
		fail_missing_end();
		return nodes;
	};

	tmpl.nodes = parse_nodes({}, 0);
	return tmpl;
}

} // namespace conflux::templates
