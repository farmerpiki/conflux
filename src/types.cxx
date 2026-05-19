module;
#include <memory>

export module conflux.types;

import std;

export struct IoError final : std::system_error {
	IoError(
		int err,
		std::string const &what)
		: std::system_error{err, std::generic_category(), what} {}
};
