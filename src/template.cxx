module;
#include <memory>

export module conflux.templates;
import conflux.types;
import std.compat;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;
#if CONFLUX_HAS_FILE_WATCH
import conflux.file_watch;
#endif
export namespace tmpl {

struct Node;
using NodePtr = SP<Node>;
using NodeList = V<NodePtr>;
struct TextNode {
	S text;
};
struct ExprNode {
	S expr;
};
struct BlockNode {
	S name;
	NodeList body;
};
struct ExtendsNode {
	S parent;
};
struct IncludeNode {
	S name;
};
struct SetNode {
	S var;
	S expr;
};
struct ForNode {
	V<S> vars;
	S iter_expr;
	NodeList body;
};
struct IfNode {
	struct Branch {
		S condition;
		NodeList body;
	};
	V<Branch> branches;
};
struct MacroNode {
	S name;
	V<S> params;
	V<S> defaults;
	NodeList body;
};
struct FromImportNode {
	S file;
	S name;
	S alias;
};
struct Node {
	std::variant<
		TextNode,
		ExprNode,
		BlockNode,
		ExtendsNode,
		IncludeNode,
		SetNode,
		ForNode,
		IfNode,
		MacroNode,
		FromImportNode>
		data;
};
struct Template {
	S name;
	NodeList nodes;
	S extends_name;
	UM<S, NodeList> blocks;
};
struct EnvironmentOptions {
	bool watch_enabled = false;
	V<S> extensions{".html", ".htm", ".txt"};
};
class Environment {
public:
	explicit Environment(S const &template_dir);
	Environment(S const &template_dir, EnvironmentOptions options);
	~Environment();
	Environment(Environment &&) noexcept;
	Environment &operator =(Environment &&) noexcept;
	Environment(Environment const &) = delete;
	Environment &operator =(Environment const &) = delete;

	void load_all();
	[[nodiscard]] S render(S const &name, S const &json_ctx) const;
	[[nodiscard]] S render_string(S const &source, S const &json_ctx) const;

private:
	struct Impl;
	UP<Impl> impl_;
};

} // namespace tmpl
namespace tmpl {
namespace fs = std::filesystem;
// ---------------------------------------------------------------------------
// Internal mutable value type for template context
// ---------------------------------------------------------------------------

struct TmplValue {
	using Array = V<TmplValue>;
	using Object = V<P<S, TmplValue>>;

	variant<std::monostate, bool, i64, u64, double, S, Array, Object> data;

