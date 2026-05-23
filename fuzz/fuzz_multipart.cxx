// libFuzzer driver for parse_multipart.
// Invariants:
//   - never crash on arbitrary boundary/body bytes
//   - parsed borrowed names, filenames, content types, data, and form fields are stable views
//   - parser-enforced part cap is respected

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http_server_helpers;

using namespace std;

namespace {

[[nodiscard]] bool points_into(
	std::string_view haystack,
	std::string_view needle) noexcept {
	if (needle.empty()) {
		return true;
	}
	auto const *base = haystack.data();
	auto const *ptr = needle.data();
	return ptr >= base && ptr + needle.size() <= base + haystack.size();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size < 2 || size > 256U * 1024U) {
		return 0;
	}
	std::string_view input{reinterpret_cast<char const *>(data), size};
	std::string_view boundary;
	std::string_view body;
	if (auto const split = input.find('\n'); split != std::string_view::npos && split > 0 && split <= 70U) {
		boundary = input.substr(0, split);
		body = input.substr(split + 1);
	} else {
		auto const cap = std::min<std::size_t>(70U, size - 1U);
		auto const boundary_size = 1U + (data[0] % cap);
		if (size <= 1U + boundary_size) {
			return 0;
		}
		boundary = std::string_view{reinterpret_cast<char const *>(data + 1), boundary_size};
		body = std::string_view{reinterpret_cast<char const *>(data + 1 + boundary_size), size - 1U - boundary_size};
	}

	HttpFieldsView form;
	std::vector<UploadedFile> files;
	parse_multipart(body, boundary, form, files);

	if (form.size() + files.size() > 1000U) {
		__builtin_trap();
	}
	for (auto const &[name, value]: form) {
		if (!points_into(body, name) || !points_into(body, value)) {
			__builtin_trap();
		}
	}
	for (auto const &file: files) {
		if (file.owns_metadata || file.owns_data) {
			__builtin_trap();
		}
		if (!points_into(body, file.name)
			|| !points_into(body, file.filename)
			|| !(file.content_type == "text/plain" || points_into(body, file.content_type))
			|| !points_into(body, file.data)) {
			__builtin_trap();
		}
	}
	return 0;
}
