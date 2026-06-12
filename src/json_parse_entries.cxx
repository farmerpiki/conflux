module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

[[nodiscard]] std::expected<void, JsonError> parse_inplace(DocumentStorage &store, JsonParseOptions const &opts);

[[nodiscard]] std::expected<Document, JsonError> parse_with_storage(
	DocumentStorage &storage_ref,
	std::unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts);

[[nodiscard]] std::expected<void, JsonError> check_input_limits(
	std::size_t input_size,
	JsonParseOptions const &opts) noexcept {
	constexpr std::size_t kU32Ceiling = (std::size_t{1} << 32) - 1;
	if (input_size >= kU32Ceiling) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds 4 GiB hard ceiling"});
	}
	if (opts.max_input_size.exceeds(input_size, kDefaultMaxInput)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds max_input_size"});
	}
	return {};
}

void set_storage_input_view(
	DocumentStorage &storage,
	std::string_view src) noexcept {
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage.bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage.input_view = src;
}

void prepare_copied_input(
	DocumentStorage &storage,
	std::string_view input) {
	storage.owned_input.assign(input);
	set_storage_input_view(storage, storage.owned_input);
}

void prepare_moved_input(
	DocumentStorage &storage,
	std::string &&input) {
	storage.owned_input = std::move(input);
	set_storage_input_view(storage, storage.owned_input);
}

void prepare_borrowed_input(
	DocumentStorage &storage,
	std::string_view input) noexcept {
	set_storage_input_view(storage, input);
}

template<typename PrepareInput>
[[nodiscard]] std::expected<Document, JsonError> parse_document_storage(
	std::size_t input_size,
	std::unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts,
	PrepareInput &&prepare_input) {
	if (auto ok = check_input_limits(input_size, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	prepare_input(*storage);
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_copied_input(*storage_, input);

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}

void JsonArena::reset_storage_for_reuse() noexcept {
	++generation_;
	storage_->reset();
	if (uses_internal_hash_index_pool()) {
		hash_index_pool_.release();
	}
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_borrowed_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_borrowed_input(*storage_, input);

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_moved_into(
	std::string input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_moved_input(*storage_, std::move(input));

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}

void JsonArena::reset() noexcept {
	++generation_;
	storage_ = nullptr;
	if (uses_internal_hash_index_pool()) {
		hash_index_pool_.release();
	}
	mbr_.release();
	storage_ = std::make_unique<DocumentStorage>(&mbr_, hash_index_resource_);
}

std::expected<Document, JsonError> parse_copy(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(),
		opts,
		[input](DocumentStorage &storage) { prepare_copied_input(storage, input); });
}

std::expected<Document, JsonError> parse_copy(
	std::string &&input,
	JsonParseOptions const &opts) {
	auto const input_size = input.size();
	return parse_document_storage(
		input_size,
		std::make_unique<DocumentStorage>(),
		opts,
		[input = std::move(input)](DocumentStorage &storage) mutable {
			prepare_moved_input(storage, std::move(input));
		});
}

std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(),
		opts,
		[input](DocumentStorage &storage) { prepare_borrowed_input(storage, input); });
}

std::expected<Document, JsonError> parse_borrowed_unsafe(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}

std::expected<Document, JsonError> parse_view(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}

std::expected<Document, JsonError> parse(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}

std::expected<Document, JsonError> parse_copy(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(resource),
		opts,
		[input](DocumentStorage &storage) { prepare_copied_input(storage, input); });
}

std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(resource),
		opts,
		[input](DocumentStorage &storage) { prepare_borrowed_input(storage, input); });
}

std::expected<Document, JsonError> parse_borrowed_unsafe(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}

std::expected<Document, JsonError> parse_view(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}

std::expected<Document, JsonError> parse(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}

namespace detail {

[[nodiscard]] JsonError dom_policy_error(
	std::string_view message) {
	return JsonError{
		.stage = JsonStage::parse,
		.code = JsonIssueCode::constraint_violation,
		.message = std::string{message}};
}

[[nodiscard]] std::expected<void, JsonError> require_dom_storage(
	JsonDomPolicy const &policy,
	JsonDomStorageModel expected_storage,
	std::string_view api_name) {
	if (policy.storage == expected_storage) {
		return {};
	}
	return std::unexpected(
		dom_policy_error(std::format("{} called with incompatible JsonDomPolicy storage model", api_name)));
}

} // namespace detail

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string_view input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return std::unexpected(
			detail::dom_policy_error(
				"parse_dom(string_view) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(string_view)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(detail::dom_policy_error("owned_move requires parse_dom(std::string&&)"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string &&input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return std::unexpected(
			detail::dom_policy_error(
				"parse_dom(std::string&&) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(std::string&&)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return std::unexpected(detail::dom_policy_error("borrowed_view is unsafe for parse_dom(std::string&&)"));
	}
	return parse_copy(std::move(input), policy.parse);
}

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string_view input,
	std::pmr::memory_resource *resource,
	JsonDomPolicy const &policy) {
	if (resource == nullptr) {
		return std::unexpected(detail::dom_policy_error("parse_dom(memory_resource*) requires a non-null resource"));
	}
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::caller_pmr_document,
			"parse_dom(memory_resource*)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(
			detail::dom_policy_error(
				"owned_move requires a std::string&& overload; caller_pmr cannot move-own input today"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	std::string_view input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, string_view)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return arena.parse_borrowed_into(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return arena.parse_into(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(detail::dom_policy_error("owned_move requires parse_dom(JsonArena&, std::string&&)"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	std::string &&input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, std::string&&)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return std::unexpected(
			detail::dom_policy_error("borrowed_view is unsafe for parse_dom(JsonArena&, std::string&&)"));
	}
	return arena.parse_moved_into(std::move(input), policy.parse);
}

} // namespace conflux::json
