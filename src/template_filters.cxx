module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

using conflux::utils::eprintln;

TmplValue apply_length_filter(
	TmplValue const &val) {
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
TmplValue apply_int_filter(
	TmplValue const &val) {
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
		} catch (std::exception const &e) {
			eprintln(std::format("template filter int: failed to parse '{}': {}", s, e.what()));
			return TmplValue{std::int64_t{0}};
		}
	}
	return TmplValue{std::int64_t{0}};
}
template<class ValueToString>
TmplValue apply_upper_filter(
	TmplValue const &val,
	ValueToString value_to_string_fn) {
	auto s = value_to_string_fn(val);
	for (auto &c: s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return TmplValue{std::move(s)};
}
template<class ValueToString>
TmplValue apply_lower_filter(
	TmplValue const &val,
	ValueToString value_to_string_fn) {
	auto s = value_to_string_fn(val);
	for (auto &c: s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return TmplValue{std::move(s)};
}
template<class ValueToString>
TmplValue apply_title_filter(
	TmplValue const &val,
	ValueToString value_to_string_fn) {
	auto s = value_to_string_fn(val);
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
	return TmplValue{std::move(s)};
}
template<class EvalArg>
TmplValue apply_replace_filter(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg,
	auto value_to_string_fn) {
	auto s = value_to_string_fn(val);
	if (arg_count >= 2) {
		auto old_s = value_to_string_fn(eval_arg(0));
		auto new_s = value_to_string_fn(eval_arg(1));
		return TmplValue{str_replace_all(s, old_s, new_s)};
	}
	return TmplValue{std::move(s)};
}
template<class EvalArg>
TmplValue apply_default_filter(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg,
	auto is_truthy_fn) {
	if (!is_truthy_fn(val) && arg_count != 0) {
		return eval_arg(0);
	}
	return val;
}
template<class EvalArg>
TmplValue apply_join_filter(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg,
	auto value_to_string_fn) {
	if (!val.is_array()) {
		return val;
	}
	auto sep = arg_count != 0 ? value_to_string_fn(eval_arg(0)) : "";
	auto const &arr = val.as_array();
	std::string result;
	result.reserve(arr.size() * (sep.size() + 16));
	bool first = true;
	for (auto const &item: arr) {
		if (!first) {
			result += sep;
		}
		first = false;
		result += value_to_string_fn(item);
	}
	return TmplValue{std::move(result)};
}
TmplValue apply_sort_filter(
	TmplValue const &val) {
	if (!val.is_array()) {
		return val;
	}
	TmplValue result{val.as_array()};
	std::sort(result.as_array().begin(), result.as_array().end(), [](TmplValue const &a, TmplValue const &b) {
		return a.dump() < b.dump();
	});
	return result;
}
TmplValue apply_reverse_filter(
	TmplValue const &val) {
	if (!val.is_array()) {
		return val;
	}
	TmplValue result{val.as_array()};
	std::reverse(result.as_array().begin(), result.as_array().end());
	return result;
}
TmplValue apply_last_filter(
	TmplValue const &val) {
	if (val.is_array() && !val.as_array().empty()) {
		return val.as_array().back();
	}
	return {};
}
TmplValue apply_min_filter(
	TmplValue const &val) {
	if (!val.is_array()) {
		return val;
	}
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
TmplValue apply_list_filter(
	TmplValue const &val) {
	if (val.is_array()) {
		return val;
	}
	return TmplValue{TmplValue::Array{}};
}
template<class EvalArg>
TmplValue apply_selectattr_filter(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg,
	auto value_to_string_fn) {
	if (!(val.is_array() && arg_count >= 3)) {
		return TmplValue{TmplValue::Array{}};
	}
	auto attr = value_to_string_fn(eval_arg(0));
	auto test = value_to_string_fn(eval_arg(1));
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
		} else if (test == "in" && test_val.is_array()) {
			auto const &values = test_val.as_array();
			if (std::ranges::find(values, *v) != values.end()) {
				result.push_back(item);
			}
		}
	}
	return result;
}
template<class EvalArg>
TmplValue apply_attr_filter(
	TmplValue const &val,
	std::size_t arg_count,
	EvalArg eval_arg,
	auto value_to_string_fn) {
	if (val.is_object() && arg_count != 0) {
		auto attr_name = value_to_string_fn(eval_arg(0));
		auto const *found = obj_find(val, attr_name);
		return (found != nullptr) ? *found : TmplValue{};
	}
	return {};
}
template<class ValueToString>
TmplValue apply_escape_filter(
	TmplValue const &val,
	ValueToString value_to_string_fn) {
	auto s = value_to_string_fn(val);
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
	return TmplValue{std::move(result)};
}

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
	auto value_to_string_fn = [](TmplValue const &v) { return value_to_string(v); };
	auto is_truthy_fn = [](TmplValue const &v) { return is_truthy(v); };
	if (name == "length" || name == "count") {
		return apply_length_filter(val);
	}
	if (name == "S") {
		return TmplValue{value_to_string(val)};
	}
	if (name == "int") {
		return apply_int_filter(val);
	}
	if (name == "upper") {
		return apply_upper_filter(val, value_to_string_fn);
	}
	if (name == "lower") {
		return apply_lower_filter(val, value_to_string_fn);
	}
	if (name == "capitalize") {
		return TmplValue{str_capitalize(value_to_string(val))};
	}
	if (name == "title") {
		return apply_title_filter(val, value_to_string_fn);
	}
	if (name == "replace") {
		return apply_replace_filter(val, args.size(), eval_arg, value_to_string_fn);
	}
	if (name == "default" || name == "d") {
		return apply_default_filter(val, args.size(), eval_arg, is_truthy_fn);
	}
	if (name == "join") {
		return apply_join_filter(val, args.size(), eval_arg, value_to_string_fn);
	}
	if (name == "sort") {
		return apply_sort_filter(val);
	}
	if (name == "reverse") {
		return apply_reverse_filter(val);
	}
	if (name == "last") {
		return apply_last_filter(val);
	}
	if (name == "std::min") {
		return apply_min_filter(val);
	}
	if (name == "list") {
		return apply_list_filter(val);
	}
	if (name == "selectattr") {
		return apply_selectattr_filter(val, args.size(), eval_arg, value_to_string_fn);
	}
	if (name == "attr") {
		return apply_attr_filter(val, args.size(), eval_arg, value_to_string_fn);
	}
	if (name == "e" || name == "escape") {
		return apply_escape_filter(val, value_to_string_fn);
	}
	return val;
}

} // namespace conflux::templates
