module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

using conflux::utils::append_json_string_fallback;
using conflux::utils::eprintln;
using conflux::utils::trim;

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
		return std::format("template build ok: {} templates compiled", templates_compiled);
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
			out += std::format(":{}:{}", loc.line, loc.column);
		}
		out += std::format(
			": {} {}.{}: {}",
			diagnostic_severity_name(d.severity),
			diagnostic_phase_name(d.phase),
			d.code.empty() ? "unknown" : d.code,
			d.message);
		if (!d.check_label.empty()) {
			out += std::format(" [check:{}]", d.check_label);
		}
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
	, report{std::move(report_)} {}

// ---------------------------------------------------------------------------
// Internal mutable value type for template context
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
std::string TmplValue::dump() const {
	if (is_null()) {
		return "null";
	}
	if (is_bool()) {
		return std::get<bool>(data) ? "true" : "false";
	}
	if (is_int()) {
		return std::to_string(std::get<std::int64_t>(data));
	}
	if (is_uint()) {
		return std::to_string(std::get<std::uint64_t>(data));
	}
	if (is_float()) {
		auto s = std::to_string(std::get<double>(data));
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
		std::string out;
		append_json_string_fallback(out, std::get<std::string>(data));
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
			append_json_string_fallback(out, k);
			out += ':';
			out += v.dump();
		}
		out += '}';
		return out;
	}
	return "null";
}
// NOLINTNEXTLINE(misc-no-recursion)
TmplValue node_to_tmpl(
	json::NodeRef node) {
	switch (node.kind()) {
	case json::JsonKind::null   : return {};
	case json::JsonKind::boolean: return TmplValue{*node.as_bool()};
	case json::JsonKind::number:
		{
			auto num = *node.as_number();
			if (num.form() == json::JsonNumberForm::integer) {
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
	case json::JsonKind::string: return TmplValue{std::string{*node.as_string()}};
	case json::JsonKind::array:
		{
			auto arr = *node.as_array();
			TmplValue::Array vec;
			vec.reserve(arr.size());
			std::ranges::transform(arr.elements(), std::back_inserter(vec), node_to_tmpl);
			return TmplValue{std::move(vec)};
		}
	case json::JsonKind::object:
		{
			auto obj = *node.as_object();
			TmplValue::Object pairs;
			pairs.reserve(obj.size());
			std::ranges::transform(obj.members(), std::back_inserter(pairs), [](auto member) {
				auto [name, val] = member;
				return std::pair<std::string, TmplValue>{std::string{name}, node_to_tmpl(val)};
			});
			return TmplValue{std::move(pairs)};
		}
	default: return {};
	}
}
// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

std::string str_replace_all(
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
std::string str_capitalize(
	std::string s) {
	if (!s.empty()) {
		s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	}
	return s;
}
std::vector<std::string> split_args(
	std::string_view s) {
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
			args.push_back(std::string{trim(current)});
			current.clear();
			continue;
		}
		current += c;
	}
	if (!current.empty()) {
		args.push_back(std::string{trim(current)});
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
std::size_t find_matching_pair(
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
template<typename Match>
static std::size_t find_top_level_match(
	std::string_view haystack,
	std::size_t max_width,
	Match match) noexcept {
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
		if (depth == 0 && i + max_width <= haystack.size() && match(i)) {
			return i;
		}
	}
	return std::string_view::npos;
}
std::size_t find_top_level_token(
	std::string_view haystack,
	std::string_view needle) noexcept {
	return find_top_level_match(haystack, needle.size(), [&](std::size_t i) noexcept {
		return haystack.substr(i, needle.size()) == needle;
	});
}
std::size_t find_top_level_char(
	std::string_view haystack,
	char needle) noexcept {
	return find_top_level_match(haystack, 1, [&](std::size_t i) noexcept { return haystack[i] == needle; });
}
template<typename Fn>
static bool split_top_level_for_each(
	std::string_view haystack,
	char needle,
	Fn fn) {
	std::size_t start = 0;
	while (start <= haystack.size()) {
		auto const rel = find_top_level_char(haystack.substr(start), needle);
		auto const end = rel == std::string_view::npos ? haystack.size() : start + rel;
		if (!std::invoke(fn, haystack.substr(start, end - start))) {
			return false;
		}
		if (rel == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}
static CompiledExprPtr compile_expr_ptr(
	std::string_view expr) {
	return std::make_shared<CompiledExpr>(compile_expr(std::string{expr}));
}
static std::vector<CompiledExprPtr> compile_expr_ptr_list(
	std::vector<std::string> const &exprs) {
	std::vector<CompiledExprPtr> out;
	out.reserve(exprs.size());
	std::ranges::transform(exprs, std::back_inserter(out), compile_expr_ptr);
	return out;
}
static std::optional<CompiledLiteral> compile_literal(
	std::string_view expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return std::nullopt;
	}
	if (is_quoted_string(b)) {
		return CompiledLiteral{.kind = CompiledLiteralKind::string, .string = std::string{b.substr(1, b.size() - 2)}};
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
				auto value = std::stod(std::string{b}, &parsed);
				if (parsed == b.size()) {
					return CompiledLiteral{.kind = CompiledLiteralKind::floating, .floating = value};
				}
			} else {
				auto value = std::stoll(std::string{b}, &parsed);
				if (parsed == b.size()) {
					return CompiledLiteral{
						.kind = CompiledLiteralKind::integer,
						.integer = static_cast<std::int64_t>(value)};
				}
			}
		} catch (std::exception const &) { return std::nullopt; }
	}
	return std::nullopt;
}
static std::optional<CompiledPathSegment> compile_path_method(
	std::string_view name,
	std::string_view remaining) {
	if (remaining.empty() || remaining.front() != '(') {
		return std::nullopt;
	}
	auto close = find_matching_pair(remaining, 0, '(', ')');
	if (close == std::string_view::npos) {
		return std::nullopt;
	}
	auto raw_args = split_args(std::string{remaining.substr(1, close - 1)});
	return CompiledPathSegment{
		.kind = CompiledPathSegmentKind::method,
		.name = std::string{name},
		.args = compile_expr_ptr_list(raw_args),
	};
}
static std::optional<CompiledBaseExpr> compile_path_base(
	std::string_view expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return std::nullopt;
	}
	CompiledBaseExpr out;
	out.kind = CompiledBaseKind::path;
	out.source = std::string{b};
	std::string_view remaining{b};
	while (!remaining.empty()) {
		auto bracket = remaining.find('[');
		auto dot = remaining.find('.');
		auto paren = remaining.find('(');
		auto next_sep = std::min({bracket, dot, paren, remaining.size()});
		if (next_sep == 0 && bracket == 0) {
			auto close = find_matching_pair(remaining, 0, '[', ']');
			if (close == std::string_view::npos) {
				return std::nullopt;
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
				out.path.push_back(std::move(seg));
			} else {
				out.path.push_back(
					CompiledPathSegment{
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
			out.path.push_back(CompiledPathSegment{.kind = CompiledPathSegmentKind::field, .name = std::string{key}});
		}
		if (is_method_call) {
			auto method = compile_path_method(key, remaining);
			if (!method) {
				return std::nullopt;
			}
			auto close = find_matching_pair(remaining, 0, '(', ')');
			out.path.push_back(std::move(*method));
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
	std::string_view expr) {
	auto b = trim(expr);
	if (b.empty()) {
		return std::nullopt;
	}
	if (auto literal = compile_literal(b); literal) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::literal;
		out.source = std::string{b};
		out.literal = std::move(*literal);
		return out;
	}
	if (outer_pair_wraps(b, '[', ']')) {
		auto inner = trim(std::string_view{b}.substr(1, b.size() - 2));
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::array;
		out.source = std::string{b};
		if (!inner.empty()) {
			out.operands = compile_expr_ptr_list(split_args(inner));
		}
		return out;
	}
	if (outer_pair_wraps(b, '(', ')')) {
		auto inner = trim(std::string_view{b}.substr(1, b.size() - 2));
		CompiledBaseExpr out;
		out.source = std::string{b};
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
		out.source = std::string{b};
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
				out.object_items.push_back(CompiledObjectItem{std::string{key}, compile_expr_ptr(val)});
			}
		}
		return out;
	}
	if (auto p = find_top_level_token(b, " or "); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::binary_or;
		out.source = std::string{b};
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 4))));
		return out;
	}
	if (auto p = find_top_level_token(b, " and "); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::binary_and;
		out.source = std::string{b};
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 5))));
		return out;
	}
	if (b.size() > 4 && b.substr(0, 4) == "not ") {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::unary_not;
		out.source = std::string{b};
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(4))));
		return out;
	}
	static constexpr auto ops = std::to_array<std::pair<std::string_view, CompiledCompareOp>>({
		{" == ", CompiledCompareOp::eq},
		{" != ", CompiledCompareOp::ne},
		{" <= ", CompiledCompareOp::le},
		{" >= ", CompiledCompareOp::ge},
		{ " < ", CompiledCompareOp::lt},
		{ " > ", CompiledCompareOp::gt},
		{" in ", CompiledCompareOp::in},
	});
	for (auto const &[op, code]: ops) {
		if (auto p = find_top_level_token(b, op); p != std::string_view::npos) {
			CompiledBaseExpr out;
			out.kind = CompiledBaseKind::compare;
			out.source = std::string{b};
			out.compare_op = code;
			out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
			out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + op.size()))));
			return out;
		}
	}
	if (auto p = find_top_level_char(b, '~'); p != std::string_view::npos) {
		CompiledBaseExpr out;
		out.kind = CompiledBaseKind::concat;
		out.source = std::string{b};
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(0, p))));
		out.operands.push_back(compile_expr_ptr(trim(std::string_view{b}.substr(p + 1))));
		return out;
	}
	return compile_path_base(b);
}
static std::optional<CompiledMacroCall> compile_macro_call(
	std::string_view expr) {
	auto e = trim(expr);
	auto paren = e.find('(');
	if (paren == std::string::npos || e.empty() || e.back() != ')') {
		return std::nullopt;
	}
	auto name = trim(e.substr(0, paren));
	if (!is_template_identifier(name)) {
		return std::nullopt;
	}
	CompiledMacroCall call;
	call.name = std::string{name};
	auto raw_args = split_args(e.substr(paren + 1, e.size() - paren - 2));
	call.args.reserve(raw_args.size());
	for (auto &arg: raw_args) {
		std::size_t eq = 0;
		while (eq < arg.size() && (std::isalnum(static_cast<unsigned char>(arg[eq])) || arg[eq] == '_')) {
			++eq;
		}
		if (eq > 0 && eq < arg.size() && arg[eq] == '=' && (eq + 1 >= arg.size() || arg[eq + 1] != '=')) {
			auto expr_part = trim(std::string_view{arg}.substr(eq + 1));
			call.args.push_back(
				CompiledMacroArg{
					std::string{trim(std::string_view{arg}.substr(0, eq))},
					std::string{expr_part},
					compile_expr_ptr(expr_part),
					true});
		} else {
			auto expr_part = trim(arg);
			call.args.push_back(CompiledMacroArg{{}, std::string{expr_part}, compile_expr_ptr(expr_part), false});
		}
	}
	return call;
}
CompiledExpr compile_expr(
	std::string const &expr) {
	CompiledExpr compiled;
	compiled.source = std::string{trim(expr)};
	if (compiled.source.empty()) {
		return compiled;
	}
	std::vector<std::string> pipe_parts;
	split_top_level_for_each(compiled.source, '|', [&](std::string_view part) {
		pipe_parts.push_back(std::string{trim(part)});
		return true;
	});
	compiled.base = pipe_parts.empty() ? std::string{} : std::move(pipe_parts[0]);
	if (auto base = compile_base_expr(compiled.base); base) {
		compiled.compiled_base = std::make_shared<CompiledBaseExpr>(std::move(*base));
	}
	compiled.filters.reserve(pipe_parts.size() > 0 ? pipe_parts.size() - 1 : 0);
	for (std::size_t i = 1; i < pipe_parts.size(); ++i) {
		auto filter = trim(pipe_parts[i]);
		CompiledFilter compiled_filter;
		auto paren = filter.find('(');
		if (paren != std::string::npos) {
			compiled_filter.name = std::string{trim(filter.substr(0, paren))};
			auto close = filter.rfind(')');
			if (close != std::string::npos) {
				compiled_filter.args = split_args(filter.substr(paren + 1, close - paren - 1));
				compiled_filter.compiled_args = compile_expr_ptr_list(compiled_filter.args);
			}
		} else {
			compiled_filter.name = std::move(filter);
		}
		compiled.filters.push_back(std::move(compiled_filter));
	}
	if (compiled.filters.empty()) {
		compiled.macro_call = compile_macro_call(compiled.base);
	}
	return compiled;
}
TmplValue const *obj_find(
	TmplValue const &obj,
	std::string_view key) {
	for (auto const &kv: obj.as_object()) {
		if (kv.first == key) {
			return &kv.second;
		}
	}
	return nullptr;
}
std::vector<std::pair<std::string, std::optional<TmplValue>>> save_scope(
	TmplValue const &ctx,
	std::span<std::string const> names) {
	std::vector<std::pair<std::string, std::optional<TmplValue>>> saved;
	saved.reserve(names.size());
	for (auto const &n: names) {
		auto const *prev = obj_find(ctx, n);
		saved.emplace_back(n, (prev != nullptr) ? std::optional<TmplValue>{*prev} : std::nullopt);
	}
	return saved;
}
void restore_scope(
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

} // namespace conflux::templates
