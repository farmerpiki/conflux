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
template<class Range, class Fn>
TmplValue tmpl_array_from(
	Range &&range,
	Fn fn) {
	TmplValue out{TmplValue::Array{}};
	auto &arr = out.as_array();
	if constexpr (std::ranges::sized_range<Range>) {
		arr.reserve(std::ranges::size(range));
	}
	std::ranges::transform(std::forward<Range>(range), std::back_inserter(arr), std::move(fn));
	return out;
}
TmplValue tmpl_object_keys(
	TmplValue::Object const &obj) {
	return tmpl_array_from(obj, [](auto const &kv) { return TmplValue{kv.first}; });
}
TmplValue tmpl_object_values(
	TmplValue::Object const &obj) {
	return tmpl_array_from(obj, [](auto const &kv) { return kv.second; });
}
TmplValue tmpl_object_items(
	TmplValue::Object const &obj) {
	return tmpl_array_from(obj, [](auto const &kv) {
		TmplValue pair{TmplValue::Array{}};
		pair.push_back(TmplValue{kv.first});
		pair.push_back(kv.second);
		return pair;
	});
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
static std::size_t find_top_level_token(
	std::string_view haystack,
	std::string_view needle) noexcept {
	return find_top_level_match(haystack, needle.size(), [&](std::size_t i) noexcept {
		return haystack.substr(i, needle.size()) == needle;
	});
}
static std::size_t find_top_level_char(
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
// ---------------------------------------------------------------------------
// Expression evaluator
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
TmplValue Environment::Impl::eval_expr(
	std::string const &expr,
	TmplValue const &context) const {
	return eval_expr(compile_expr(expr), context);
}

template<class EvalArg>
TmplValue Environment::Impl::apply_template_get_method(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg) const {
	if (!val.is_object() || arg_count == 0) {
		return {};
	}
	auto k = eval_arg(0);
	auto const *found = obj_find(val, value_to_string(k));
	if (found) {
		return *found;
	}
	return arg_count > 1 ? eval_arg(1) : TmplValue{};
}
template<class EvalArg>
TmplValue Environment::Impl::apply_template_replace_method(
	TmplValue const &val,
	EvalArg eval_arg) const {
	auto s = value_to_string(val);
	auto old_s = value_to_string(eval_arg(0));
	auto new_s = value_to_string(eval_arg(1));
	return TmplValue{str_replace_all(s, old_s, new_s)};
}
TmplValue Environment::Impl::apply_template_title_method(
	TmplValue const &val) const {
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
	return TmplValue{std::move(s)};
}
TmplValue Environment::Impl::apply_template_upper_method(
	TmplValue const &val) const {
	auto s = value_to_string(val);
	for (auto &c: s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return TmplValue{std::move(s)};
}
TmplValue Environment::Impl::apply_template_lower_method(
	TmplValue const &val) const {
	auto s = value_to_string(val);
	for (auto &c: s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return TmplValue{std::move(s)};
}
template<class EvalArg>
TmplValue Environment::Impl::apply_template_startswith_method(
	TmplValue const &val,
	EvalArg eval_arg) const {
	auto s = value_to_string(val);
	auto prefix = value_to_string(eval_arg(0));
	return TmplValue{s.compare(0, prefix.size(), prefix) == 0};
}
template<class EvalArg>
TmplValue Environment::Impl::apply_template_split_method(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg) const {
	auto s = value_to_string(val);
	auto sep = arg_count != 0 ? value_to_string(eval_arg(0)) : std::string{" "};
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

template<class EvalArg>
TmplValue Environment::Impl::apply_template_method(
	std::string const &name,
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg) const {
	if (name == "get" && val.is_object()) {
		return apply_template_get_method(val, arg_count, eval_arg);
	}
	if (name == "replace" && arg_count >= 2) {
		return apply_template_replace_method(val, eval_arg);
	}
	if (name == "title") {
		return apply_template_title_method(val);
	}
	if (name == "upper") {
		return apply_template_upper_method(val);
	}
	if (name == "lower") {
		return apply_template_lower_method(val);
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
	if (name == "startswith" && arg_count != 0) {
		return apply_template_startswith_method(val, eval_arg);
	}
	if (name == "split") {
		return apply_template_split_method(val, arg_count, eval_arg);
	}
	if (name == "keys" && val.is_object()) {
		return tmpl_object_keys(val.as_object());
	}
	if (name == "values" && val.is_object()) {
		return tmpl_object_values(val.as_object());
	}
	if (name == "items" && val.is_object()) {
		return tmpl_object_items(val.as_object());
	}
	return val;
}
TmplValue Environment::Impl::apply_method(
	std::string const &name,
	TmplValue const &val,
	std::vector<CompiledExprPtr> const &args,
	TmplValue const &context) const {
	auto eval_arg = [&](std::size_t idx) -> TmplValue {
		return idx < args.size() && args[idx] ? eval_expr(*args[idx], context) : TmplValue{};
	};
	return apply_template_method(name, val, args.size(), eval_arg);
}
TmplValue Environment::Impl::apply_fallback_path_method(
	std::string const &name,
	TmplValue const &val,
	std::vector<std::string> const &args,
	TmplValue const &context) const {
	auto eval_arg = [&](std::size_t idx) -> TmplValue {
		return idx < args.size() ? eval_expr(args[idx], context) : TmplValue{};
	};
	return apply_template_method(name, val, args.size(), eval_arg);
}
TmplValue slice_template_string(
	std::string_view value,
	std::optional<TmplValue> const &start_value,
	std::optional<TmplValue> const &end_value) {
	std::int64_t start = 0;
	std::int64_t end = static_cast<std::int64_t>(value.size());
	if (start_value && start_value->is_int()) {
		start = start_value->as<std::int64_t>();
		if (start < 0) {
			start = std::max<std::int64_t>(0, static_cast<std::int64_t>(value.size()) + start);
		}
	}
	if (end_value && end_value->is_int()) {
		end = end_value->as<std::int64_t>();
		if (end < 0) {
			end = std::max<std::int64_t>(0, static_cast<std::int64_t>(value.size()) + end);
		}
	}
	start = std::clamp<std::int64_t>(start, 0, static_cast<std::int64_t>(value.size()));
	end = std::clamp<std::int64_t>(end, 0, static_cast<std::int64_t>(value.size()));
	return TmplValue{std::string{value.substr(
		static_cast<std::size_t>(start),
		static_cast<std::size_t>(std::max<std::int64_t>(0, end - start)))}};
}
std::optional<TmplValue> lookup_template_index(
	TmplValue const &value,
	TmplValue const &index) {
	if (value.is_array() && index.is_int()) {
		auto idx = index.as<std::int64_t>();
		auto const &arr = value.as_array();
		if (idx < 0) {
			idx += static_cast<std::int64_t>(arr.size());
		}
		if (idx >= 0 && static_cast<std::size_t>(idx) < arr.size()) {
			return arr[static_cast<std::size_t>(idx)];
		}
		return std::nullopt;
	}
	if (value.is_object() && index.is_string()) {
		auto const *found = obj_find(value, index.as<std::string_view>());
		if (found) {
			return *found;
		}
	}
	return std::nullopt;
}
std::optional<TmplValue> lookup_template_field(
	TmplValue const &value,
	std::string_view name) {
	if (value.is_object()) {
		auto const *found = obj_find(value, name);
		if (found) {
			return *found;
		}
		return std::nullopt;
	}
	if (value.is_string() && name == "value") {
		return value;
	}
	return std::nullopt;
}
TmplValue Environment::Impl::eval_path(
	std::vector<CompiledPathSegment> const &path,
	TmplValue const &context) const {
	TmplValue owned;
	bool use_owned = false;
	TmplValue const *cur = &context;
	auto set_owned = [&](TmplValue v) {
		owned = std::move(v);
		cur = &owned;
		use_owned = true;
	};
	for (auto const &seg: path) {
		switch (seg.kind) {
		case CompiledPathSegmentKind::field:
			if (auto found = lookup_template_field(*cur, seg.name)) {
				set_owned(std::move(*found));
			} else {
				return {};
			}
			break;
		case CompiledPathSegmentKind::index:
			{
				auto idx_val = seg.expr ? eval_expr(*seg.expr, context) : TmplValue{};
				auto found = lookup_template_index(*cur, idx_val);
				if (!found) {
					return {};
				}
				set_owned(std::move(*found));
				break;
			}
		case CompiledPathSegmentKind::slice:
			if (cur->is_string()) {
				set_owned(slice_template_string(
					cur->as<std::string_view>(),
					seg.start ? std::optional{eval_expr(*seg.start, context)} : std::nullopt,
					seg.end ? std::optional{eval_expr(*seg.end, context)} : std::nullopt));
			} else {
				return {};
			}
			break;
		case CompiledPathSegmentKind::method: set_owned(apply_method(seg.name, *cur, seg.args, context)); break;
		}
	}
	return use_owned ? owned : *cur;
}
TmplValue Environment::Impl::eval_base_operand(
	CompiledBaseExpr const &base,
	std::size_t index,
	TmplValue const &context) const {
	return base.operands.size() > index && base.operands[index] ? eval_expr(*base.operands[index], context) :
																  TmplValue{};
}
TmplValue Environment::Impl::eval_base_object(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	TmplValue obj{TmplValue::Object{}};
	for (auto const &item: base.object_items) {
		obj.set(item.key, item.value ? eval_expr(*item.value, context) : TmplValue{});
	}
	return obj;
}
template<class EvalRight>
TmplValue Environment::Impl::eval_template_or(
	TmplValue left,
	EvalRight eval_right) const {
	if (is_truthy(left)) {
		return left;
	}
	return eval_right();
}
template<class EvalRight>
TmplValue Environment::Impl::eval_template_and(
	TmplValue left,
	EvalRight eval_right) const {
	if (!is_truthy(left)) {
		return left;
	}
	return eval_right();
}
TmplValue Environment::Impl::eval_template_not(
	TmplValue const &value) const {
	return TmplValue{!is_truthy(value)};
}
TmplValue Environment::Impl::eval_base_binary_or(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	auto left = eval_base_operand(base, 0, context);
	return eval_template_or(std::move(left), [&] { return eval_base_operand(base, 1, context); });
}
TmplValue Environment::Impl::eval_base_binary_and(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	auto left = eval_base_operand(base, 0, context);
	return eval_template_and(std::move(left), [&] { return eval_base_operand(base, 1, context); });
}

TmplValue compare_template_values(
	TmplValue const &left,
	TmplValue const &right,
	CompiledCompareOp op) {
	switch (op) {
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
			if (op == CompiledCompareOp::le) {
				return TmplValue{lv <= rv};
			}
			if (op == CompiledCompareOp::ge) {
				return TmplValue{lv >= rv};
			}
			if (op == CompiledCompareOp::lt) {
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

TmplValue Environment::Impl::eval_base_compare(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	auto left = eval_base_operand(base, 0, context);
	auto right = eval_base_operand(base, 1, context);
	return compare_template_values(left, right, base.compare_op);
}
TmplValue concat_template_values(
	TmplValue const &left,
	TmplValue const &right,
	auto value_to_string_fn) {
	return TmplValue{value_to_string_fn(left) + value_to_string_fn(right)};
}
TmplValue Environment::Impl::eval_base_concat(
	CompiledBaseExpr const &base,
	TmplValue const &context) const {
	auto left = eval_base_operand(base, 0, context);
	auto right = eval_base_operand(base, 1, context);
	return concat_template_values(left, right, [](TmplValue const &value) { return value_to_string(value); });
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
		return tmpl_array_from(base.operands, [&](auto const &item) {
			return item ? eval_expr(*item, context) : TmplValue{};
		});
	case CompiledBaseKind::object    : return eval_base_object(base, context);
	case CompiledBaseKind::group     : return eval_base_operand(base, 0, context);
	case CompiledBaseKind::path      : return eval_path(base.path, context);
	case CompiledBaseKind::unary_not : return eval_template_not(eval_base_operand(base, 0, context));
	case CompiledBaseKind::binary_or : return eval_base_binary_or(base, context);
	case CompiledBaseKind::binary_and: return eval_base_binary_and(base, context);
	case CompiledBaseKind::compare   : return eval_base_compare(base, context);
	case CompiledBaseKind::concat    : return eval_base_concat(base, context);
	}
	return {};
}

std::optional<TmplValue> Environment::Impl::eval_fallback_literal(
	std::string_view base) const {
	auto b = trim(base);
	if (b.empty()) {
		return TmplValue{};
	}

	if ((b.front() == '"' && b.back() == '"') || (b.front() == '\'' && b.back() == '\'')) {
		return TmplValue{b.substr(1, b.size() - 2)};
	}

	if (std::isdigit(static_cast<unsigned char>(b[0])) || (b[0] == '-' && b.size() > 1)) {
		try {
			if (b.find('.') != std::string::npos) {
				return TmplValue{std::stod(std::string{b})};
			}
			return TmplValue{static_cast<std::int64_t>(std::stoll(std::string{b}))};
		} catch (std::exception const &ex) {
			eprintln(std::format("template eval_literal: failed to parse number '{}': {}", b, ex.what()));
		}
	}

	if (b == "true" || b == "True") {
		return TmplValue{true};
	}
	if (b == "false" || b == "False") {
		return TmplValue{false};
	}
	if (b == "none" || b == "None") {
		return TmplValue{};
	}

	return std::nullopt;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::optional<TmplValue> Environment::Impl::eval_fallback_collection(
	std::string_view base,
	TmplValue const &context) const {
	auto b = trim(base);
	if (b.empty()) {
		return TmplValue{};
	}

	if (b.front() == '[' && b.back() == ']') {
		auto inner = trim(b.substr(1, b.size() - 2));
		if (inner.empty()) {
			return TmplValue{TmplValue::Array{}};
		}
		auto items = split_args(inner);
		return tmpl_array_from(items, [&](auto const &item) { return eval_expr(item, context); });
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
		return tmpl_array_from(items, [&](auto const &item) { return eval_expr(item, context); });
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
				auto key = trim(std::string_view{p}.substr(0, colon));
				auto val = trim(std::string_view{p}.substr(colon + 1));
				if ((key.front() == '"' && key.back() == '"') || (key.front() == '\'' && key.back() == '\'')) {
					key = key.substr(1, key.size() - 2);
				}
				obj.set(std::string{key}, eval_expr(std::string{val}, context));
			}
		}
		return obj;
	}

	return std::nullopt;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::optional<TmplValue> Environment::Impl::eval_fallback_operator(
	std::string_view base,
	TmplValue const &context) const {
	auto b = trim(base);
	if (b.empty()) {
		return TmplValue{};
	}

	{
		auto const i = find_top_level_token(b, " or ");
		if (i != std::string_view::npos) {
			auto left = eval_expr(std::string{b.substr(0, i)}, context);
			return eval_template_or(std::move(left), [&] { return eval_expr(std::string{b.substr(i + 4)}, context); });
		}
	}

	{
		auto const i = find_top_level_token(b, " and ");
		if (i != std::string_view::npos) {
			auto left = eval_expr(std::string{b.substr(0, i)}, context);
			return eval_template_and(std::move(left), [&] { return eval_expr(std::string{b.substr(i + 5)}, context); });
		}
	}

	if (b.size() > 4 && b.substr(0, 4) == "not ") {
		auto inner_val = eval_expr(std::string{b.substr(4)}, context);
		return eval_template_not(inner_val);
	}

	{
		static constexpr auto ops = std::to_array<std::pair<std::string_view, CompiledCompareOp>>({
			{" == ", CompiledCompareOp::eq},
			{" != ", CompiledCompareOp::ne},
			{" <= ", CompiledCompareOp::le},
			{" >= ", CompiledCompareOp::ge},
			{ " < ", CompiledCompareOp::lt},
			{ " > ", CompiledCompareOp::gt},
			{" in ", CompiledCompareOp::in},
		});
		for (auto &[op, code]: ops) {
			auto p = find_top_level_token(b, op);
			if (p != std::string_view::npos) {
				auto left = eval_expr(std::string{b.substr(0, p)}, context);
				auto right = eval_expr(std::string{b.substr(p + op.size())}, context);
				return compare_template_values(left, right, code);
			}
		}
	}

	{
		auto const i = find_top_level_char(b, '~');
		if (i != std::string_view::npos) {
			auto left = eval_expr(std::string{b.substr(0, i)}, context);
			auto right = eval_expr(std::string{b.substr(i + 1)}, context);
			return concat_template_values(left, right, [](TmplValue const &value) { return value_to_string(value); });
		}
	}

	return std::nullopt;
}

TmplValue Environment::Impl::eval_fallback_base(
	std::string const &base,
	TmplValue const &context) const {
	auto b = trim(base);
	if (b.empty()) {
		return {};
	}
	if (auto literal = eval_fallback_literal(b); literal) {
		return std::move(*literal);
	}
	if (auto collection = eval_fallback_collection(b, context); collection) {
		return std::move(*collection);
	}
	if (auto operation = eval_fallback_operator(b, context); operation) {
		return std::move(*operation);
	}
	return eval_fallback_path(b, context);
}

TmplValue Environment::Impl::eval_fallback_path(
	std::string_view base,
	TmplValue const &context) const {
	TmplValue owned;
	bool use_owned = false;
	TmplValue const *cur = &context;
	auto set_owned = [&](TmplValue v) {
		owned = std::move(v);
		cur = &owned;
		use_owned = true;
	};
	std::string remaining{base};

	while (!remaining.empty()) {
		auto bracket = remaining.find('[');
		auto dot = remaining.find('.');
		auto paren = remaining.find('(');

		auto next_sep = std::min({bracket, dot, paren, remaining.size()});

		if (next_sep == 0 && bracket == 0) {
			auto close = remaining.find(']', 1);
			if (close == std::string::npos) {
				return {};
			}
			auto idx_str = trim(std::string_view{remaining}.substr(1, close - 1));
			if (auto colon = idx_str.find(':'); colon != std::string_view::npos) {
				if (cur->is_string()) {
					auto start_s = trim(idx_str.substr(0, colon));
					auto end_s = trim(idx_str.substr(colon + 1));
					set_owned(slice_template_string(
						cur->as<std::string_view>(),
						start_s.empty() ? std::nullopt : std::optional{eval_expr(std::string{start_s}, context)},
						end_s.empty() ? std::nullopt : std::optional{eval_expr(std::string{end_s}, context)}));
				} else {
					return {};
				}
				remaining = remaining.substr(close + 1);
				if (!remaining.empty() && remaining[0] == '.') {
					remaining = remaining.substr(1);
				}
				continue;
			}
			auto idx_val = eval_expr(std::string{idx_str}, context);
			auto found = lookup_template_index(*cur, idx_val);
			if (!found) {
				return {};
			}
			set_owned(std::move(*found));
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
			auto found = lookup_template_field(*cur, key);
			if (!found) {
				return {};
			}
			set_owned(std::move(*found));
		}

		if (is_method_call) {
			auto const close = find_matching_pair(remaining, 0, '(', ')');
			if (close == std::string::npos) {
				return {};
			}
			auto args_str = remaining.substr(1, close - 1);
			auto method_args = split_args(args_str);
			remaining = remaining.substr(close + 1);
			if (!remaining.empty() && remaining[0] == '.') {
				remaining = remaining.substr(1);
			}

			set_owned(apply_fallback_path_method(key, *cur, method_args, context));
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

TmplValue Environment::Impl::eval_expr(
	CompiledExpr const &expr,
	TmplValue const &context) const {
	if (expr.base.empty()) {
		return {};
	}

	TmplValue result =
		expr.compiled_base ? this->eval_base(*expr.compiled_base, context) : eval_fallback_base(expr.base, context);

	for (auto const &filter: expr.filters) {
		result = apply_filter(filter, result, context);
	}

	return result;
}

} // namespace conflux::templates