	TmplValue() = default;
	explicit TmplValue(
		bool b)
		: data(b) {}
	explicit TmplValue(
		i64 v)
		: data(v) {}
	explicit TmplValue(
		u64 v)
		: data(v) {}
	explicit TmplValue(
		double v)
		: data(v) {}
	explicit TmplValue(
		S s)
		: data(move(s)) {}
	explicit TmplValue(
		SV sv)
		: data(S{sv}) {}
	explicit TmplValue(
		Array a)
		: data(move(a)) {}
	explicit TmplValue(
		Object o)
		: data(move(o)) {}
	[[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
	[[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
	[[nodiscard]] bool is_int() const noexcept { return std::holds_alternative<i64>(data); }
	[[nodiscard]] bool is_uint() const noexcept { return std::holds_alternative<u64>(data); }
	[[nodiscard]] bool is_float() const noexcept { return std::holds_alternative<double>(data); }
	[[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<S>(data); }
	[[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
	[[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(data); }
	template<class T>
	[[nodiscard]] decltype(auto) as() const {
		if constexpr (same_as<T, SV>) {
			return SV{std::get<S>(data)};
		} else {
			return std::get<T>(data);
		}
	}
	[[nodiscard]] Array &as_array() { return std::get<Array>(data); }
	[[nodiscard]] Array const &as_array() const { return std::get<Array>(data); }
	[[nodiscard]] Object &as_object() { return std::get<Object>(data); }
	[[nodiscard]] Object const &as_object() const { return std::get<Object>(data); }
	void set(
		SV key,
		TmplValue val) {
		auto &obj = std::get<Object>(data);
		for (auto &[k, v]: obj) {
			if (k == key) {
				v = move(val);
				return;
			}
		}
		obj.emplace_back(S{key}, move(val));
	}
	void erase(
		SV key) {
		auto &obj = std::get<Object>(data);
		std::erase_if(obj, [key](auto const &p) { return p.first == key; });
	}
	void push_back(
		TmplValue val) {
		std::get<Array>(data).push_back(move(val));
	}
	[[nodiscard]] bool operator ==(TmplValue const &) const = default;

	[[nodiscard]] S dump() const;
};
// NOLINTNEXTLINE(misc-no-recursion)
S TmplValue::dump() const {
	if (is_null()) {
		return "null";
	}
	if (is_bool()) {
		return std::get<bool>(data) ? "true" : "false";
	}
	if (is_int()) {
		return to_string(std::get<i64>(data));
	}
	if (is_uint()) {
		return to_string(std::get<u64>(data));
	}
	if (is_float()) {
		auto s = to_string(std::get<double>(data));
		auto dot = s.find('.');
		if (dot != S::npos) {
			auto last = s.find_last_not_of('0');
			if (last != S::npos && last > dot) {
				s.erase(last + 1);
			}
			if (s.back() == '.') {
				s.pop_back();
			}
		}
		return s;
	}
	if (is_string()) {
		S out = "\"";
		for (char const c: std::get<S>(data)) {
			switch (c) {
			case '"' : out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default  : out += c;
			}
		}
		out += '"';
		return out;
	}
	if (is_array()) {
		S out = "[";
		bool first = true;
		for (auto const &v: std::get<Array>(data)) {
			if (!first) {
				out += ',';
			}
			first = false;
			out += v.dump();
		}
		out += ']';
		return out;
	}
	if (is_object()) {
		S out = "{";
		bool first = true;
		for (auto const &[k, v]: std::get<Object>(data)) {
			if (!first) {
				out += ',';
			}
			first = false;
			out += '"';
			out += k;
			out += "\":";
			out += v.dump();
		}
		out += '}';
		return out;
	}
	return "null";
}
// NOLINTNEXTLINE(misc-no-recursion)
static TmplValue node_to_tmpl(
	NodeRef node) {
	switch (node.kind()) {
	case JsonKind::null   : return {};
	case JsonKind::boolean: return TmplValue{*node.as_bool()};
	case JsonKind::number:
		{
			auto num = *node.as_number();
			if (num.form() == JsonNumberForm::integer) {
				if (!num.lexeme().empty() && num.lexeme()[0] == '-') {
					if (auto v = num.to_i64(); v) {
						return TmplValue{*v};
					}
				} else {
					if (auto v = num.to_i64(); v) {
						return TmplValue{*v};
					}
					if (auto v = num.to_u64(); v) {
						return TmplValue{*v};
					}
				}
			}
			if (auto v = num.to_f64(); v) {
				return TmplValue{*v};
			}
			return {};
		}
	case JsonKind::string: return TmplValue{S{*node.as_string()}};
	case JsonKind::array:
		{
			auto arr = *node.as_array();
			TmplValue::Array vec;
			vec.reserve(arr.size());
			for (auto elem: arr.elements()) {
				vec.push_back(node_to_tmpl(elem));
			}
			return TmplValue{move(vec)};
		}
	case JsonKind::object:
		{
			auto obj = *node.as_object();
			TmplValue::Object pairs;
			pairs.reserve(obj.size());
			for (auto [name, val]: obj.members()) {
				pairs.emplace_back(S{name}, node_to_tmpl(val));
			}
			return TmplValue{move(pairs)};
		}
	default: return {};
	}
}
// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

static S trim(
	SV s) {
	while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
		s.remove_suffix(1);
	}
	return S(s);
}
static S str_replace_all(
	SV src,
	SV old_s,
	SV new_s) {
	S out;
	if (old_s.empty()) {
		out.assign(src);
		return out;
	}
	out.reserve(src.size());
	SZ p = 0;
	while (p < src.size()) {
		auto f = src.find(old_s, p);
		if (f == SV::npos) {
			out.append(src.substr(p));
			break;
		}
		out.append(src.substr(p, f - p));
		out.append(new_s);
		p = f + old_s.size();
	}
	return out;
}
static S str_capitalize(
	S s) {
	if (!s.empty()) {
		s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	}
	return s;
}
static V<S> split_args(
	S const &s) {
	V<S> args;
	S current;
	int depth = 0;
	bool in_str = false;
	char str_char = 0;
	for (SZ i = 0; i < s.size(); ++i) {
		char const c = s[i];
		if (in_str) {
			current += c;
			if (c == str_char && (i == 0 || s[i - 1] != '\\')) {
				in_str = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_str = true;
			str_char = c;
			current += c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			++depth;
			current += c;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			--depth;
			current += c;
			continue;
		}
		if (c == ',' && depth == 0) {
			args.push_back(trim(current));
			current.clear();
			continue;
		}
		current += c;
	}
	if (!current.empty()) {
		args.push_back(trim(current));
	}
	return args;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class TokenType : u8 {
	Text,
	Expr,
	Tag,
	Comment,
};
struct Token {
	TokenType type;
	S content;
};
static V<Token> tokenize(
	S const &source) {
	V<Token> tokens;
	SZ pos = 0;

	while (pos < source.size()) {
		auto next_expr = source.find("{{", pos);
		auto next_tag = source.find("{%", pos);
		auto next_comment = source.find("{#", pos);

		auto next = min({next_expr, next_tag, next_comment});
		if (next == S::npos) {
			tokens.push_back({TokenType::Text, source.substr(pos)});
			break;
		}

		if (next > pos) {
			tokens.push_back({TokenType::Text, source.substr(pos, next - pos)});
		}

		if (next == next_expr) {
			auto end = source.find("}}", next + 2);
			if (end == S::npos) {
				tokens.push_back({TokenType::Text, source.substr(next)});
				break;
			}
			tokens.push_back({TokenType::Expr, trim(source.substr(next + 2, end - next - 2))});
			pos = end + 2;
		} else if (next == next_tag) {
			auto end = source.find("%}", next + 2);
			if (end == S::npos) {
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
			tokens.push_back({TokenType::Tag, trim(content)});
			pos = end + 2;
			if (trim_right) {
				while (pos < source.size()
					   && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' || source[pos] == '\r')) {
					++pos;
				}
			}
		} else {
			auto end = source.find("#}", next + 2);
			if (end == S::npos) {
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
	S const &s,
	char const *prefix) {
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}
static S extract_string_arg(
	S const &tag) {
	auto q1 = tag.find('"');
	if (q1 != S::npos) {
		auto q2 = tag.find('"', q1 + 1);
		if (q2 != S::npos) {
			return tag.substr(q1 + 1, q2 - q1 - 1);
		}
	}
	q1 = tag.find('\'');
	if (q1 != S::npos) {
		auto q2 = tag.find('\'', q1 + 1);
		if (q2 != S::npos) {
			return tag.substr(q1 + 1, q2 - q1 - 1);
		}
	}
	auto sp = tag.find(' ');
	return sp != S::npos ? trim(tag.substr(sp + 1)) : "";
}
static TmplValue const *obj_find(
	TmplValue const &obj,
	SV key) {
	for (auto const &kv: obj.as_object()) {
		if (kv.first == key) {
			return &kv.second;
		}
	}
	return nullptr;
}
static V<P<S, Opt<TmplValue>>> save_scope(
	TmplValue const &ctx,
	span<S const> names) {
	V<P<S, Opt<TmplValue>>> saved;
	saved.reserve(names.size());
	for (auto const &n: names) {
		auto const *prev = obj_find(ctx, n);
		saved.emplace_back(n, (prev != nullptr) ? Opt<TmplValue>{*prev} : nullopt);
	}
	return saved;
}
static void restore_scope(
	TmplValue &ctx,
	V<P<S, Opt<TmplValue>>> const &saved) {
	for (auto const &[k, v]: saved) {
		if (v) {
			ctx.set(k, *v);
		} else {
			ctx.erase(k);
		}
	}
}
// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Environment::Impl {
	S template_dir;
	EnvironmentOptions options;
	UM<S, Template> cache;
	mutable std::shared_mutex cache_mtx;
#if CONFLUX_HAS_FILE_WATCH
	mutable UP<FileWatcher> watcher;
	mutable Atom<bool> watch_started{false};
#endif

	Template parse(S const &name, S const &source) const;
	TmplValue eval_expr(S const &expr, TmplValue const &context) const;
	TmplValue apply_filter(S const &name, TmplValue const &val, V<S> const &args, TmplValue const &context) const;
	static S value_to_string(TmplValue const &v);
	static bool is_truthy(TmplValue const &v);
	static constexpr int kMaxTemplateDepth = 256;
	S render_nodes(
		NodeList const &nodes,
		TmplValue context,
		UM<S, NodeList> const *blocks,
		UM<S, Tup<V<S>, V<S>, NodeList>> *macros,
		int depth = 0) const;
	S render_template(
		Template const &tmpl,
		TmplValue context,
		UM<S, NodeList> const *child_blocks = nullptr,
		int depth = 0) const;
	void reload_path(S const &path);
	void remove_path(S const &path);
#if CONFLUX_HAS_FILE_WATCH
	void maybe_start_watcher() const;
#endif
	bool extension_allowed(fs::path const &path) const;
};
// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

Template Environment::Impl::parse(
	S const &name,
	S const &source) const {
	auto tokens = tokenize(source);
	Template tmpl;
	tmpl.name = name;
	struct ParseState {
		V<Token> const &tokens;
		SZ pos = 0;
		[[nodiscard]] Token const &cur() const { return tokens[pos]; }
		[[nodiscard]] bool done() const { return pos >= tokens.size(); }
		void advance() { ++pos; }
	};
	ParseState state{tokens};

	Fn<NodeList(V<S> const &, int)> parse_nodes;
	parse_nodes = [&](V<S> const &end_tags, int depth) -> NodeList {
		if (depth > kMaxTemplateDepth) {
			throw RE{"template parse recursion depth exceeded"};
		}
		NodeList nodes;
		auto const fail_missing_end = [&] {
			if (!end_tags.empty()) {
				throw RE{format("template parse error: missing end tag (expected one of '{}')", end_tags.front())};
			}
		};
		while (!state.done()) {
			auto &tok = state.cur();

			if (tok.type == TokenType::Text) {
				nodes.push_back(make_shared<Node>(Node{TextNode{tok.content}}));
				state.advance();
				continue;
			}
			if (tok.type == TokenType::Comment) {
				state.advance();
				continue;
			}
			if (tok.type == TokenType::Expr) {
				nodes.push_back(make_shared<Node>(Node{ExprNode{tok.content}}));
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
				nodes.push_back(make_shared<Node>(Node{ExtendsNode{tmpl.extends_name}}));
				state.advance();
			} else if (starts_with(tag, "block ")) {
				auto block_name = trim(tag.substr(6));
				state.advance();
				auto body = parse_nodes({"endblock"}, depth + 1);
				state.advance();
				tmpl.blocks[block_name] = body;
				nodes.push_back(
					make_shared<Node>(Node{
						BlockNode{block_name, body}
                }));
			} else if (starts_with(tag, "for ")) {
				auto in_pos = tag.find(" in ");
				if (in_pos == S::npos) {
					throw RE{"template parse error: missing 'in' in for tag"};
				}
				auto var_part = trim(tag.substr(4, in_pos - 4));
				auto iter_expr = trim(tag.substr(in_pos + 4));
				V<S> vars;
				SV vp{var_part};
				while (!vp.empty()) {
					auto cp = vp.find(',');
					auto vtok = (cp == SV::npos) ? vp : vp.substr(0, cp);
					vars.push_back(trim(S{vtok}));
					if (cp == SV::npos) {
						break;
					}
					vp.remove_prefix(cp + 1);
				}
				if (vars.empty()) {
					vars.push_back(var_part);
				}
				state.advance();
				auto body = parse_nodes({"endfor"}, depth + 1);
				state.advance();
				nodes.push_back(
					make_shared<Node>(Node{
						ForNode{vars, iter_expr, body}
                }));
			} else if (starts_with(tag, "if ")) {
				IfNode if_node;
				auto cond = trim(tag.substr(3));
				state.advance();
				auto body = parse_nodes({"elif", "else", "endif"}, depth + 1);
				if_node.branches.push_back({cond, body});

				while (!state.done()) {
					auto &t = state.cur().content;
					if (t == "endif") {
						state.advance();
						break;
					}
					if (starts_with(t, "elif ")) {
						auto c = trim(t.substr(5));
						state.advance();
						auto b = parse_nodes({"elif", "else", "endif"}, depth + 1);
						if_node.branches.push_back({c, b});
					} else if (t == "else") {
						state.advance();
						auto b = parse_nodes({"endif"}, depth + 1);
						if_node.branches.push_back({"", b});
						state.advance();
						break;
					} else {
						break;
					}
				}
				nodes.push_back(make_shared<Node>(Node{if_node}));
			} else if (starts_with(tag, "set ")) {
				auto eq = tag.find('=');
				if (eq == S::npos) {
					throw RE{format("template parse error: set tag missing '=': {}", tag)};
				}
				auto var = trim(tag.substr(4, eq - 4));
				auto expr = trim(tag.substr(eq + 1));
				nodes.push_back(
					make_shared<Node>(Node{
						SetNode{var, expr}
                }));
				state.advance();
			} else if (starts_with(tag, "include ")) {
				auto inc_name = extract_string_arg(tag);
				nodes.push_back(make_shared<Node>(Node{IncludeNode{inc_name}}));
				state.advance();
			} else if (starts_with(tag, "macro ")) {
				auto paren = tag.find('(');
				S mname;
				V<S> params;
				V<S> defaults;
				if (paren != S::npos) {
					mname = trim(tag.substr(6, paren - 6));
					auto close = tag.find(')', paren);
					if (close != S::npos) {
						auto raw = split_args(tag.substr(paren + 1, close - paren - 1));
						for (auto &p: raw) {
							auto eq = p.find('=');
							if (eq != S::npos) {
								params.push_back(trim(p.substr(0, eq)));
								defaults.push_back(trim(p.substr(eq + 1)));
							} else {
								params.push_back(trim(p));
								defaults.push_back("");
							}
						}
					}
				} else {
					mname = trim(tag.substr(6));
				}
				state.advance();
				auto body = parse_nodes({"endmacro"}, depth + 1);
				state.advance();
				nodes.push_back(
					make_shared<Node>(Node{
						MacroNode{mname, params, defaults, body}
                }));
			} else if (starts_with(tag, "from ")) {
				auto rest = trim(tag.substr(5));
				S file;
				if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
					char const qc = rest.front();
					auto end = rest.find(qc, 1);
					if (end != S::npos) {
						file = rest.substr(1, end - 1);
						rest = trim(rest.substr(end + 1));
					}
				}
				if (starts_with(rest, "import ")) {
					rest = trim(rest.substr(7));
				}
				S nm, alias;
				auto as_pos = rest.find(" as ");
				if (as_pos != S::npos) {
					nm = trim(rest.substr(0, as_pos));
					alias = trim(rest.substr(as_pos + 4));
				} else {
					nm = trim(rest);
					alias = nm;
				}
				nodes.push_back(
					make_shared<Node>(Node{
						FromImportNode{file, nm, alias}
                }));
				state.advance();
			} else {
				throw RE{format("template parse error: unknown tag '{}'", tag)};
			}
		}
		fail_missing_end();
		return nodes;
	};

	tmpl.nodes = parse_nodes({}, 0);
	return tmpl;
}
// ---------------------------------------------------------------------------
// Expression evaluator
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
TmplValue Environment::Impl::eval_expr(
	S const &expr,
	TmplValue const &context) const {
	auto e = trim(expr);
	if (e.empty()) {
		return {};
	}

	V<S> pipe_parts;
	{
		S current;
		int depth = 0;
		bool in_str = false;
		char sc = 0;
		for (SZ i = 0; i < e.size(); ++i) {
			char const c = e[i];
			if (in_str) {
				current += c;
				if (c == sc && (i == 0 || e[i - 1] != '\\')) {
					in_str = false;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				in_str = true;
				sc = c;
				current += c;
				continue;
			}
			if (c == '(' || c == '[' || c == '{') {
				++depth;
				current += c;
				continue;
			}
			if (c == ')' || c == ']' || c == '}') {
				--depth;
				current += c;
				continue;
			}
			if (c == '|' && depth == 0) {
				pipe_parts.push_back(trim(current));
				current.clear();
				continue;
			}
			current += c;
		}
		pipe_parts.push_back(trim(current));
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	auto eval_base = [&](S const &base) -> TmplValue {
		auto b = trim(base);
		if (b.empty()) {
			return {};
		}

		if ((b.front() == '"' && b.back() == '"') || (b.front() == '\'' && b.back() == '\'')) {
			return TmplValue{b.substr(1, b.size() - 2)};
		}

		if (std::isdigit(static_cast<unsigned char>(b[0])) || (b[0] == '-' && b.size() > 1)) {
			try {
				if (b.find('.') != S::npos) {
					return TmplValue{std::stod(b)};
				}
				return TmplValue{static_cast<i64>(std::stoll(b))};
			} catch (exception const &ex) {
				eprintln(format("template eval_literal: failed to parse number '{}': {}", b, ex.what()));
			}
		}

		if (b == "true" || b == "True") {
			return TmplValue{true};
		}
		if (b == "false" || b == "False") {
			return TmplValue{false};
		}
		if (b == "none" || b == "None") {
			return {};
		}

		if (b.front() == '[' && b.back() == ']') {
			auto inner = trim(b.substr(1, b.size() - 2));
			if (inner.empty()) {
				return TmplValue{TmplValue::Array{}};
			}
			auto items = split_args(inner);
			TmplValue arr{TmplValue::Array{}};
			for (auto &item: items) {
				arr.push_back(eval_expr(item, context));
			}
			return arr;
		}

		if (b.front() == '(' && b.back() == ')') {
			auto inner = trim(b.substr(1, b.size() - 2));
			if (inner.empty()) {
				return TmplValue{TmplValue::Array{}};
			}
			auto items = split_args(inner);
			if (items.size() == 1) {
				return eval_expr(items[0], context);
			}
			TmplValue arr{TmplValue::Array{}};
			for (auto &item: items) {
				arr.push_back(eval_expr(item, context));
			}
			return arr;
		}

		if (b.front() == '{' && b.back() == '}') {
			auto inner = trim(b.substr(1, b.size() - 2));
			if (inner.empty()) {
				return TmplValue{TmplValue::Object{}};
			}
			auto pairs = split_args(inner);
			TmplValue obj{TmplValue::Object{}};
			for (auto &p: pairs) {
				auto colon = p.find(':');
				if (colon != S::npos) {
					auto key = trim(p.substr(0, colon));
					auto val = trim(p.substr(colon + 1));
					if ((key.front() == '"' && key.back() == '"') || (key.front() == '\'' && key.back() == '\'')) {
						key = key.substr(1, key.size() - 2);
					}
					obj.set(key, eval_expr(val, context));
				}
			}
			return obj;
		}

		{
			int depth = 0;
			bool in_s = false;
			char sqc = 0;
			for (SZ i = 0; i < b.size(); ++i) {
				char const c = b[i];
				if (in_s) {
					if (c == sqc) {
						in_s = false;
					}
					continue;
				}
				if (c == '"' || c == '\'') {
					in_s = true;
					sqc = c;
					continue;
				}
				if (c == '(' || c == '[') {
					++depth;
					continue;
				}
				if (c == ')' || c == ']') {
					--depth;
					continue;
				}
				if (depth == 0 && i + 4 <= b.size() && b.substr(i, 4) == " or ") {
					auto left = eval_expr(b.substr(0, i), context);
					if (is_truthy(left)) {
						return left;
					}
					return eval_expr(b.substr(i + 4), context);
				}
			}
		}

		{
			int depth = 0;
			bool in_s = false;
			char sqc = 0;
			for (SZ i = 0; i < b.size(); ++i) {
				char const c = b[i];
				if (in_s) {
					if (c == sqc) {
						in_s = false;
					}
					continue;
				}
				if (c == '"' || c == '\'') {
					in_s = true;
					sqc = c;
					continue;
				}
				if (c == '(' || c == '[') {
					++depth;
					continue;
				}
				if (c == ')' || c == ']') {
					--depth;
					continue;
				}
				if (depth == 0 && i + 5 <= b.size() && b.substr(i, 5) == " and ") {
					auto left = eval_expr(b.substr(0, i), context);
					if (!is_truthy(left)) {
						return left;
					}
					return eval_expr(b.substr(i + 5), context);
				}
			}
		}

		if (b.size() > 4 && b.substr(0, 4) == "not ") {
			auto inner_val = eval_expr(b.substr(4), context);
			return TmplValue{!is_truthy(inner_val)};
		}

		{
			static V<P<S, int>> const ops = {
				{" == ", 0},
				{" != ", 1},
				{" <= ", 2},
				{" >= ", 3},
				{ " < ", 4},
				{ " > ", 5},
				{" in ", 6}
            };
			auto find_top_level = [&](SV haystack, SV needle) -> SZ {
				int d = 0;
				bool in_s3 = false;
				char sq3 = 0;
				for (SZ i = 0; i + needle.size() <= haystack.size(); ++i) {
					char const c3 = haystack[i];
					if (in_s3) {
						if (c3 == sq3) {
							in_s3 = false;
						}
						continue;
					}
					if (c3 == '"' || c3 == '\'') {
						in_s3 = true;
						sq3 = c3;
						continue;
					}
					if (c3 == '(' || c3 == '[') {
						++d;
						continue;
					}
					if (c3 == ')' || c3 == ']') {
						--d;
						continue;
					}
					if (d == 0 && haystack.substr(i, needle.size()) == needle) {
						return i;
					}
				}
				return SV::npos;
			};
			for (auto &[op, code]: ops) {
				auto p = find_top_level(b, op);
				if (p != SV::npos) {
					auto left = eval_expr(b.substr(0, p), context);
					auto right = eval_expr(b.substr(p + op.size()), context);
					switch (code) {
					case 0: return TmplValue{left == right};
					case 1: return TmplValue{left != right};
					case 2:
					case 3:
					case 4:
					case 5:
						{
							double const lv = left.is_int()   ? static_cast<double>(left.as<i64>()) :
											  left.is_uint()  ? static_cast<double>(left.as<u64>()) :
											  left.is_float() ? left.as<double>() :
																0.0;
							double const rv = right.is_int()   ? static_cast<double>(right.as<i64>()) :
											  right.is_uint()  ? static_cast<double>(right.as<u64>()) :
											  right.is_float() ? right.as<double>() :
																 0.0;
							if (code == 2) {
								return TmplValue{lv <= rv};
							}
							if (code == 3) {
								return TmplValue{lv >= rv};
							}
							if (code == 4) {
								return TmplValue{lv < rv};
							}
							return TmplValue{lv > rv};
						}
					case 6:
						{
							if (right.is_array()) {
								for (auto const &item: right.as_array()) {
									if (item == left) {
										return TmplValue{true};
									}
								}
								return TmplValue{false};
							}
							if (right.is_string() && left.is_string()) {
								return TmplValue{right.as<SV>().find(left.as<SV>()) != SV::npos};
							}
							return TmplValue{false};
						}
					default: break;
					}
				}
			}
		}

		{
			int depth = 0;
			bool in_s = false;
			char sqc = 0;
			for (SZ i = 0; i < b.size(); ++i) {
				char const c = b[i];
				if (in_s) {
					if (c == sqc) {
						in_s = false;
					}
					continue;
				}
				if (c == '"' || c == '\'') {
					in_s = true;
					sqc = c;
					continue;
				}
				if (c == '(' || c == '[') {
					++depth;
					continue;
				}
				if (c == ')' || c == ']') {
					--depth;
					continue;
				}
				if (depth == 0 && c == '~') {
					auto left = eval_expr(b.substr(0, i), context);
					auto right = eval_expr(b.substr(i + 1), context);
					return TmplValue{value_to_string(left) + value_to_string(right)};
				}
			}
		}

		{
			TmplValue owned;
			bool use_owned = false;
			TmplValue const *cur = &context;
			auto set_owned = [&](TmplValue v) {
				owned = move(v);
				cur = &owned;
				use_owned = true;
			};
			S remaining = b;

			while (!remaining.empty()) {
				auto bracket = remaining.find('[');
				auto dot = remaining.find('.');
				auto paren = remaining.find('(');

				auto next_sep = min({bracket, dot, paren, remaining.size()});

				if (next_sep == 0 && bracket == 0) {
					auto close = remaining.find(']', 1);
					if (close == S::npos) {
						return {};
					}
					auto idx_str = trim(remaining.substr(1, close - 1));
					if (auto colon = idx_str.find(':'); colon != S::npos) {
						if (cur->is_string()) {
							auto s = S(cur->as<SV>());
							auto start_s = trim(idx_str.substr(0, colon));
							auto end_s = trim(idx_str.substr(colon + 1));
							i64 start = 0;
							i64 end = static_cast<i64>(s.size());
							if (!start_s.empty()) {
								auto sv = eval_expr(start_s, context);
								if (sv.is_int()) {
									start = sv.as<i64>();
									if (start < 0) {
										start = max<i64>(0, static_cast<i64>(s.size()) + start);
									}
								}
							}
							if (!end_s.empty()) {
								auto ev = eval_expr(end_s, context);
								if (ev.is_int()) {
									end = ev.as<i64>();
									if (end < 0) {
										end = max<i64>(0, static_cast<i64>(s.size()) + end);
									}
								}
							}
							start = std::clamp<i64>(start, 0, static_cast<i64>(s.size()));
							end = std::clamp<i64>(end, 0, static_cast<i64>(s.size()));
							set_owned(
								TmplValue{s.substr(static_cast<SZ>(start), static_cast<SZ>(max<i64>(0, end - start)))});
						} else {
							return {};
						}
						remaining = remaining.substr(close + 1);
						if (!remaining.empty() && remaining[0] == '.') {
							remaining = remaining.substr(1);
						}
						continue;
					}
					auto idx_val = eval_expr(idx_str, context);
					if (cur->is_array() && idx_val.is_int()) {
						auto idx = idx_val.as<i64>();
						auto const &arr = cur->as_array();
						if (idx < 0) {
							idx += static_cast<i64>(arr.size());
						}
						if (idx >= 0 && static_cast<SZ>(idx) < arr.size()) {
							set_owned(arr[static_cast<SZ>(idx)]);
						} else {
							return {};
						}
					} else if (cur->is_object() && idx_val.is_string()) {
						auto const *found = obj_find(*cur, idx_val.as<SV>());
						if (found) {
							set_owned(*found);
						} else {
							return {};
						}
					} else {
						return {};
					}
					remaining = remaining.substr(close + 1);
					if (!remaining.empty() && remaining[0] == '.') {
						remaining = remaining.substr(1);
					}
					continue;
				}

				S const key = remaining.substr(0, next_sep);
				remaining = next_sep < remaining.size() ? remaining.substr(next_sep) : "";

				bool const is_method_call = !remaining.empty() && remaining[0] == '(';

				if (!key.empty() && !is_method_call) {
					if (cur->is_object()) {
						auto const *found = obj_find(*cur, key);
						if (found) {
							set_owned(*found);
						} else {
							return {};
						}
					} else if (cur->is_string() && key == "value") {
						// no-op
					} else {
						return {};
					}
				}

				if (is_method_call) {
					// Find matching ')' respecting nesting and S literals.
					SZ close = S::npos;
					{
						int d = 0;
						bool in_s2 = false;
						char sq2 = 0;
						for (SZ ci = 0; ci < remaining.size(); ++ci) {
							char const c2 = remaining[ci];
							if (in_s2) {
								if (c2 == sq2) {
									in_s2 = false;
								}
								continue;
							}
							if (c2 == '"' || c2 == '\'') {
								in_s2 = true;
								sq2 = c2;
								continue;
							}
							if (c2 == '(') {
								++d;
							} else if (c2 == ')') {
								--d;
								if (d == 0) {
									close = ci;
									break;
								}
							}
						}
					}
					if (close == S::npos) {
						return {};
					}
					auto args_str = remaining.substr(1, close - 1);
					auto method_args = split_args(args_str);
					remaining = remaining.substr(close + 1);
					if (!remaining.empty() && remaining[0] == '.') {
						remaining = remaining.substr(1);
					}

					if (key == "get" && cur->is_object()) {
						if (method_args.empty()) {
							return {};
						}
						auto k = eval_expr(method_args[0], context);
						auto const *found = obj_find(*cur, value_to_string(k));
						if (found) {
							set_owned(*found);
						} else if (method_args.size() > 1) {
							set_owned(eval_expr(method_args[1], context));
						} else {
							set_owned(TmplValue{});
						}
					} else if (key == "replace" && method_args.size() >= 2) {
						auto s = value_to_string(*cur);
						auto old_s = value_to_string(eval_expr(method_args[0], context));
						auto new_s = value_to_string(eval_expr(method_args[1], context));
						set_owned(TmplValue{str_replace_all(s, old_s, new_s)});
					} else if (key == "title") {
						auto s = value_to_string(*cur);
						bool up = true;
						for (auto &c: s) {
							if (std::isspace(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
								up = true;
								continue;
							}
							if (up) {
								c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
								up = false;
							} else {
								c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
							}
						}
						set_owned(TmplValue{move(s)});
					} else if (key == "upper") {
						auto s = value_to_string(*cur);
						for (auto &c: s) {
							c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
						}
						set_owned(TmplValue{move(s)});
					} else if (key == "lower") {
						auto s = value_to_string(*cur);
						for (auto &c: s) {
							c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
						}
						set_owned(TmplValue{move(s)});
					} else if (key == "capitalize") {
						set_owned(TmplValue{str_capitalize(value_to_string(*cur))});
					} else if (key == "strftime") {
						set_owned(TmplValue{value_to_string(*cur)});
					} else if (key == "strip") {
						set_owned(TmplValue{trim(value_to_string(*cur))});
					} else if (key == "startswith" && !method_args.empty()) {
						auto s = value_to_string(*cur);
						auto p = value_to_string(eval_expr(method_args[0], context));
						set_owned(TmplValue{s.compare(0, p.size(), p) == 0});
					} else if (key == "split") {
						auto s = value_to_string(*cur);
						auto sep = !method_args.empty() ? value_to_string(eval_expr(method_args[0], context)) : " ";
						TmplValue arr{TmplValue::Array{}};
						SZ p = 0;
						while (p <= s.size()) {
							auto f = sep.empty() ? S::npos : s.find(sep, p);
							if (f == S::npos) {
								arr.push_back(TmplValue{s.substr(p)});
								break;
							}
							arr.push_back(TmplValue{s.substr(p, f - p)});
							p = f + sep.size();
						}
						set_owned(move(arr));
					} else if (key == "keys" && cur->is_object()) {
						TmplValue keys{TmplValue::Array{}};
						for (auto const &kv: cur->as_object()) {
							keys.push_back(TmplValue{kv.first});
						}
						set_owned(move(keys));
					} else if (key == "values" && cur->is_object()) {
						TmplValue vals{TmplValue::Array{}};
						for (auto const &kv: cur->as_object()) {
							vals.push_back(kv.second);
						}
						set_owned(move(vals));
					} else if (key == "items" && cur->is_object()) {
						TmplValue items{TmplValue::Array{}};
						for (auto const &kv: cur->as_object()) {
							TmplValue P{TmplValue::Array{}};
							P.push_back(TmplValue{kv.first});
							P.push_back(kv.second);
							items.push_back(move(P));
						}
						set_owned(move(items));
					}
					continue;
				}

				if (!remaining.empty() && remaining[0] == '.') {
					remaining = remaining.substr(1);
				}
			}
			if (use_owned) {
				return owned;
			}
			return *cur;
		}
	};

	TmplValue result = eval_base(pipe_parts[0]);

	for (SZ i = 1; i < pipe_parts.size(); ++i) {
		auto filter = trim(pipe_parts[i]);
		S filter_name;
		V<S> filter_args;

		auto paren = filter.find('(');
		if (paren != S::npos) {
			filter_name = trim(filter.substr(0, paren));
			auto close = filter.rfind(')');
			if (close != S::npos) {
				filter_args = split_args(filter.substr(paren + 1, close - paren - 1));
			}
		} else {
			filter_name = filter;
		}

		result = apply_filter(filter_name, result, filter_args, context);
	}

	return result;
}
// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
TmplValue Environment::Impl::apply_filter(
	S const &name,
	TmplValue const &val,
	V<S> const &args,
	TmplValue const &context) const {
	if (name == "length" || name == "count") {
		if (val.is_array()) {
			return TmplValue{static_cast<i64>(val.as_array().size())};
		}
		if (val.is_string()) {
			return TmplValue{static_cast<i64>(val.as<SV>().size())};
		}
		if (val.is_object()) {
			return TmplValue{static_cast<i64>(val.as_object().size())};
		}
		return TmplValue{i64{0}};
	}
	if (name == "S") {
		return TmplValue{value_to_string(val)};
	}
	if (name == "int") {
		if (val.is_int() || val.is_uint()) {
			return val;
		}
		if (val.is_float()) {
			return TmplValue{static_cast<i64>(val.as<double>())};
		}
		if (val.is_string()) {
			auto s = S(val.as<SV>());
			try {
				return TmplValue{static_cast<i64>(std::stoll(s))};
			} catch (exception const &e) {
				eprintln(format("template filter int: failed to parse '{}': {}", s, e.what()));
				return TmplValue{i64{0}};
			}
		}
		return TmplValue{i64{0}};
	}
	if (name == "upper") {
		auto s = value_to_string(val);
		for (auto &c: s) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return TmplValue{move(s)};
	}
	if (name == "lower") {
		auto s = value_to_string(val);
		for (auto &c: s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return TmplValue{move(s)};
	}
	if (name == "capitalize") {
		return TmplValue{str_capitalize(value_to_string(val))};
	}
	if (name == "title") {
		auto s = value_to_string(val);
		bool next_upper = true;
		for (auto &c: s) {
			if (std::isspace(static_cast<unsigned char>(c)) != 0) {
				next_upper = true;
				continue;
			}
			if (next_upper) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				next_upper = false;
			}
		}
		return TmplValue{move(s)};
	}
	if (name == "replace") {
		auto s = value_to_string(val);
		if (args.size() >= 2) {
			auto old_s = value_to_string(eval_expr(args[0], context));
			auto new_s = value_to_string(eval_expr(args[1], context));
			return TmplValue{str_replace_all(s, old_s, new_s)};
		}
		return TmplValue{move(s)};
	}
	if (name == "default" || name == "d") {
		if (!is_truthy(val) && !args.empty()) {
			return eval_expr(args[0], context);
		}
		return val;
	}
	if (name == "join") {
		if (val.is_array()) {
			auto sep = !args.empty() ? value_to_string(eval_expr(args[0], context)) : "";
			auto const &arr = val.as_array();
			S result;
			result.reserve(arr.size() * (sep.size() + 16));
			bool first = true;
			for (auto const &item: arr) {
				if (!first) {
					result += sep;
				}
				first = false;
				result += value_to_string(item);
			}
			return TmplValue{move(result)};
		}
		return val;
	}
	if (name == "sort") {
		if (val.is_array()) {
			TmplValue result{val.as_array()};
			std::sort(result.as_array().begin(), result.as_array().end(), [](TmplValue const &a, TmplValue const &b) {
				return a.dump() < b.dump();
			});
			return result;
		}
		return val;
	}
	if (name == "reverse") {
		if (val.is_array()) {
			TmplValue result{val.as_array()};
			std::reverse(result.as_array().begin(), result.as_array().end());
			return result;
		}
		return val;
	}
	if (name == "last") {
		if (val.is_array() && !val.as_array().empty()) {
			return val.as_array().back();
		}
		return {};
	}
	if (name == "min") {
		if (val.is_array()) {
			auto const &arr = val.as_array();
			if (arr.empty()) {
				return {};
			}
			TmplValue const *m = arr.data();
			for (SZ i = 1; i < arr.size(); ++i) {
				if (arr[i].dump() < m->dump()) {
					m = &arr[i];
				}
			}
			return *m;
		}
		return val;
	}
	if (name == "list") {
		if (val.is_array()) {
			return val;
		}
		return TmplValue{TmplValue::Array{}};
	}
	if (name == "selectattr") {
		if (val.is_array() && args.size() >= 3) {
			auto attr = value_to_string(eval_expr(args[0], context));
			auto test = value_to_string(eval_expr(args[1], context));
			auto test_val = eval_expr(args[2], context);
			TmplValue result{TmplValue::Array{}};
			for (auto const &item: val.as_array()) {
				if (!item.is_object()) {
					continue;
				}
				auto const *v = obj_find(item, attr);
				if (v == nullptr) {
					continue;
				}
				if (test == "eq" || test == "equalto" || test == "==") {
					if (*v == test_val) {
						result.push_back(item);
					}
				} else if (test == "in") {
					if (test_val.is_array()) {
						for (auto const &tv: test_val.as_array()) {
							if (*v == tv) {
								result.push_back(item);
								break;
							}
						}
					}
				}
			}
			return result;
		}
		return TmplValue{TmplValue::Array{}};
	}
	if (name == "attr") {
		if (val.is_object() && !args.empty()) {
			auto attr_name = value_to_string(eval_expr(args[0], context));
			auto const *found = obj_find(val, attr_name);
			return (found != nullptr) ? *found : TmplValue{};
		}
		return {};
	}
	if (name == "e" || name == "escape") {
		auto s = value_to_string(val);
		S result;
		result.reserve(s.size());
		for (char const c: s) {
			switch (c) {
			case '&' : result += "&amp;"; break;
			case '<' : result += "&lt;"; break;
			case '>' : result += "&gt;"; break;
			case '"' : result += "&quot;"; break;
			case '\'': result += "&#39;"; break;
			default  : result += c;
			}
		}
		return TmplValue{move(result)};
	}
	return val;
}
// ---------------------------------------------------------------------------
// value_to_string / is_truthy
// ---------------------------------------------------------------------------

S Environment::Impl::value_to_string(
	TmplValue const &v) {
	if (v.is_null()) {
		return "";
	}
	if (v.is_string()) {
		return S(v.as<SV>());
	}
	if (v.is_int()) {
		return to_string(v.as<i64>());
	}
	if (v.is_uint()) {
		return to_string(v.as<u64>());
	}
	if (v.is_float()) {
		auto s = to_string(v.as<double>());
		auto dot = s.find('.');
		if (dot != S::npos) {
			auto last = s.find_last_not_of('0');
			if (last != S::npos && last > dot) {
				s.erase(last + 1);
			}
			if (s.back() == '.') {
				s.pop_back();
			}
		}
		return s;
	}
	if (v.is_bool()) {
		return v.as<bool>() ? "True" : "False";
	}
	return v.dump();
}
bool Environment::Impl::is_truthy(
	TmplValue const &v) {
	if (v.is_null()) {
		return false;
	}
	if (v.is_bool()) {
		return v.as<bool>();
	}
	if (v.is_int()) {
		return v.as<i64>() != 0;
	}
	if (v.is_uint()) {
		return v.as<u64>() != 0;
	}
	if (v.is_float()) {
		return v.as<double>() != 0.0;
	}
	if (v.is_string()) {
		return !v.as<SV>().empty();
	}
	if (v.is_array()) {
		return !v.as_array().empty();
	}
	if (v.is_object()) {
		return !v.as_object().empty();
	}
	return false;
}
// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
S Environment::Impl::render_nodes(
	NodeList const &nodes,
	TmplValue context,
	UM<S, NodeList> const *blocks,
	UM<S, Tup<V<S>, V<S>, NodeList>> *macros,
	int depth) const {
	if (depth > kMaxTemplateDepth) {
		throw RE{"template render recursion depth exceeded"};
	}
	S out;
	UM<S, Tup<V<S>, V<S>, NodeList>> local_macros;
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
					{
						auto &e = n.expr;
						auto paren = e.find('(');
						if (paren != S::npos && e.back() == ')') {
							auto macro_name = trim(e.substr(0, paren));
							bool const simple_ident =
								!macro_name.empty() && std::all_of(macro_name.begin(), macro_name.end(), [](char c) {
									return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
								});
							if (simple_ident && macros) {
								auto it = macros->find(macro_name);
								if (it != macros->end()) {
									auto args_str = e.substr(paren + 1, e.size() - paren - 2);
									auto raw_args = split_args(args_str);
									V<S> pos_args;
									UM<S, S> kw_args;
									for (auto &a: raw_args) {
										SZ ei = 0;
										while (ei < a.size()
											   && (std::isalnum(static_cast<unsigned char>(a[ei])) || a[ei] == '_')) {
											++ei;
										}
										if (ei > 0
											&& ei < a.size()
											&& a[ei] == '='
											&& (ei + 1 >= a.size() || a[ei + 1] != '=')) {
											kw_args[trim(a.substr(0, ei))] = trim(a.substr(ei + 1));
										} else {
											pos_args.push_back(a);
										}
									}
									auto &[params, defaults, body] = it->second;
									auto saved = save_scope(context, params);
									for (SZ i = 0; i < params.size(); ++i) {
										if (i < pos_args.size()) {
											context.set(params[i], eval_expr(pos_args[i], context));
										} else if (auto kit = kw_args.find(params[i]); kit != kw_args.end()) {
											context.set(params[i], eval_expr(kit->second, context));
										} else if (i < defaults.size() && !defaults[i].empty()) {
											context.set(params[i], eval_expr(defaults[i], context));
										} else {
											context.set(params[i], TmplValue{});
										}
									}
									out += render_nodes(body, context, blocks, macros, depth + 1);
									restore_scope(context, saved);
									macro_handled = true;
								}
							}
						}
					}
					if (!macro_handled) {
						out += value_to_string(eval_expr(n.expr, context));
					}
				} else if constexpr (std::is_same_v<T, BlockNode>) {
					if (blocks) {
						auto it = blocks->find(n.name);
						if (it != blocks->end()) {
							out += render_nodes(it->second, context, blocks, macros, depth + 1);
							return;
						}
					}
					out += render_nodes(n.body, context, blocks, macros, depth + 1);
				} else if constexpr (std::is_same_v<T, ExtendsNode>) {
					// handled at template level
				} else if constexpr (std::is_same_v<T, IncludeNode>) {
					auto it = cache.find(n.name);
					if (it == cache.end()) {
						throw RE{format("template error: included template '{}' not found", n.name)};
					}
					out += render_template(it->second, context, blocks, depth + 1);
				} else if constexpr (std::is_same_v<T, SetNode>) {
					auto val = eval_expr(n.expr, context);
					if (context.is_object()) {
						context.set(n.var, move(val));
					}
				} else if constexpr (std::is_same_v<T, ForNode>) {
					auto iter_val = eval_expr(n.iter_expr, context);
					if (iter_val.is_array()) {
						auto saved = save_scope(context, n.vars);
						auto const *prev_loop = obj_find(context, "loop");
						Opt<TmplValue> saved_loop = prev_loop ? Opt<TmplValue>{*prev_loop} : nullopt;
						auto const &arr = iter_val.as_array();
						for (SZ i = 0; i < arr.size(); ++i) {
							if (n.vars.size() == 1) {
								context.set(n.vars[0], arr[i]);
							} else {
								auto const &item = arr[i];
								for (SZ j = 0; j < n.vars.size(); ++j) {
									if (item.is_array() && j < item.as_array().size()) {
										context.set(n.vars[j], item.as_array()[j]);
									} else {
										context.set(n.vars[j], TmplValue{});
									}
								}
							}
							TmplValue loop_obj{TmplValue::Object{}};
							loop_obj.set("index0", TmplValue{static_cast<i64>(i)});
							loop_obj.set("index", TmplValue{static_cast<i64>(i + 1)});
							loop_obj.set("first", TmplValue{i == 0});
							loop_obj.set("last", TmplValue{i == arr.size() - 1});
							loop_obj.set("length", TmplValue{static_cast<i64>(arr.size())});
							context.set("loop", move(loop_obj));
							out += render_nodes(n.body, context, blocks, macros, depth + 1);
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
							out += render_nodes(branch.body, context, blocks, macros, depth + 1);
							break;
						}
						if (is_truthy(eval_expr(branch.condition, context))) {
							out += render_nodes(branch.body, context, blocks, macros, depth + 1);
							break;
						}
					}
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					(*macros)[n.name] = {n.params, n.defaults, n.body};
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					auto it_tmpl = cache.find(n.file);
					if (it_tmpl == cache.end()) {
						throw RE{format("template error: imported file '{}' not found", n.file)};
					}
					bool found = false;
					for (auto &sub: it_tmpl->second.nodes) {
						std::visit(
							[&](auto &&sn) {
								using ST = std::decay_t<decltype(sn)>;
								if constexpr (std::is_same_v<ST, MacroNode>) {
									if (sn.name == n.name) {
										(*macros)[n.alias] = {sn.params, sn.defaults, sn.body};
										found = true;
									}
								}
							},
							sub->data);
					}
					if (!found) {
						throw RE{format("template error: macro '{}' not found in '{}'", n.name, n.file)};
					}
				}
			},
			node->data);
	}
	return out;
}
// NOLINTNEXTLINE(misc-no-recursion)
S Environment::Impl::render_template(
	Template const &tmpl,
	TmplValue context,
	UM<S, NodeList> const *child_blocks,
	int depth) const {
	if (depth > kMaxTemplateDepth) {
		throw RE{"template render recursion depth exceeded"};
	}
	if (!tmpl.extends_name.empty()) {
		auto it = cache.find(tmpl.extends_name);
		if (it == cache.end()) {
			throw RE{"template not found: " + tmpl.extends_name};
		}

		for (auto &node: tmpl.nodes) {
			std::visit(
				[&](auto &n) {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, SetNode>) {
						auto val = eval_expr(n.expr, context);
						if (context.is_object()) {
							context.set(n.var, move(val));
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

		return render_template(it->second, move(context), &merged, depth + 1);
	}

	return render_nodes(tmpl.nodes, move(context), child_blocks, nullptr, depth);
}
// ---------------------------------------------------------------------------
// Environment public interface
// ---------------------------------------------------------------------------

bool Environment::Impl::extension_allowed(
	fs::path const &path) const {
	auto ext = path.extension().string();
	for (auto const &allowed: options.extensions) {
		if (ext == allowed) {
			return true;
		}
	}
	return false;
}
void Environment::Impl::reload_path(
	S const &path) {
	fs::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	auto buf = blocking_read_text_file_nothrow(p.string());
	if (!buf) {
		return;
	}
	auto name = p.filename().string();
	auto parsed = parse(name, *buf);
	std::unique_lock const lk{cache_mtx};
	cache[name] = move(parsed);
}
void Environment::Impl::remove_path(
	S const &path) {
	fs::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	auto name = p.filename().string();
	std::unique_lock const lk{cache_mtx};
	cache.erase(name);
}
#if CONFLUX_HAS_FILE_WATCH
void Environment::Impl::maybe_start_watcher() const {
	if (!options.watch_enabled || watch_started.load(memory_order_acquire)) {
		return;
	}
	bool expected = false;
	if (!watch_started.compare_exchange_strong(expected, true, memory_order_acq_rel)) {
		return;
	}
	try {
		auto fw = make_unique<FileWatcher>();
		fw->watch_directory(template_dir);
		auto *self = const_cast<Impl *>(this);
		fw->on_events([self](V<FileEvent> const &events) {
			for (auto const &ev: events) {
				switch (ev.kind) {
				case FileEventKind::created:
				case FileEventKind::modified:
				case FileEventKind::moved_to  : self->reload_path(ev.path); break;
				case FileEventKind::removed   :
				case FileEventKind::moved_from: self->remove_path(ev.path); break;
				case FileEventKind::overflow:
					for (auto &entry: fs::directory_iterator(self->template_dir)) {
						if (entry.is_regular_file()) {
							self->reload_path(entry.path().string());
						}
					}
					break;
				}
			}
		});
		fw->on_error([this](EP const &) { watch_started.store(false, memory_order_release); });
		fw->start();
		watcher = move(fw);
	} catch (...) { watch_started.store(false, memory_order_release); }
}
#endif
Environment::Environment(
	S const &template_dir)
	: impl_(make_unique<Impl>()) {
	impl_->template_dir = template_dir;
}
Environment::Environment(
	S const &template_dir,
	EnvironmentOptions options)
	: impl_(make_unique<Impl>()) {
	impl_->template_dir = template_dir;
	impl_->options = move(options);
}
Environment::~Environment() = default;
Environment::Environment(Environment &&) noexcept = default;
Environment &Environment::operator =(Environment &&) noexcept = default;
void Environment::load_all() {
	if (!fs::exists(impl_->template_dir)) {
		return;
	}

	UM<S, Template> parsed;
	for (auto &entry: fs::directory_iterator(impl_->template_dir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		if (!impl_->extension_allowed(entry.path())) {
			continue;
		}

		auto buf = blocking_read_text_file_nothrow(entry.path().string());
		if (!buf) {
			continue;
		}

		auto name = entry.path().filename().string();
		parsed[name] = impl_->parse(name, *buf);
	}

	std::unique_lock const lk{impl_->cache_mtx};
	impl_->cache = move(parsed);
}
S Environment::render(
	S const &name,
	S const &json_ctx) const {
#if CONFLUX_HAS_FILE_WATCH
	impl_->maybe_start_watcher();
#endif
	std::shared_lock const lk{impl_->cache_mtx};
	auto it = impl_->cache.find(name);
	if (it == impl_->cache.end()) {
		throw RE{"template not found: " + name};
	}

	auto parsed_doc = conflux::json::parse(json_ctx);
	TmplValue ctx = parsed_doc ? node_to_tmpl(parsed_doc->root()) : TmplValue{TmplValue::Object{}};
	return impl_->render_template(it->second, move(ctx));
}
S Environment::render_string(
	S const &source,
	S const &json_ctx) const {
	auto tmpl = impl_->parse("<S>", source);
	auto parsed_doc = conflux::json::parse(json_ctx);
	TmplValue ctx = parsed_doc ? node_to_tmpl(parsed_doc->root()) : TmplValue{TmplValue::Object{}};
	std::shared_lock const lk{impl_->cache_mtx};
	return impl_->render_template(tmpl, move(ctx));
}

} // namespace tmpl
