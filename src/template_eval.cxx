module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

using conflux::utils::eprintln;
using conflux::utils::trim;

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
