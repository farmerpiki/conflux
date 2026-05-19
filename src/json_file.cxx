module;
#include <fcntl.h>

export module conflux.json.file;

import std;
import conflux.types;
import conflux.file_io_sync;
import conflux.json;

export namespace conflux::json {

enum class JsonFileStage : std::uint8_t {
	read,
	parse,
};

struct JsonFileError {
	JsonFileStage stage{JsonFileStage::read};
	int file_errno{};
	std::optional<JsonError> json{};
	std::string message{};

	[[nodiscard]] bool is_file_error() const noexcept { return stage == JsonFileStage::read; }
	[[nodiscard]] bool is_parse_error() const noexcept { return stage == JsonFileStage::parse; }
};

inline constexpr std::size_t kDefaultJsonFileReadLimit = 128ULL * 1024ULL * 1024ULL;

} // namespace conflux::json

namespace {

[[nodiscard]] std::size_t json_file_read_limit(
	conflux::json::JsonParseOptions const &opts) noexcept {
	if (opts.max_input_size.is_unlimited()) {
		return std::numeric_limits<std::size_t>::max();
	}
	if (auto explicit_limit = opts.max_input_size.explicit_value()) {
		return *explicit_limit;
	}
	return conflux::json::kDefaultJsonFileReadLimit;
}

[[nodiscard]] conflux::json::JsonFileError make_file_error(
	FileIoSyncError const &err) {
	return conflux::json::JsonFileError{
		.stage = conflux::json::JsonFileStage::read,
		.file_errno = err.code().value(),
		.message = std::string{err.what()}};
}

[[nodiscard]] conflux::json::JsonFileError make_parse_error(
	conflux::json::JsonError err) {
	std::string message = err.message;
	return conflux::json::JsonFileError{
		.stage = conflux::json::JsonFileStage::parse,
		.json = std::move(err),
		.message = std::move(message)};
}

} // namespace

export namespace conflux::json {

[[nodiscard]] std::expected<Document, JsonFileError> blocking_parse_file_at(
	int root_fd,
	std::string_view contained_relative_path,
	JsonParseOptions const &opts = {}) {
	auto bytes = blocking_read_file_at(root_fd, contained_relative_path, json_file_read_limit(opts));
	if (!bytes) {
		return std::unexpected{make_file_error(bytes.error())};
	}

	auto doc = parse_copy(std::move(*bytes), opts);
	if (!doc) {
		return std::unexpected{make_parse_error(std::move(doc.error()))};
	}
	return std::move(*doc);
}

[[nodiscard]] std::expected<Document, JsonFileError> blocking_parse_file(
	std::string_view contained_relative_path,
	JsonParseOptions const &opts = {}) {
	return blocking_parse_file_at(AT_FDCWD, contained_relative_path, opts);
}

} // namespace conflux::json
