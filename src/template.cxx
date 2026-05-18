module;
#include <memory>

export module conflux.templates;
import conflux.types;
import std.compat;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;
export namespace tmpl {

struct CompiledExpr;
using CompiledExprPtr = std::shared_ptr<CompiledExpr>;

enum class CompiledLiteralKind : std::uint8_t {
	none,
	boolean,
	integer,
	floating,
	string,
};
struct CompiledLiteral {
	CompiledLiteralKind kind = CompiledLiteralKind::none;
	bool boolean = false;
	std::int64_t integer = 0;
	double floating = 0.0;
	std::string string;
};
enum class CompiledCompareOp : std::uint8_t {
	eq,
	ne,
	le,
	ge,
	lt,
	gt,
	in,
};
enum class CompiledPathSegmentKind : std::uint8_t {
	field,
	index,
	slice,
	method,
};
struct CompiledPathSegment {
	CompiledPathSegmentKind kind = CompiledPathSegmentKind::field;
	std::string name;
	CompiledExprPtr expr;
	CompiledExprPtr start;
	CompiledExprPtr end;
	std::vector<CompiledExprPtr> args;
};
struct CompiledObjectItem {
	std::string key;
	CompiledExprPtr value;
};
enum class CompiledBaseKind : std::uint8_t {
	literal,
	array,
	tuple,
	object,
	group,
	path,
	unary_not,
	binary_or,
	binary_and,
	compare,
	concat,
};
struct CompiledBaseExpr {
	CompiledBaseKind kind = CompiledBaseKind::path;
	std::string source;
	CompiledLiteral literal;
	CompiledCompareOp compare_op = CompiledCompareOp::eq;
	std::vector<CompiledExprPtr> operands;
	std::vector<CompiledObjectItem> object_items;
	std::vector<CompiledPathSegment> path;
};
struct CompiledFilter {
	std::string name;
	std::vector<std::string> args;
	std::vector<CompiledExprPtr> compiled_args;
};
struct CompiledMacroArg {
	std::string name;
	std::string expr;
	CompiledExprPtr compiled;
	bool keyword = false;
};
struct CompiledMacroCall {
	std::string name;
	std::vector<CompiledMacroArg> args;
};
struct CompiledExpr {
	std::string source;
	std::string base;
	std::shared_ptr<CompiledBaseExpr> compiled_base;
	std::vector<CompiledFilter> filters;
	std::optional<CompiledMacroCall> macro_call;
};
struct Node;
using NodePtr = std::shared_ptr<Node>;
using NodeList = std::vector<NodePtr>;
struct TextNode {
	std::string text;
};
struct ExprNode {
	std::string expr;
	CompiledExpr compiled;
};
struct BlockNode {
	std::string name;
	NodeList body;
};
struct ExtendsNode {
	std::string parent;
};
struct IncludeNode {
	std::string name;
};
struct SetNode {
	std::string var;
	std::string expr;
	CompiledExpr compiled;
};
struct ForNode {
	std::vector<std::string> vars;
	std::string iter_expr;
	CompiledExpr compiled_iter;
	NodeList body;
};
struct IfNode {
	struct Branch {
		std::string condition;
		CompiledExpr compiled_condition;
		NodeList body;
	};
	std::vector<Branch> branches;
};
struct MacroNode {
	std::string name;
	std::vector<std::string> params;
	std::vector<std::string> defaults;
	std::vector<CompiledExpr> compiled_defaults;
	NodeList body;
};
struct FromImportNode {
	std::string file;
	std::string name;
	std::string alias;
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
	std::string name;
	NodeList nodes;
	std::string extends_name;
	std::unordered_map<std::string, NodeList> blocks;
};
struct EnvironmentOptions {
	std::vector<std::string> extensions{".html", ".htm", ".txt"};
};
struct TmplValue;
enum class TemplateDiagnosticSeverity : std::uint8_t {
	warning,
	error,
};
enum class TemplateDiagnosticPhase : std::uint8_t {
	io,
	parse,
	compile,
	link,
	render_check,
};
struct TemplateSourceLocation {
	std::string template_name;
	std::string path;
	std::uint32_t line = 0;
	std::uint32_t column = 0;
	std::uint32_t byte_offset = 0;
};
struct TemplateDiagnostic {
	TemplateDiagnosticSeverity severity = TemplateDiagnosticSeverity::error;
	TemplateDiagnosticPhase phase = TemplateDiagnosticPhase::compile;
	TemplateSourceLocation location;
	std::vector<TemplateSourceLocation> stack;
	std::string code;
	std::string message;
};
struct TemplateBuildReport {
	std::vector<TemplateDiagnostic> diagnostics;
	std::size_t templates_seen = 0;
	std::size_t templates_compiled = 0;

	[[nodiscard]] bool ok() const noexcept;
	[[nodiscard]] std::string format_text() const;
};
struct TemplateBuildError final : std::runtime_error {
	TemplateBuildReport report;
	explicit TemplateBuildError(TemplateBuildReport report);
};
class Environment {
public:
	explicit Environment(std::string const &template_dir);
	Environment(std::string const &template_dir, EnvironmentOptions options);
	~Environment();
	Environment(Environment &&) noexcept;
	Environment &operator =(Environment &&) noexcept;
	Environment(Environment const &) = delete;
	Environment &operator =(Environment const &) = delete;

	void load_all();
	void blocking_load_all();
	void blocking_reload_all();
	[[nodiscard]] expected<void, TemplateBuildReport> blocking_load_all_checked();
	[[nodiscard]] expected<void, TemplateBuildReport> blocking_reload_all_checked();
	[[nodiscard]] std::string render(std::string const &name, std::string const &json_ctx) const;
	[[nodiscard]] std::string render(std::string const &name, TmplValue const &ctx) const;
	[[nodiscard]] std::string render(std::string const &name, NodeRef ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, std::string const &json_ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, TmplValue const &ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, NodeRef ctx) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace tmpl
namespace tmpl {
namespace fs = std::filesystem;
// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static std::string_view diagnostic_severity_name(
	TemplateDiagnosticSeverity severity) noexcept {
	switch (severity) {
	case TemplateDiagnosticSeverity::warning: return "warning";
	case TemplateDiagnosticSeverity::error  : return "error";
	}
	return "error";
}
static std::string_view diagnostic_phase_name(
	TemplateDiagnosticPhase phase) noexcept {
	switch (phase) {
	case TemplateDiagnosticPhase::io          : return "io";
	case TemplateDiagnosticPhase::parse       : return "parse";
	case TemplateDiagnosticPhase::compile     : return "compile";
	case TemplateDiagnosticPhase::link        : return "link";
	case TemplateDiagnosticPhase::render_check: return "render_check";
	}
	return "compile";
}
bool TemplateBuildReport::ok() const noexcept {
	return std::none_of(diagnostics.begin(), diagnostics.end(), [](TemplateDiagnostic const &d) {
		return d.severity == TemplateDiagnosticSeverity::error;
	});
}
std::string TemplateBuildReport::format_text() const {
	if (diagnostics.empty()) {
		return format("template build ok: {} templates compiled", templates_compiled);
	}
	std::string out;
	for (auto const &d: diagnostics) {
		auto const &loc = d.location;
		if (!loc.path.empty()) {
			out += loc.path;
		} else if (!loc.template_name.empty()) {
			out += loc.template_name;
		} else {
			out += "<templates>";
		}
		if (loc.line != 0 || loc.column != 0) {
			out += format(":{}:{}", loc.line, loc.column);
		}
		out += format(
			": {} {}.{}: {}",
			diagnostic_severity_name(d.severity),
			diagnostic_phase_name(d.phase),
			d.code.empty() ? "unknown" : d.code,
			d.message);
		if (!d.stack.empty()) {
			out += " [stack:";
			for (auto const &frame: d.stack) {
				out += ' ';
				out += !frame.template_name.empty() ? frame.template_name : frame.path;
			}
			out += ']';
		}
		out += '\n';
	}
	return out;
}
TemplateBuildError::TemplateBuildError(
	TemplateBuildReport report_)
	: std::runtime_error{report_.format_text()}
	, report{move(report_)} {}

// ---------------------------------------------------------------------------
// Internal mutable value type for template context
// ---------------------------------------------------------------------------

export struct TmplValue {
	using Array = std::vector<TmplValue>;
	using Object = std::vector<std::pair<std::string, TmplValue>>;

	variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> data;

	TmplValue() = default;
	explicit TmplValue(
		bool b)
		: data(b) {}
	explicit TmplValue(
		std::int64_t v)
		: data(v) {}
	explicit TmplValue(
		std::uint64_t v)
		: data(v) {}
	explicit TmplValue(
		double v)
		: data(v) {}
	explicit TmplValue(
		std::string s)
		: data(move(s)) {}
	explicit TmplValue(
		std::string_view sv)
		: data(std::string{sv}) {}
	explicit TmplValue(
		Array a)
		: data(move(a)) {}
	explicit TmplValue(
		Object o)
		: data(move(o)) {}
	[[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
	[[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
	[[nodiscard]] bool is_int() const noexcept { return std::holds_alternative<std::int64_t>(data); }
	[[nodiscard]] bool is_uint() const noexcept { return std::holds_alternative<std::uint64_t>(data); }
	[[nodiscard]] bool is_float() const noexcept { return std::holds_alternative<double>(data); }
	[[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(data); }
	[[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
	[[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(data); }
	template<class T>
	[[nodiscard]] decltype(auto) as() const {
		if constexpr (same_as<T, std::string_view>) {
			return std::string_view{std::get<std::string>(data)};
		} else {
			return std::get<T>(data);
		}
	}
	[[nodiscard]] Array &as_array() { return std::get<Array>(data); }
	[[nodiscard]] Array const &as_array() const { return std::get<Array>(data); }
	[[nodiscard]] Object &as_object() { return std::get<Object>(data); }
	[[nodiscard]] Object const &as_object() const { return std::get<Object>(data); }
	void set(
		std::string_view key,
		TmplValue val) {
		auto &obj = std::get<Object>(data);
		for (auto &[k, v]: obj) {
			if (k == key) {
				v = move(val);
				return;
			}
		}
		obj.emplace_back(std::string{key}, move(val));
	}
	void erase(
		std::string_view key) {
		auto &obj = std::get<Object>(data);
		std::erase_if(obj, [key](auto const &p) { return p.first == key; });
	}
	void push_back(
		TmplValue val) {
		std::get<Array>(data).push_back(move(val));
	}
	[[nodiscard]] bool operator ==(TmplValue const &) const = default;

	[[nodiscard]] std::string dump() const;
};
// NOLINTNEXTLINE(misc-no-recursion)
std::string TmplValue::dump() const {
	if (is_null()) {
		return "null";
	}
	if (is_bool()) {
		return std::get<bool>(data) ? "true" : "false";
	}
	if (is_int()) {
		return to_string(std::get<std::int64_t>(data));
	}
	if (is_uint()) {
		return to_string(std::get<std::uint64_t>(data));
	}
	if (is_float()) {
		auto s = to_string(std::get<double>(data));
		auto dot = s.find('.');
		if (dot != std::string::npos) {
			auto last = s.find_last_not_of('0');
			if (last != std::string::npos && last > dot) {
				s.erase(last + 1);
			}
			if (s.back() == '.') {
				s.pop_back();
			}
		}
		return s;
	}
	if (is_string()) {
		std::string out = "\"";
		for (char const c: std::get<std::string>(data)) {
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
		std::string out = "[";
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
		std::string out = "{";
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
	case JsonKind::string: return TmplValue{std::string{*node.as_string()}};
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
				pairs.emplace_back(std::string{name}, node_to_tmpl(val));
			}
			return TmplValue{move(pairs)};
		}
	default: return {};
	}
}
// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

static std::string trim(
	std::string_view s) {
	while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
		s.remove_suffix(1);
	}
	return std::string(s);
}
static std::string str_replace_all(
	std::string_view src,
	std::string_view old_s,
	std::string_view new_s) {
	std::string out;
	if (old_s.empty()) {
		out.assign(src);
		return out;
	}
	out.reserve(src.size());
	std::size_t p = 0;
	while (p < src.size()) {
		auto f = src.find(old_s, p);
		if (f == std::string_view::npos) {
			out.append(src.substr(p));
			break;
		}
		out.append(src.substr(p, f - p));
		out.append(new_s);
		p = f + old_s.size();
	}
	return out;
}
static std::string str_capitalize(
	std::string s) {
	if (!s.empty()) {
		s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	}
	return s;
}
static std::vector<std::string> split_args(
	std::string const &s) {
	std::vector<std::string> args;
	std::string current;
	int depth = 0;
	bool in_str = false;
	char str_char = 0;
	for (std::size_t i = 0; i < s.size(); ++i) {
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

static bool is_template_identifier(
	std::string_view s) noexcept {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	});
}
static bool is_quote(
	char c) noexcept {
	return c == '"' || c == '\'';
}
static bool is_quoted_string(
	std::string_view s) noexcept {
	return s.size() >= 2 && is_quote(s.front()) && s.back() == s.front();
}
static std::size_t find_matching_pair(
	std::string_view s,
	std::size_t open_pos,
	char open,
	char close) noexcept {
	int depth = 0;
	bool in_str = false;
	char quote = 0;
	for (std::size_t i = open_pos; i < s.size(); ++i) {
		char const c = s[i];
		if (in_str) {
			if (c == quote && (i == 0 || s[i - 1] != '\\')) {
				in_str = false;
			}
			continue;
		}
		if (is_quote(c)) {
			in_str = true;
			quote = c;
			continue;
		}
		if (c == open) {
			++depth;
			continue;
		}
		if (c == close) {
			--depth;
			if (depth == 0) {
				return i;
			}
		}
	}
	return std::string_view::npos;
}
static bool outer_pair_wraps(
	std::string_view s,
	char open,
	char close) noexcept {
	return s.size() >= 2 && s.front() == open && find_matching_pair(s, 0, open, close) == s.size() - 1;
}
static std::size_t find_top_level_token(
	std::string_view haystack,
	std::string_view needle) noexcept {
	int depth = 0;
	bool in_str = false;
	char quote = 0;
	for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		char const c = haystack[i];
		if (in_str) {
			if (c == quote && (i == 0 || haystack[i - 1] != '\\')) {
				in_str = false;
			}
			continue;
		}
		if (is_quote(c)) {
			in_str = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			++depth;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			--depth;
			continue;
		}
		if (depth == 0 && haystack.substr(i, needle.size()) == needle) {
			return i;
		}
	}
	return std::string_view::npos;
}
static std::size_t find_top_level_char(
	std::string_view haystack,
	char needle) noexcept {
	int depth = 0;
	bool in_str = false;
	char quote = 0;
	for (std::size_t i = 0; i < haystack.size(); ++i) {
		char const c = haystack[i];
		if (in_str) {
			if (c == quote && (i == 0 || haystack[i - 1] != '\\')) {
				in_str = false;
			}
			continue;
		}
		if (is_quote(c)) {
			in_str = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			++depth;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			--depth;
			continue;
		}
		if (depth == 0 && c == needle) {
			return i;
		}
	}
	return std::string_view::npos;
}
static CompiledExpr compile_expr(std::string const &expr);
static CompiledExprPtr compile_expr_ptr(
	std::string const &expr) {
	return make_shared<CompiledExpr>(compile_expr(expr));
}
static std::vector<CompiledExprPtr> compile_expr_ptr_list(
	std::vector<std::string> const &exprs) {
	std::vector<CompiledExprPtr> out;
	out.reserve(exprs.size());
	for (auto const &expr: exprs) {
		out.push_back(compile_expr_ptr(expr));
	}
	return out;
}
static std::optional<CompiledLiteral> compile_literal(
	std::string const &expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return nullopt;
	}
	if (is_quoted_string(b)) {
		return CompiledLiteral{.kind = CompiledLiteralKind::string, .string = b.substr(1, b.size() - 2)};
	}
	if (b == "true" || b == "True") {
		return CompiledLiteral{.kind = CompiledLiteralKind::boolean, .boolean = true};
	}
	if (b == "false" || b == "False") {
		return CompiledLiteral{.kind = CompiledLiteralKind::boolean, .boolean = false};
	}
	if (b == "none" || b == "None") {
		return CompiledLiteral{};
	}
	if (std::isdigit(static_cast<unsigned char>(b[0])) || (b[0] == '-' && b.size() > 1)) {
		try {
			std::size_t parsed = 0;
			if (b.find('.') != std::string::npos) {
				auto value = std::stod(b, &parsed);
				if (parsed == b.size()) {
					return CompiledLiteral{.kind = CompiledLiteralKind::floating, .floating = value};
				}
			} else {
				auto value = std::stoll(b, &parsed);
				if (parsed == b.size()) {
					return CompiledLiteral{.kind = CompiledLiteralKind::integer, .integer = static_cast<std::int64_t>(value)};
				}
			}
		} catch (exception const &) {
			return nullopt;
		}
	}
	return nullopt;
}
static std::optional<CompiledPathSegment> compile_path_method(
	std::string const &name,
	std::string_view remaining) {
	if (remaining.empty() || remaining.front() != '(') {
		return nullopt;
	}
	auto close = find_matching_pair(remaining, 0, '(', ')');
	if (close == std::string_view::npos) {
		return nullopt;
	}
	auto raw_args = split_args(std::string{remaining.substr(1, close - 1)});
	return CompiledPathSegment{
		.kind = CompiledPathSegmentKind::method,
		.name = name,
		.args = compile_expr_ptr_list(raw_args),
	};
}
static std::optional<CompiledBaseExpr> compile_path_base(
	std::string const &expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return nullopt;
	}
	CompiledBaseExpr out;
	out.kind = CompiledBaseKind::path;
	out.source = b;
	std::string_view remaining{b};
	while (!remaining.empty()) {
		auto bracket = remaining.find('[');
		auto dot = remaining.find('.');
		auto paren = remaining.find('(');
		auto next_sep = min({bracket, dot, paren, remaining.size()});
		if (next_sep == 0 && bracket == 0) {
			auto close = find_matching_pair(remaining, 0, '[', ']');
			if (close == std::string_view::npos) {
				return nullopt;
			}
			auto idx = trim(remaining.substr(1, close - 1));
			if (auto colon = find_top_level_char(idx, ':'); colon != std::string_view::npos) {
				CompiledPathSegment seg{.kind = CompiledPathSegmentKind::slice};
				auto start = trim(std::string_view{idx}.substr(0, colon));
				auto end = trim(std::string_view{idx}.substr(colon + 1));
				if (!start.empty()) {
					seg.start = compile_expr_ptr(start);
				}
				if (!end.empty()) {
					seg.end = compile_expr_ptr(end);
				}
				out.path.push_back(move(seg));
			} else {
				out.path.push_back(CompiledPathSegment{
					.kind = CompiledPathSegmentKind::index,
					.expr = compile_expr_ptr(idx),
				});
			}
			remaining.remove_prefix(close + 1);
			if (!remaining.empty() && remaining.front() == '.') {
				remaining.remove_prefix(1);
			}
			continue;
		}

		auto key = trim(remaining.substr(0, next_sep));
		remaining = next_sep < remaining.size() ? remaining.substr(next_sep) : std::string_view{};
		bool const is_method_call = !remaining.empty() && remaining.front() == '(';
		if (!key.empty() && !is_method_call) {
			out.path.push_back(CompiledPathSegment{.kind = CompiledPathSegmentKind::field, .name = move(key)});
		}
		if (is_method_call) {
			auto method = compile_path_method(key, remaining);
			if (!method) {
				return nullopt;
			}
			auto close = find_matching_pair(remaining, 0, '(', ')');
			out.path.push_back(move(*method));
			remaining.remove_prefix(close + 1);
			if (!remaining.empty() && remaining.front() == '.') {
				remaining.remove_prefix(1);
			}
			continue;
		}
		if (!remaining.empty() && remaining.front() == '.') {
			remaining.remove_prefix(1);
		}
	}
	return out;
}
static std::optional<CompiledBaseExpr> compile_base_expr(
	std::string const &expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return nullopt;
	}
	if (auto literal = compile_literal(b); literal) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::literal;
		out.source = b;
		out.literal = move(*literal);
		return out;
	}
	if (outer_pair_wraps(b, '[', ']')) {
		auto inner = trim(std::string_view{b}.substr(1, b.size() - 2));
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::array;
		out.source = b;
		if (!inner.empty()) {
			out.operands = compile_expr_ptr_list(split_args(inner));
		}
		return out;
	}
	if (outer_pair_wraps(b, '(', ')')) {
		auto inner = trim(std::string_view{b}.substr(1, b.size() - 2));
		CompiledBaseExpr out;
		out.source = b;
		if (inner.empty()) {
			out.kind = CompiledBaseKind::tuple;
			return out;
		}
		auto items = split_args(inner);
		if (items.size() == 1) {
			out.kind = CompiledBaseKind::group;
			out.operands.push_back(compile_expr_ptr(items[0]));
			return out;
		}
		out.kind = CompiledBaseKind::tuple;
		out.operands = compile_expr_ptr_list(items);
		return out;
	}
	if (outer_pair_wraps(b, '{', '}')) {
		auto inner = trim(std::string_view{b}.substr(1, b.size() - 2));
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::object;
		out.source = b;
		if (!inner.empty()) {
			auto pairs = split_args(inner);
			out.object_items.reserve(pairs.size());
			for (auto &pair: pairs) {
				auto colon = find_top_level_char(pair, ':');
				if (colon == std::string_view::npos) {
					continue;
				}
				auto key = trim(std::string_view{pair}.substr(0, colon));
				auto val = trim(std::string_view{pair}.substr(colon + 1));
				if (is_quoted_string(key)) {
					key = key.substr(1, key.size() - 2);
				}
				out.object_items.push_back(CompiledObjectItem{move(key), compile_expr_ptr(val)});
			}
		}
		return out;
	}
	if (auto p = find_top_level_token(b, " or "); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::binary_or;
		out.source = b;
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 4))));
		return out;
	}
	if (auto p = find_top_level_token(b, " and "); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::binary_and;
		out.source = b;
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 5))));
		return out;
	}
	if (b.size() > 4 && b.substr(0, 4) == "not ") {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::unary_not;
		out.source = b;
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(4))));
		return out;
	}
	static std::vector<std::pair<std::string, CompiledCompareOp>> const ops = {
		{" == ", CompiledCompareOp::eq},
		{" != ", CompiledCompareOp::ne},
		{" <= ", CompiledCompareOp::le},
		{" >= ", CompiledCompareOp::ge},
		{ " < ", CompiledCompareOp::lt},
		{ " > ", CompiledCompareOp::gt},
		{" in ", CompiledCompareOp::in},
	};
	for (auto const &[op, code]: ops) {
		if (auto p = find_top_level_token(b, op); p != std::string_view::npos) {
			CompiledBaseExpr out;
			out.kind = CompiledBaseKind::compare;
			out.source = b;
			out.compare_op = code;
			out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
			out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + op.size()))));
			return out;
		}
	}
	if (auto p = find_top_level_char(b, '~'); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::concat;
		out.source = b;
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 1))));
		return out;
	}
	return compile_path_base(b);
}
static std::optional<CompiledMacroCall> compile_macro_call(
	std::string const &expr) {
	auto e = trim(expr);
	auto paren = e.find('(');
	if (paren == std::string::npos || e.empty() || e.back() != ')') {
		return nullopt;
	}
	auto name = trim(e.substr(0, paren));
	if (!is_template_identifier(name)) {
		return nullopt;
	}
	CompiledMacroCall call;
	call.name = move(name);
	auto raw_args = split_args(e.substr(paren + 1, e.size() - paren - 2));
	call.args.reserve(raw_args.size());
	for (auto &arg: raw_args) {
		std::size_t eq = 0;
		while (eq < arg.size() && (std::isalnum(static_cast<unsigned char>(arg[eq])) || arg[eq] == '_')) {
			++eq;
		}
		if (eq > 0 && eq < arg.size() && arg[eq] == '=' && (eq + 1 >= arg.size() || arg[eq + 1] != '=')) {
			auto expr_part = trim(arg.substr(eq + 1));
			call.args.push_back(CompiledMacroArg{trim(arg.substr(0, eq)), expr_part, compile_expr_ptr(expr_part), true});
		} else {
			auto expr_part = trim(arg);
			call.args.push_back(CompiledMacroArg{{}, expr_part, compile_expr_ptr(expr_part), false});
		}
	}
	return call;
}
static CompiledExpr compile_expr(
	std::string const &expr) {
	CompiledExpr compiled;
	compiled.source = trim(expr);
	if (compiled.source.empty()) {
		return compiled;
	}
	std::vector<std::string> pipe_parts;
	{
		std::string current;
		int depth = 0;
		bool in_str = false;
		char sc = 0;
		for (std::size_t i = 0; i < compiled.source.size(); ++i) {
			char const c = compiled.source[i];
			if (in_str) {
				current += c;
				if (c == sc && (i == 0 || compiled.source[i - 1] != '\\')) {
					in_str = false;
				}
				continue;
			}
			if (is_quote(c)) {
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
	compiled.base = pipe_parts.empty() ? std::string{} : move(pipe_parts[0]);
	if (auto base = compile_base_expr(compiled.base); base) {
		compiled.compiled_base = make_shared<CompiledBaseExpr>(move(*base));
	}
	compiled.filters.reserve(pipe_parts.size() > 0 ? pipe_parts.size() - 1 : 0);
	for (std::size_t i = 1; i < pipe_parts.size(); ++i) {
		auto filter = trim(pipe_parts[i]);
		CompiledFilter compiled_filter;
		auto paren = filter.find('(');
		if (paren != std::string::npos) {
			compiled_filter.name = trim(filter.substr(0, paren));
			auto close = filter.rfind(')');
			if (close != std::string::npos) {
				compiled_filter.args = split_args(filter.substr(paren + 1, close - paren - 1));
				compiled_filter.compiled_args = compile_expr_ptr_list(compiled_filter.args);
			}
		} else {
			compiled_filter.name = move(filter);
		}
		compiled.filters.push_back(move(compiled_filter));
	}
	if (compiled.filters.empty()) {
		compiled.macro_call = compile_macro_call(compiled.base);
	}
	return compiled;
}
static std::vector<CompiledExpr> compile_expr_list(
	std::vector<std::string> const &exprs) {
	std::vector<CompiledExpr> out;
	out.reserve(exprs.size());
	for (auto const &expr: exprs) {
		out.push_back(compile_expr(expr));
	}
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
static std::vector<Token> tokenize(
	std::string const &source) {
	std::vector<Token> tokens;
	std::size_t pos = 0;

	while (pos < source.size()) {
		auto next_expr = source.find("{{", pos);
		auto next_tag = source.find("{%", pos);
		auto next_comment = source.find("{#", pos);

		auto next = min({next_expr, next_tag, next_comment});
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
			tokens.push_back({TokenType::Expr, trim(source.substr(next + 2, end - next - 2))});
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
	return sp != std::string::npos ? trim(tag.substr(sp + 1)) : "";
}
static TmplValue const *obj_find(
	TmplValue const &obj,
	std::string_view key) {
	for (auto const &kv: obj.as_object()) {
		if (kv.first == key) {
			return &kv.second;
		}
	}
	return nullptr;
}
static std::vector<std::pair<std::string, std::optional<TmplValue>>> save_scope(
	TmplValue const &ctx,
	span<std::string const> names) {
	std::vector<std::pair<std::string, std::optional<TmplValue>>> saved;
	saved.reserve(names.size());
	for (auto const &n: names) {
		auto const *prev = obj_find(ctx, n);
		saved.emplace_back(n, (prev != nullptr) ? std::optional<TmplValue>{*prev} : nullopt);
	}
	return saved;
}
static void restore_scope(
	TmplValue &ctx,
	std::vector<std::pair<std::string, std::optional<TmplValue>>> const &saved) {
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
	std::string template_dir;
	EnvironmentOptions options;
	std::unordered_map<std::string, Template> cache;
	mutable std::shared_mutex cache_mtx;

	struct MacroBinding {
		std::vector<std::string> params;
		std::vector<CompiledExpr> defaults;
		NodeList body;
	};
	Template parse(std::string const &name, std::string const &source) const;
	TmplValue eval_expr(std::string const &expr, TmplValue const &context) const;
	TmplValue eval_expr(CompiledExpr const &expr, TmplValue const &context) const;
	TmplValue eval_base(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_path(std::vector<CompiledPathSegment> const &path, TmplValue const &context) const;
	TmplValue apply_method(std::string const &name, TmplValue const &val, std::vector<CompiledExprPtr> const &args, TmplValue const &context) const;
	TmplValue apply_filter(CompiledFilter const &filter, TmplValue const &val, TmplValue const &context) const;
	static std::string value_to_string(TmplValue const &v);
	static bool is_truthy(TmplValue const &v);
	static constexpr int kMaxTemplateDepth = 256;
	std::string render_nodes(
		NodeList const &nodes,
		TmplValue context,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth = 0) const;
	std::string render_template(
		Template const &tmpl,
		TmplValue context,
		std::unordered_map<std::string, NodeList> const *child_blocks = nullptr,
		int depth = 0) const;
	expected<std::unordered_map<std::string, Template>, TemplateBuildReport> build_cache_from_directory() const;
	expected<void, TemplateBuildReport> reload_all_checked();
	void validate_links(std::unordered_map<std::string, Template> const &candidate, TemplateBuildReport &report) const;
	void reload_path(std::string const &path);
	void remove_path(std::string const &path);
	bool extension_allowed(fs::path const &path) const;
};
// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

Template Environment::Impl::parse(
	std::string const &name,
	std::string const &source) const {
	auto tokens = tokenize(source);
	Template tmpl;
	tmpl.name = name;
	struct ParseState {
		std::vector<Token> const &tokens;
		std::size_t pos = 0;
		[[nodiscard]] Token const &cur() const { return tokens[pos]; }
		[[nodiscard]] bool done() const { return pos >= tokens.size(); }
		void advance() { ++pos; }
	};
	ParseState state{tokens};

	std::function<NodeList(std::vector<std::string> const &, int)> parse_nodes;
	parse_nodes = [&](std::vector<std::string> const &end_tags, int depth) -> NodeList {
		if (depth > kMaxTemplateDepth) {
			throw std::runtime_error{"template parse recursion depth exceeded"};
		}
		NodeList nodes;
		auto const fail_missing_end = [&] {
			if (!end_tags.empty()) {
				throw std::runtime_error{format("template parse error: missing end tag (expected one of '{}')", end_tags.front())};
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
				nodes.push_back(make_shared<Node>(Node{ExprNode{tok.content, compile_expr(tok.content)}}));
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
				if (in_pos == std::string::npos) {
					throw std::runtime_error{"template parse error: missing 'in' in for tag"};
				}
				auto var_part = trim(tag.substr(4, in_pos - 4));
				auto iter_expr = trim(tag.substr(in_pos + 4));
				std::vector<std::string> vars;
				std::string_view vp{var_part};
				while (!vp.empty()) {
					auto cp = vp.find(',');
					auto vtok = (cp == std::string_view::npos) ? vp : vp.substr(0, cp);
					vars.push_back(trim(std::string{vtok}));
					if (cp == std::string_view::npos) {
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
						ForNode{vars, iter_expr, compile_expr(iter_expr), body}
                }));
			} else if (starts_with(tag, "if ")) {
				IfNode if_node;
				auto cond = trim(tag.substr(3));
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
						auto c = trim(t.substr(5));
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
				nodes.push_back(make_shared<Node>(Node{if_node}));
			} else if (starts_with(tag, "set ")) {
				auto eq = tag.find('=');
				if (eq == std::string::npos) {
					throw std::runtime_error{format("template parse error: set tag missing '=': {}", tag)};
				}
				auto var = trim(tag.substr(4, eq - 4));
				auto expr = trim(tag.substr(eq + 1));
				nodes.push_back(
					make_shared<Node>(Node{
						SetNode{var, expr, compile_expr(expr)}
                }));
				state.advance();
			} else if (starts_with(tag, "include ")) {
				auto inc_name = extract_string_arg(tag);
				nodes.push_back(make_shared<Node>(Node{IncludeNode{inc_name}}));
				state.advance();
			} else if (starts_with(tag, "macro ")) {
				auto paren = tag.find('(');
				std::string mname;
				std::vector<std::string> params;
				std::vector<std::string> defaults;
				if (paren != std::string::npos) {
					mname = trim(tag.substr(6, paren - 6));
					auto close = tag.find(')', paren);
					if (close != std::string::npos) {
						auto raw = split_args(tag.substr(paren + 1, close - paren - 1));
						for (auto &p: raw) {
							auto eq = p.find('=');
							if (eq != std::string::npos) {
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
						MacroNode{mname, params, defaults, compile_expr_list(defaults), body}
                }));
			} else if (starts_with(tag, "from ")) {
				auto rest = trim(tag.substr(5));
				std::string file;
				if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
					char const qc = rest.front();
					auto end = rest.find(qc, 1);
					if (end != std::string::npos) {
						file = rest.substr(1, end - 1);
						rest = trim(rest.substr(end + 1));
					}
				}
				if (starts_with(rest, "import ")) {
					rest = trim(rest.substr(7));
				}
				std::string nm, alias;
				auto as_pos = rest.find(" as ");
				if (as_pos != std::string::npos) {
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
				throw std::runtime_error{format("template parse error: unknown tag '{}'", tag)};
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
	std::string const &expr,
	TmplValue const &context) const {
	return eval_expr(compile_expr(expr), context);
}

TmplValue Environment::Impl::apply_method(
	std::string const &name,
	TmplValue const &val,
	std::vector<CompiledExprPtr> const &args,
	TmplValue const &context) const {
	auto eval_arg = [&](std::size_t idx) -> TmplValue {
		return idx < args.size() && args[idx] ? eval_expr(*args[idx], context) : TmplValue{};
	};
	if (name == "get" && val.is_object()) {
		if (args.empty()) {
			return {};
		}
		auto k = eval_arg(0);
		auto const *found = obj_find(val, value_to_string(k));
		if (found) {
			return *found;
		}
		return args.size() > 1 ? eval_arg(1) : TmplValue{};
	}
	if (name == "replace" && args.size() >= 2) {
		auto s = value_to_string(val);
		auto old_s = value_to_string(eval_arg(0));
		auto new_s = value_to_string(eval_arg(1));
		return TmplValue{str_replace_all(s, old_s, new_s)};
	}
	if (name == "title") {
		auto s = value_to_string(val);
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
		return TmplValue{move(s)};
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
	if (name == "strftime") {
		return TmplValue{value_to_string(val)};
	}
	if (name == "strip") {
		return TmplValue{trim(value_to_string(val))};
	}
	if (name == "startswith" && !args.empty()) {
		auto s = value_to_string(val);
		auto prefix = value_to_string(eval_arg(0));
		return TmplValue{s.compare(0, prefix.size(), prefix) == 0};
	}
	if (name == "split") {
		auto s = value_to_string(val);
		auto sep = !args.empty() ? value_to_string(eval_arg(0)) : std::string{" "};
		TmplValue arr{TmplValue::Array{}};
		std::size_t p = 0;
		while (p <= s.size()) {
			auto f = sep.empty() ? std::string::npos : s.find(sep, p);
			if (f == std::string::npos) {
				arr.push_back(TmplValue{s.substr(p)});
				break;
			}
			arr.push_back(TmplValue{s.substr(p, f - p)});
			p = f + sep.size();
		}
		return arr;
	}
	if (name == "keys" && val.is_object()) {
		TmplValue keys{TmplValue::Array{}};
		for (auto const &kv: val.as_object()) {
			keys.push_back(TmplValue{kv.first});
		}
		return keys;
	}
	if (name == "values" && val.is_object()) {
		TmplValue values{TmplValue::Array{}};
		for (auto const &kv: val.as_object()) {
			values.push_back(kv.second);
		}
		return values;
	}
	if (name == "items" && val.is_object()) {
		TmplValue items{TmplValue::Array{}};
		for (auto const &kv: val.as_object()) {
			TmplValue pair{TmplValue::Array{}};
			pair.push_back(TmplValue{kv.first});
			pair.push_back(kv.second);
			items.push_back(move(pair));
		}
		return items;
	}
	return val;
}
TmplValue Environment::Impl::eval_path(
	std::vector<CompiledPathSegment> const &path,
	TmplValue const &context) const {
	TmplValue owned;
	bool use_owned = false;
	TmplValue const *cur = &context;
	auto set_owned = [&](TmplValue v) {
		owned = move(v);
		cur = &owned;
		use_owned = true;
	};
	for (auto const &seg: path) {
		switch (seg.kind) {
		case CompiledPathSegmentKind::field:
			if (cur->is_object()) {
				auto const *found = obj_find(*cur, seg.name);
				if (!found) {
					return {};
				}
				set_owned(*found);
			} else if (!(cur->is_string() && seg.name == "value")) {
				return {};
			}
			break;
		case CompiledPathSegmentKind::index:
			{
				auto idx_val = seg.expr ? eval_expr(*seg.expr, context) : TmplValue{};
				if (cur->is_array() && idx_val.is_int()) {
					auto idx = idx_val.as<std::int64_t>();
					auto const &arr = cur->as_array();
					if (idx < 0) {
						idx += static_cast<std::int64_t>(arr.size());
					}
					if (idx >= 0 && static_cast<std::size_t>(idx) < arr.size()) {
						set_owned(arr[static_cast<std::size_t>(idx)]);
					} else {
						return {};
					}
				} else if (cur->is_object() && idx_val.is_string()) {
					auto const *found = obj_find(*cur, idx_val.as<std::string_view>());
					if (!found) {
						return {};
					}
					set_owned(*found);
				} else {
					return {};
				}
				break;
			}
		case CompiledPathSegmentKind::slice:
			if (cur->is_string()) {
				auto str = std::string(cur->as<std::string_view>());
				std::int64_t start = 0;
				std::int64_t end = static_cast<std::int64_t>(str.size());
				if (seg.start) {
					auto sv = eval_expr(*seg.start, context);
					if (sv.is_int()) {
						start = sv.as<std::int64_t>();
						if (start < 0) {
							start = max<std::int64_t>(0, static_cast<std::int64_t>(str.size()) + start);
						}
					}
				}
				if (seg.end) {
					auto ev = eval_expr(*seg.end, context);
					if (ev.is_int()) {
						end = ev.as<std::int64_t>();
						if (end < 0) {
							end = max<std::int64_t>(0, static_cast<std::int64_t>(str.size()) + end);
						}
					}
				}
				start = std::clamp<std::int64_t>(start, 0, static_cast<std::int64_t>(str.size()));
				end = std::clamp<std::int64_t>(end, 0, static_cast<std::int64_t>(str.size()));
				set_owned(TmplValue{str.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(max<std::int64_t>(0, end - start)))});
			} else {
				return {};
			}
			break;
		case CompiledPathSegmentKind::method:
			set_owned(apply_method(seg.name, *cur, seg.args, context));
			break;
		}
	}
	return use_owned ? owned : *cur;
}
TmplValue Environment::Impl::eval_base(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	switch (base.kind) {
	case CompiledBaseKind::literal:
		switch (base.literal.kind) {
		case CompiledLiteralKind::none    : return {};
		case CompiledLiteralKind::boolean : return TmplValue{base.literal.boolean};
		case CompiledLiteralKind::integer : return TmplValue{base.literal.integer};
		case CompiledLiteralKind::floating: return TmplValue{base.literal.floating};
		case CompiledLiteralKind::string  : return TmplValue{base.literal.string};
		}
		return {};
	case CompiledBaseKind::array:
	case CompiledBaseKind::tuple:
		{
			TmplValue arr{TmplValue::Array{}};
			for (auto const &item: base.operands) {
				arr.push_back(item ? eval_expr(*item, context) : TmplValue{});
			}
			return arr;
		}
	case CompiledBaseKind::object:
		{
			TmplValue obj{TmplValue::Object{}};
			for (auto const &item: base.object_items) {
				obj.set(item.key, item.value ? eval_expr(*item.value, context) : TmplValue{});
			}
			return obj;
		}
	case CompiledBaseKind::group:
		return !base.operands.empty() && base.operands[0] ? eval_expr(*base.operands[0], context) : TmplValue{};
	case CompiledBaseKind::path:
		return eval_path(base.path, context);
	case CompiledBaseKind::unary_not:
		return TmplValue{!(base.operands.empty() || !base.operands[0] ? false : is_truthy(eval_expr(*base.operands[0], context)))};
	case CompiledBaseKind::binary_or:
		{
			auto left = !base.operands.empty() && base.operands[0] ? eval_expr(*base.operands[0], context) : TmplValue{};
			if (is_truthy(left)) {
				return left;
			}
			return base.operands.size() > 1 && base.operands[1] ? eval_expr(*base.operands[1], context) : TmplValue{};
		}
	case CompiledBaseKind::binary_and:
		{
			auto left = !base.operands.empty() && base.operands[0] ? eval_expr(*base.operands[0], context) : TmplValue{};
			if (!is_truthy(left)) {
				return left;
			}
			return base.operands.size() > 1 && base.operands[1] ? eval_expr(*base.operands[1], context) : TmplValue{};
		}
	case CompiledBaseKind::compare:
		{
			auto left = !base.operands.empty() && base.operands[0] ? eval_expr(*base.operands[0], context) : TmplValue{};
			auto right = base.operands.size() > 1 && base.operands[1] ? eval_expr(*base.operands[1], context) : TmplValue{};
			switch (base.compare_op) {
			case CompiledCompareOp::eq: return TmplValue{left == right};
			case CompiledCompareOp::ne: return TmplValue{left != right};
			case CompiledCompareOp::le:
			case CompiledCompareOp::ge:
			case CompiledCompareOp::lt:
			case CompiledCompareOp::gt:
				{
					double const lv = left.is_int()   ? static_cast<double>(left.as<std::int64_t>()) :
								  left.is_uint()  ? static_cast<double>(left.as<std::uint64_t>()) :
								  left.is_float() ? left.as<double>() :
												0.0;
					double const rv = right.is_int()   ? static_cast<double>(right.as<std::int64_t>()) :
								  right.is_uint()  ? static_cast<double>(right.as<std::uint64_t>()) :
								  right.is_float() ? right.as<double>() :
												 0.0;
					if (base.compare_op == CompiledCompareOp::le) {
						return TmplValue{lv <= rv};
					}
					if (base.compare_op == CompiledCompareOp::ge) {
						return TmplValue{lv >= rv};
					}
					if (base.compare_op == CompiledCompareOp::lt) {
						return TmplValue{lv < rv};
					}
					return TmplValue{lv > rv};
				}
			case CompiledCompareOp::in:
				if (right.is_array()) {
					for (auto const &item: right.as_array()) {
						if (item == left) {
							return TmplValue{true};
						}
					}
					return TmplValue{false};
				}
				if (right.is_string() && left.is_string()) {
					return TmplValue{right.as<std::string_view>().find(left.as<std::string_view>()) != std::string_view::npos};
				}
				return TmplValue{false};
			}
			return {};
		}
	case CompiledBaseKind::concat:
		{
			auto left = !base.operands.empty() && base.operands[0] ? eval_expr(*base.operands[0], context) : TmplValue{};
			auto right = base.operands.size() > 1 && base.operands[1] ? eval_expr(*base.operands[1], context) : TmplValue{};
			return TmplValue{value_to_string(left) + value_to_string(right)};
		}
	}
	return {};
}
TmplValue Environment::Impl::eval_expr(
	CompiledExpr const &expr,
	TmplValue const &context) const {
	if (expr.base.empty()) {
		return {};
	}

	// NOLINTNEXTLINE(misc-no-recursion)
	auto eval_base = [&](std::string const &base) -> TmplValue {
		auto b = trim(base);
		if (b.empty()) {
			return {};
		}

		if ((b.front() == '"' && b.back() == '"') || (b.front() == '\'' && b.back() == '\'')) {
			return TmplValue{b.substr(1, b.size() - 2)};
		}

		if (std::isdigit(static_cast<unsigned char>(b[0])) || (b[0] == '-' && b.size() > 1)) {
			try {
				if (b.find('.') != std::string::npos) {
					return TmplValue{std::stod(b)};
				}
				return TmplValue{static_cast<std::int64_t>(std::stoll(b))};
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
				if (colon != std::string::npos) {
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
			for (std::size_t i = 0; i < b.size(); ++i) {
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
			for (std::size_t i = 0; i < b.size(); ++i) {
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
			static std::vector<std::pair<std::string, int>> const ops = {
				{" == ", 0},
				{" != ", 1},
				{" <= ", 2},
				{" >= ", 3},
				{ " < ", 4},
				{ " > ", 5},
				{" in ", 6}
            };
			auto find_top_level = [&](std::string_view haystack, std::string_view needle) -> std::size_t {
				int d = 0;
				bool in_s3 = false;
				char sq3 = 0;
				for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
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
				return std::string_view::npos;
			};
			for (auto &[op, code]: ops) {
				auto p = find_top_level(b, op);
				if (p != std::string_view::npos) {
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
							double const lv = left.is_int()   ? static_cast<double>(left.as<std::int64_t>()) :
											  left.is_uint()  ? static_cast<double>(left.as<std::uint64_t>()) :
											  left.is_float() ? left.as<double>() :
																0.0;
							double const rv = right.is_int()   ? static_cast<double>(right.as<std::int64_t>()) :
											  right.is_uint()  ? static_cast<double>(right.as<std::uint64_t>()) :
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
								return TmplValue{right.as<std::string_view>().find(left.as<std::string_view>()) != std::string_view::npos};
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
			for (std::size_t i = 0; i < b.size(); ++i) {
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
			std::string remaining = b;

			while (!remaining.empty()) {
				auto bracket = remaining.find('[');
				auto dot = remaining.find('.');
				auto paren = remaining.find('(');

				auto next_sep = min({bracket, dot, paren, remaining.size()});

				if (next_sep == 0 && bracket == 0) {
					auto close = remaining.find(']', 1);
					if (close == std::string::npos) {
						return {};
					}
					auto idx_str = trim(remaining.substr(1, close - 1));
					if (auto colon = idx_str.find(':'); colon != std::string::npos) {
						if (cur->is_string()) {
							auto s = std::string(cur->as<std::string_view>());
							auto start_s = trim(idx_str.substr(0, colon));
							auto end_s = trim(idx_str.substr(colon + 1));
							std::int64_t start = 0;
							std::int64_t end = static_cast<std::int64_t>(s.size());
							if (!start_s.empty()) {
								auto sv = eval_expr(start_s, context);
								if (sv.is_int()) {
									start = sv.as<std::int64_t>();
									if (start < 0) {
										start = max<std::int64_t>(0, static_cast<std::int64_t>(s.size()) + start);
									}
								}
							}
							if (!end_s.empty()) {
								auto ev = eval_expr(end_s, context);
								if (ev.is_int()) {
									end = ev.as<std::int64_t>();
									if (end < 0) {
										end = max<std::int64_t>(0, static_cast<std::int64_t>(s.size()) + end);
									}
								}
							}
							start = std::clamp<std::int64_t>(start, 0, static_cast<std::int64_t>(s.size()));
							end = std::clamp<std::int64_t>(end, 0, static_cast<std::int64_t>(s.size()));
							set_owned(
								TmplValue{s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(max<std::int64_t>(0, end - start)))});
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
						auto idx = idx_val.as<std::int64_t>();
						auto const &arr = cur->as_array();
						if (idx < 0) {
							idx += static_cast<std::int64_t>(arr.size());
						}
						if (idx >= 0 && static_cast<std::size_t>(idx) < arr.size()) {
							set_owned(arr[static_cast<std::size_t>(idx)]);
						} else {
							return {};
						}
					} else if (cur->is_object() && idx_val.is_string()) {
						auto const *found = obj_find(*cur, idx_val.as<std::string_view>());
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

				std::string const key = remaining.substr(0, next_sep);
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
					// Find matching ')' respecting nesting and std::string literals.
					std::size_t close = std::string::npos;
					{
						int d = 0;
						bool in_s2 = false;
						char sq2 = 0;
						for (std::size_t ci = 0; ci < remaining.size(); ++ci) {
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
					if (close == std::string::npos) {
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
						std::size_t p = 0;
						while (p <= s.size()) {
							auto f = sep.empty() ? std::string::npos : s.find(sep, p);
							if (f == std::string::npos) {
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

	TmplValue result = expr.compiled_base ? this->eval_base(*expr.compiled_base, context) : eval_base(expr.base);

	for (auto const &filter: expr.filters) {
		result = apply_filter(filter, result, context);
	}

	return result;
}
// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
TmplValue Environment::Impl::apply_filter(
	CompiledFilter const &filter,
	TmplValue const &val,
	TmplValue const &context) const {
	auto const &name = filter.name;
	auto const &args = filter.args;
	auto eval_arg = [&](std::size_t idx) -> TmplValue {
		if (idx < filter.compiled_args.size() && filter.compiled_args[idx]) {
			return eval_expr(*filter.compiled_args[idx], context);
		}
		return idx < args.size() ? eval_expr(args[idx], context) : TmplValue{};
	};
	if (name == "length" || name == "count") {
		if (val.is_array()) {
			return TmplValue{static_cast<std::int64_t>(val.as_array().size())};
		}
		if (val.is_string()) {
			return TmplValue{static_cast<std::int64_t>(val.as<std::string_view>().size())};
		}
		if (val.is_object()) {
			return TmplValue{static_cast<std::int64_t>(val.as_object().size())};
		}
		return TmplValue{std::int64_t{0}};
	}
	if (name == "S") {
		return TmplValue{value_to_string(val)};
	}
	if (name == "int") {
		if (val.is_int() || val.is_uint()) {
			return val;
		}
		if (val.is_float()) {
			return TmplValue{static_cast<std::int64_t>(val.as<double>())};
		}
		if (val.is_string()) {
			auto s = std::string(val.as<std::string_view>());
			try {
				return TmplValue{static_cast<std::int64_t>(std::stoll(s))};
			} catch (exception const &e) {
				eprintln(format("template filter int: failed to parse '{}': {}", s, e.what()));
				return TmplValue{std::int64_t{0}};
			}
		}
		return TmplValue{std::int64_t{0}};
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
			auto old_s = value_to_string(eval_arg(0));
			auto new_s = value_to_string(eval_arg(1));
			return TmplValue{str_replace_all(s, old_s, new_s)};
		}
		return TmplValue{move(s)};
	}
	if (name == "default" || name == "d") {
		if (!is_truthy(val) && !args.empty()) {
			return eval_arg(0);
		}
		return val;
	}
	if (name == "join") {
		if (val.is_array()) {
			auto sep = !args.empty() ? value_to_string(eval_arg(0)) : "";
			auto const &arr = val.as_array();
			std::string result;
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
			for (std::size_t i = 1; i < arr.size(); ++i) {
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
			auto attr = value_to_string(eval_arg(0));
			auto test = value_to_string(eval_arg(1));
			auto test_val = eval_arg(2);
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
			auto attr_name = value_to_string(eval_arg(0));
			auto const *found = obj_find(val, attr_name);
			return (found != nullptr) ? *found : TmplValue{};
		}
		return {};
	}
	if (name == "e" || name == "escape") {
		auto s = value_to_string(val);
		std::string result;
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

std::string Environment::Impl::value_to_string(
	TmplValue const &v) {
	if (v.is_null()) {
		return "";
	}
	if (v.is_string()) {
		return std::string(v.as<std::string_view>());
	}
	if (v.is_int()) {
		return to_string(v.as<std::int64_t>());
	}
	if (v.is_uint()) {
		return to_string(v.as<std::uint64_t>());
	}
	if (v.is_float()) {
		auto s = to_string(v.as<double>());
		auto dot = s.find('.');
		if (dot != std::string::npos) {
			auto last = s.find_last_not_of('0');
			if (last != std::string::npos && last > dot) {
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
		return v.as<std::int64_t>() != 0;
	}
	if (v.is_uint()) {
		return v.as<std::uint64_t>() != 0;
	}
	if (v.is_float()) {
		return v.as<double>() != 0.0;
	}
	if (v.is_string()) {
		return !v.as<std::string_view>().empty();
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
std::string Environment::Impl::render_nodes(
	NodeList const &nodes,
	TmplValue context,
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
							out += render_nodes(body, context, blocks, macros, depth + 1);
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
						throw std::runtime_error{format("template error: included template '{}' not found", n.name)};
					}
					out += render_template(it->second, context, blocks, depth + 1);
				} else if constexpr (std::is_same_v<T, SetNode>) {
					auto val = eval_expr(n.compiled, context);
					if (context.is_object()) {
						context.set(n.var, move(val));
					}
				} else if constexpr (std::is_same_v<T, ForNode>) {
					auto iter_val = eval_expr(n.compiled_iter, context);
					if (iter_val.is_array()) {
						auto saved = save_scope(context, n.vars);
						auto const *prev_loop = obj_find(context, "loop");
						std::optional<TmplValue> saved_loop = prev_loop ? std::optional<TmplValue>{*prev_loop} : nullopt;
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
						if (is_truthy(eval_expr(branch.compiled_condition, context))) {
							out += render_nodes(branch.body, context, blocks, macros, depth + 1);
							break;
						}
					}
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					(*macros)[n.name] = {n.params, n.compiled_defaults, n.body};
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					auto it_tmpl = cache.find(n.file);
					if (it_tmpl == cache.end()) {
						throw std::runtime_error{format("template error: imported file '{}' not found", n.file)};
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
						throw std::runtime_error{format("template error: macro '{}' not found in '{}'", n.name, n.file)};
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
	std::unordered_map<std::string, NodeList> const *child_blocks,
	int depth) const {
	if (depth > kMaxTemplateDepth) {
		throw std::runtime_error{"template render recursion depth exceeded"};
	}
	if (!tmpl.extends_name.empty()) {
		auto it = cache.find(tmpl.extends_name);
		if (it == cache.end()) {
			throw std::runtime_error{"template not found: " + tmpl.extends_name};
		}

		for (auto &node: tmpl.nodes) {
			std::visit(
				[&](auto &n) {
					using T = std::decay_t<decltype(n)>;
					if constexpr (std::is_same_v<T, SetNode>) {
						auto val = eval_expr(n.compiled, context);
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
	std::vector<TemplateSourceLocation> stack = {}) {
	report.diagnostics.push_back(TemplateDiagnostic{
		.severity = TemplateDiagnosticSeverity::error,
		.phase = phase,
		.location = move(location),
		.stack = move(stack),
		.code = move(code),
		.message = move(message),
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
static void collect_direct_node_deps(
	NodeList const &nodes,
	std::vector<std::string> &deps) {
	for (auto const &node: nodes) {
		std::visit(
			[&](auto const &n) {
				using T = std::decay_t<decltype(n)>;
				if constexpr (std::is_same_v<T, IncludeNode>) {
					deps.push_back(n.name);
				} else if constexpr (std::is_same_v<T, FromImportNode>) {
					deps.push_back(n.file);
				} else if constexpr (std::is_same_v<T, BlockNode>) {
					collect_direct_node_deps(n.body, deps);
				} else if constexpr (std::is_same_v<T, ForNode>) {
					collect_direct_node_deps(n.body, deps);
				} else if constexpr (std::is_same_v<T, IfNode>) {
					for (auto const &branch: n.branches) {
						collect_direct_node_deps(branch.body, deps);
					}
				} else if constexpr (std::is_same_v<T, MacroNode>) {
					collect_direct_node_deps(n.body, deps);
				}
			},
			node->data);
	}
}
static std::vector<std::string> collect_direct_template_deps(
	Template const &tmpl) {
	std::vector<std::string> deps;
	if (!tmpl.extends_name.empty()) {
		deps.push_back(tmpl.extends_name);
	}
	collect_direct_node_deps(tmpl.nodes, deps);
	return deps;
}
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
								format("include '{}' was not loaded", n.name),
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
								format("import file '{}' was not loaded", n.file),
								{template_location(owner), template_location(n.file)});
						} else if (!node_list_has_top_level_macro(it->second.nodes, n.name)) {
							add_template_diag(
								report,
								TemplateDiagnosticPhase::link,
								template_location(owner),
								"import_macro_not_found",
								format("macro '{}' was not found in '{}'", n.name, n.file),
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
				format("parent template '{}' was not loaded", tmpl.extends_name),
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
					format("template dependency cycle reaches '{}'", name),
					move(diag_stack));
			}
			return;
		}
		visit_state[name] = 1;
		stack.push_back(name);
		auto it = candidate.find(name);
		if (it != candidate.end()) {
			for (auto const &dep: collect_direct_template_deps(it->second)) {
				if (candidate.contains(dep)) {
					dfs(dep);
				}
			}
		}
		stack.pop_back();
		visit_state[name] = 2;
	};
	for (auto const &[name, _]: candidate) {
		dfs(name);
	}
}
expected<std::unordered_map<std::string, Template>, TemplateBuildReport> Environment::Impl::build_cache_from_directory() const {
	TemplateBuildReport report;
	std::unordered_map<std::string, Template> parsed;
	fs::path const dir{template_dir};
	std::error_code ec;
	if (!fs::exists(dir, ec)) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"directory_not_found",
			format("template directory '{}' does not exist", dir.string()));
		return unexpected{move(report)};
	}
	if (!fs::is_directory(dir, ec)) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"not_directory",
			format("template path '{}' is not a directory", dir.string()));
		return unexpected{move(report)};
	}
	std::vector<fs::path> files;
	try {
		for (auto const &entry: fs::directory_iterator(dir)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			if (!extension_allowed(entry.path())) {
				continue;
			}
			files.push_back(entry.path());
		}
	} catch (exception const &e) {
		add_template_diag(
			report,
			TemplateDiagnosticPhase::io,
			template_location(std::string{}, dir.string()),
			"directory_scan_failed",
			format("failed to scan template directory '{}': {}", dir.string(), e.what()));
		return unexpected{move(report)};
	}
	std::sort(files.begin(), files.end(), [](fs::path const &a, fs::path const &b) {
		return a.string() < b.string();
	});
	report.templates_seen = files.size();
	for (auto const &path: files) {
		auto name = path.filename().string();
		auto contents = blocking_read_text_file(path.string());
		if (!contents) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::io,
				template_location(name, path.string()),
				"read_failed",
				format("failed to read template '{}': {}", path.string(), contents.error().what()));
			continue;
		}
		try {
			parsed[name] = parse(name, *contents);
			++report.templates_compiled;
		} catch (exception const &e) {
			add_template_diag(
				report,
				TemplateDiagnosticPhase::parse,
				template_location(name, path.string()),
				"parse_failed",
				format("failed to parse template '{}': {}", name, e.what()));
		}
	}
	if (!report.ok()) {
		return unexpected{move(report)};
	}
	validate_links(parsed, report);
	if (!report.ok()) {
		return unexpected{move(report)};
	}
	return parsed;
}
expected<void, TemplateBuildReport> Environment::Impl::reload_all_checked() {
	auto candidate = build_cache_from_directory();
	if (!candidate) {
		return unexpected{move(candidate.error())};
	}
	{
		std::unique_lock const lk{cache_mtx};
		cache = move(*candidate);
	}
	return {};
}
void Environment::Impl::reload_path(
	std::string const &path) {
	fs::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	(void)reload_all_checked();
}
void Environment::Impl::remove_path(
	std::string const &path) {
	fs::path const p{path};
	if (!extension_allowed(p)) {
		return;
	}
	(void)reload_all_checked();
}
Environment::Environment(
	std::string const &template_dir)
	: impl_(make_unique<Impl>()) {
	impl_->template_dir = template_dir;
}
Environment::Environment(
	std::string const &template_dir,
	EnvironmentOptions options)
	: impl_(make_unique<Impl>()) {
	impl_->template_dir = template_dir;
	impl_->options = move(options);
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
		throw TemplateBuildError{move(res.error())};
	}
}
void Environment::blocking_reload_all() {
	auto res = blocking_reload_all_checked();
	if (!res) {
		throw TemplateBuildError{move(res.error())};
	}
}
expected<void, TemplateBuildReport> Environment::blocking_load_all_checked() {
	return impl_->reload_all_checked();
}
expected<void, TemplateBuildReport> Environment::blocking_reload_all_checked() {
	return impl_->reload_all_checked();
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
	return impl_->render_template(it->second, ctx);
}
std::string Environment::render(
	std::string const &name,
	NodeRef ctx) const {
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
	return impl_->render_template(tmpl, ctx);
}
std::string Environment::render_string(
	std::string const &source,
	NodeRef ctx) const {
	return render_string(source, node_to_tmpl(ctx));
}

} // namespace tmpl

export namespace conflux::templates {
using ::tmpl::BlockNode;
using ::tmpl::CompiledExpr;
using ::tmpl::CompiledFilter;
using ::tmpl::CompiledMacroArg;
using ::tmpl::CompiledMacroCall;
using ::tmpl::Environment;
using ::tmpl::EnvironmentOptions;
using ::tmpl::ExprNode;
using ::tmpl::ExtendsNode;
using ::tmpl::ForNode;
using ::tmpl::FromImportNode;
using ::tmpl::IfNode;
using ::tmpl::IncludeNode;
using ::tmpl::MacroNode;
using ::tmpl::Node;
using ::tmpl::NodeList;
using ::tmpl::NodePtr;
using ::tmpl::SetNode;
using ::tmpl::Template;
using ::tmpl::TemplateBuildError;
using ::tmpl::TemplateBuildReport;
using ::tmpl::TemplateDiagnostic;
using ::tmpl::TemplateDiagnosticPhase;
using ::tmpl::TemplateDiagnosticSeverity;
using ::tmpl::TemplateSourceLocation;
using ::tmpl::TextNode;
using ::tmpl::TmplValue;
}
