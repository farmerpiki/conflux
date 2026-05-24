// libFuzzer driver for parse_multipart.
// Invariants:
//   - never crash on arbitrary or synthesized multipart boundary/body bytes
//   - borrowed names, filenames, content types, file data, and form fields stay valid
//   - parser-enforced part cap is respected
// Coverage focus:
//   - exact boundary parsing, malformed boundaries, filename/content-type params,
//     many tiny parts, and huge part headers

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

void check_parse(
	std::string_view body,
	std::string_view boundary) {
	if (boundary.empty()) {
		return;
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
}

[[nodiscard]] char token_char(
	std::uint8_t b) noexcept {
	static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
	return alphabet[b % alphabet.size()];
}

[[nodiscard]] std::string make_boundary(
	std::uint8_t const *data,
	std::size_t size) {
	std::size_t const n = 1U + (size == 0 ? 0U : data[0] % 32U);
	std::string out;
	out.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		out.push_back(token_char(i < size ? data[i] : static_cast<std::uint8_t>(i)));
	}
	return out;
}

void append_payload(
	std::string &body,
	std::uint8_t const *data,
	std::size_t size,
	std::size_t &cursor,
	std::size_t max_len) {
	std::size_t const n = size == 0 ? 0U : data[cursor++ % size] % (max_len + 1U);
	for (std::size_t i = 0; i < n; ++i) {
		char c = static_cast<char>(data[cursor++ % size]);
		if (c == '\r' || c == '\n') {
			c = static_cast<char>('a' + (static_cast<unsigned char>(c) % 26U));
		}
		body.push_back(c);
	}
}

void synthesize_multipart(
	std::uint8_t const *data,
	std::size_t size) {
	if (size == 0) {
		return;
	}

	auto const boundary = make_boundary(data, size);
	std::size_t cursor = 1;
	std::size_t const parts = 1U + (data[cursor++ % size] % 64U);
	std::string body;
	body.reserve(std::min<std::size_t>(size * 8U + 4096U, 256U * 1024U));

	for (std::size_t part = 0; part < parts && body.size() < 240U * 1024U; ++part) {
		bool const prefix_crlf = (data[cursor++ % size] & 1U) != 0;
		if (prefix_crlf && !body.empty()) {
			body += "\r\n";
		}
		body += "--";
		body += boundary;
		body += (data[cursor++ % size] & 1U) ? "\n" : "\r\n";

		switch (data[cursor++ % size] % 7U) {
		case 0: body += "Content-Disposition: form-data; name=field"; break;
		case 1: body += "Content-Disposition: form-data; name=upload; filename=file.txt"; break;
		case 2: body += "Content-Disposition: form-data; filename=only-file.txt"; break;
		case 3:
			body += "Content-Disposition: form-data; x-name=wrong; name=right; filename=quoted-";
			append_payload(body, data, size, cursor, 12);
			body += ".bin";
			break;
		case 4 : body += "Content-Disposition: form-data; NAME=upper; FILENAME=case.txt"; break;
		case 5 : body += "Content-Disposition: form-data; name=\"escaped\\\"name\"; filename=\"f\\\"n.txt\""; break;
		default: body += "Content-Disposition: form-data; name"; break;
		}
		body += "\r\n";

		if ((data[cursor++ % size] % 3U) == 0U) {
			body += "Content-Type: text/plain; charset=utf-8\r\n";
		}
		if ((data[cursor++ % size] % 17U) == 0U) {
			body += "X-Fill: ";
			std::size_t const fill = 1024U + (data[cursor++ % size] * 32U);
			body.append(fill, 'x');
			body += "\r\n";
		}

		body += "\r\n";
		append_payload(body, data, size, cursor, 96);
	}

	body += "\r\n--";
	body += boundary;
	if ((data[cursor % size] % 5U) == 0U) {
		body += "\r\n"; // malformed final delimiter: should be ignored or stop cleanly
	} else {
		body += "--\r\n";
	}

	check_parse(body, boundary);
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

	check_parse(body, boundary);
	synthesize_multipart(data, size);
	return 0;
}
