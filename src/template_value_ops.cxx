module;

module conflux.templates;
import std;
import conflux.types;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;

namespace conflux::templates {

std::string Environment::Impl::value_to_string(
	TmplValue const &v) {
	if (v.is_null()) {
		return "";
	}
	if (v.is_string()) {
		return std::string(v.as<std::string_view>());
	}
	if (v.is_int()) {
		return std::to_string(v.as<std::int64_t>());
	}
	if (v.is_uint()) {
		return std::to_string(v.as<std::uint64_t>());
	}
	if (v.is_float()) {
		auto s = std::to_string(v.as<double>());
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

} // namespace conflux::templates
