#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

namespace json = conflux::json;

namespace {

struct Case {
	std::string_view name;
	std::string_view input;
	bool valid;
};

[[nodiscard]] bool command_ok(
	std::string const &command) {
	auto const status = std::system(command.c_str());
	return status == 0;
}

[[nodiscard]] bool tool_available(
	std::string_view name) {
	return command_ok(std::format("command -v {} >/dev/null 2>&1", name));
}

[[nodiscard]] std::string shell_quote(
	std::string_view value) {
	std::string out{"'"};
	for (auto const ch: value) {
		if (ch == '\'') {
			out += "'\\''";
		} else {
			out += ch;
		}
	}
	out += "'";
	return out;
}

[[nodiscard]] std::filesystem::path write_case_file(
	std::string_view input,
	std::string_view name) {
	auto clean_name = std::string{name};
	std::ranges::replace_if(
		clean_name,
		[](char const ch) { return !std::isalnum(static_cast<unsigned char>(ch)); },
		'_');
	auto path = std::filesystem::temp_directory_path()
			  / std::format(
					"conflux-json-differential-{}-{}.json",
					clean_name,
					std::chrono::steady_clock::now().time_since_epoch().count());
	std::ofstream file{path, std::ios::binary};
	REQUIRE(file.good());
	file << input;
	REQUIRE(file.good());
	return path;
}

[[nodiscard]] bool node_accepts(
	std::filesystem::path const &path) {
	auto const script = "const fs=require('fs');JSON.parse(fs.readFileSync(process.argv[1],'utf8'))";
	return command_ok(std::format("node -e {} {} >/dev/null 2>&1", shell_quote(script), shell_quote(path.string())));
}

[[nodiscard]] bool python_accepts(
	std::filesystem::path const &path) {
	auto const script =
		"import json,sys;"
		"json.loads(open(sys.argv[1],encoding='utf-8').read(),"
		"parse_constant=lambda c: (_ for _ in ()).throw(ValueError(c)))";
	return command_ok(std::format("python3 -c {} {} >/dev/null 2>&1", shell_quote(script), shell_quote(path.string())));
}

constexpr auto cases = std::array{
	Case{			 "empty object",										 "{}",  true},
	Case{			  "empty array",										 "[]",  true},
	Case{			"nested object",     R"({"a":[1,true,false,null,{"b":"c"}]})",  true},
	Case{		   "escaped string",                    R"(["\"\\\/\b\f\n\r\t"])",  true},
	Case{		  "unicode escapes",               R"(["\u0041","\uD83D\uDE00"])",  true},
	Case{			 "number forms", R"([0,-0,1,-1,1.5,-1.5,1e10,1E-10,1.5e+10])",  true},
	Case{        "top scalar string",								 R"("value")",  true},
	Case{		  "top scalar null",									   "null",  true},
	Case{			   "whitespace",                   " \r\n\t [ 1 , 2 , 3 ] \n",  true},
	Case{			 "leading zero",									   "[01]", false},
	Case{  "missing fraction digits",									   "[1.]", false},
	Case{  "missing exponent digits",									  "[1e+]", false},
	Case{				"plus sign",									   "[+1]", false},
	Case{     "trailing comma array",									   "[1,]", false},
	Case{    "trailing comma object",                                R"({"a":1,})", false},
	Case{     "single quoted string",									  "['x']", false},
	Case{			 "unquoted key",									  "{a:1}", false},
	Case{				  "comment",							  "// comment\n1", false},
	Case{		 "trailing garbage",										"1 2", false},
	Case{		   "invalid escape",								R"(["\x00"])", false},
	Case{"incomplete unicode escape",                               R"(["\u123"])", false},
	Case{		  "literal newline",                          "[\"line\nbreak\"]", false},
	Case{				 "bare nan",									  "[NaN]", false},
	Case{			"bare infinity",								 "[Infinity]", false},
};

}

TEST_CASE(
	"json differential smoke: strict accept/reject agrees with node and python",
	"[json][differential][external]") {
	auto const has_node = tool_available("node");
	auto const has_python = tool_available("python3");
	if (!has_node && !has_python) {
		SKIP("neither node nor python3 is available for JSON differential smoke");
	}

	for (auto const &item: cases) {
		INFO(item.name);
		auto doc = json::parse(item.input);
		CHECK(doc.has_value() == item.valid);

		auto const path = write_case_file(item.input, item.name);

		if (has_node) {
			INFO("node");
			CHECK(node_accepts(path) == item.valid);
		}
		if (has_python) {
			INFO("python3");
			CHECK(python_accepts(path) == item.valid);
		}
		auto const removed = std::filesystem::remove(path);
		CHECK(removed);
	}
}
