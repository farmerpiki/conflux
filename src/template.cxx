module;

export module conflux.templates;
import std;
import conflux.types;
import std.compat;
import conflux.json;
import conflux.utils;
import conflux.file_io_sync;
export namespace conflux::templates {

struct CompiledExpr;
using CompiledExprPtr = std::shared_ptr<CompiledExpr>;

enum class CompiledLiteralKind : std::uint8_t {
	none,
	boolean,
	integer,
	floating,
	string,
};
struct CompiledLiteral {
	CompiledLiteralKind kind = CompiledLiteralKind::none;
	bool boolean = false;
	std::int64_t integer = 0;
	double floating = 0.0;
	std::string string;
};
enum class CompiledCompareOp : std::uint8_t {
	eq,
	ne,
	le,
	ge,
	lt,
	gt,
	in,
};
enum class CompiledPathSegmentKind : std::uint8_t {
	field,
	index,
	slice,
	method,
};
struct CompiledPathSegment {
	CompiledPathSegmentKind kind = CompiledPathSegmentKind::field;
	std::string name;
	CompiledExprPtr expr;
	CompiledExprPtr start;
	CompiledExprPtr end;
	std::vector<CompiledExprPtr> args;
};
struct CompiledObjectItem {
	std::string key;
	CompiledExprPtr value;
};
enum class CompiledBaseKind : std::uint8_t {
	literal,
	array,
	tuple,
	object,
	group,
	path,
	unary_not,
	binary_or,
	binary_and,
	compare,
	concat,
};
struct CompiledBaseExpr {
	CompiledBaseKind kind = CompiledBaseKind::path;
	std::string source;
	CompiledLiteral literal;
	CompiledCompareOp compare_op = CompiledCompareOp::eq;
	std::vector<CompiledExprPtr> operands;
	std::vector<CompiledObjectItem> object_items;
	std::vector<CompiledPathSegment> path;
};
struct CompiledFilter {
	std::string name;
	std::vector<std::string> args;
	std::vector<CompiledExprPtr> compiled_args;
};
struct CompiledMacroArg {
	std::string name;
	std::string expr;
	CompiledExprPtr compiled;
	bool keyword = false;
};
struct CompiledMacroCall {
	std::string name;
	std::vector<CompiledMacroArg> args;
};
struct CompiledExpr {
	std::string source;
	std::string base;
	std::shared_ptr<CompiledBaseExpr> compiled_base;
	std::vector<CompiledFilter> filters;
	std::optional<CompiledMacroCall> macro_call;
};
struct Node;
using NodePtr = std::shared_ptr<Node>;
using NodeList = std::vector<NodePtr>;
struct TextNode {
	std::string text;
};
struct ExprNode {
	std::string expr;
	CompiledExpr compiled;
};
struct BlockNode {
	std::string name;
	NodeList body;
};
struct ExtendsNode {
	std::string parent;
};
struct IncludeNode {
	std::string name;
};
struct SetNode {
	std::string var;
	std::string expr;
	CompiledExpr compiled;
};
struct ForNode {
	std::vector<std::string> vars;
	std::string iter_expr;
	CompiledExpr compiled_iter;
	NodeList body;
};
struct IfNode {
	struct Branch {
		std::string condition;
		CompiledExpr compiled_condition;
		NodeList body;
	};
	std::vector<Branch> branches;
};
struct MacroNode {
	std::string name;
	std::vector<std::string> params;
	std::vector<std::string> defaults;
	std::vector<CompiledExpr> compiled_defaults;
	NodeList body;
};
struct FromImportNode {
	std::string file;
	std::string name;
	std::string alias;
};
struct Node {
	std::variant<
		TextNode,
		ExprNode,
		BlockNode,
		ExtendsNode,
		IncludeNode,
		SetNode,
		ForNode,
		IfNode,
		MacroNode,
		FromImportNode>
		data;
};
struct Template {
	std::string name;
	NodeList nodes;
	std::string extends_name;
	std::unordered_map<std::string, NodeList> blocks;
};
struct EnvironmentOptions {
	std::vector<std::string> extensions{".html", ".htm", ".txt"};
};
struct TmplValue;
enum class TemplateDiagnosticSeverity : std::uint8_t {
	warning,
	error,
};
enum class TemplateDiagnosticPhase : std::uint8_t {
	io,
	parse,
	compile,
	link,
	render_check,
};
struct TemplateSourceLocation {
	std::string template_name;
	std::string path;
	std::uint32_t line = 0;
	std::uint32_t column = 0;
	std::uint32_t byte_offset = 0;
};
struct TemplateDiagnostic {
	TemplateDiagnosticSeverity severity = TemplateDiagnosticSeverity::error;
	TemplateDiagnosticPhase phase = TemplateDiagnosticPhase::compile;
	TemplateSourceLocation location;
	std::vector<TemplateSourceLocation> stack;
	std::string check_label;
	std::string code;
	std::string message;
};
struct TemplateBuildReport {
	std::vector<TemplateDiagnostic> diagnostics;
	std::size_t templates_seen = 0;
	std::size_t templates_compiled = 0;

	[[nodiscard]] bool ok() const noexcept;
	[[nodiscard]] std::string format_text() const;
};
struct TemplateRenderCheckCase;
struct TemplateRenderCheckOptions {
	bool require_all_templates_covered = false;
	std::size_t max_output_bytes = 0;
};
using TemplateRenderCheckReport = TemplateBuildReport;
struct TemplateBuildError final : std::runtime_error {
	TemplateBuildReport report;
	explicit TemplateBuildError(TemplateBuildReport report);
};
class Environment {
public:
	explicit Environment(std::string const &template_dir);
	Environment(std::string const &template_dir, EnvironmentOptions options);
	~Environment();
	Environment(Environment &&) noexcept;
	Environment &operator =(Environment &&) noexcept;
	Environment(Environment const &) = delete;
	Environment &operator =(Environment const &) = delete;

	void load_all();
	void blocking_load_all();
	void blocking_reload_all();
	[[nodiscard]] std::expected<void, TemplateBuildReport> blocking_load_all_checked();
	[[nodiscard]] std::expected<void, TemplateBuildReport> blocking_reload_all_checked();
	[[nodiscard]] std::expected<void, TemplateRenderCheckReport>
	blocking_reload_all_checked(std::span<TemplateRenderCheckCase const> cases, TemplateRenderCheckOptions opts = {});
	[[nodiscard]] std::string render(std::string const &name, std::string const &json_ctx) const;
	[[nodiscard]] std::string render(std::string const &name, TmplValue const &ctx) const;
	[[nodiscard]] std::string render(std::string const &name, json::NodeRef ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, std::string const &json_ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, TmplValue const &ctx) const;
	[[nodiscard]] std::string render_string(std::string const &source, json::NodeRef ctx) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct TmplValue {
	using Array = std::vector<TmplValue>;
	using Object = std::vector<std::pair<std::string, TmplValue>>;

	std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> data;

	TmplValue() = default;
	explicit TmplValue(
		bool b)
		: data(b) {}
	explicit TmplValue(
		std::int64_t v)
		: data(v) {}
	explicit TmplValue(
		std::uint64_t v)
		: data(v) {}
	explicit TmplValue(
		double v)
		: data(v) {}
	explicit TmplValue(
		std::string s)
		: data(std::move(s)) {}
	explicit TmplValue(
		std::string_view sv)
		: data(std::string{sv}) {}
	explicit TmplValue(
		Array a)
		: data(std::move(a)) {}
	explicit TmplValue(
		Object o)
		: data(std::move(o)) {}
	[[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
	[[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
	[[nodiscard]] bool is_int() const noexcept { return std::holds_alternative<std::int64_t>(data); }
	[[nodiscard]] bool is_uint() const noexcept { return std::holds_alternative<std::uint64_t>(data); }
	[[nodiscard]] bool is_float() const noexcept { return std::holds_alternative<double>(data); }
	[[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(data); }
	[[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
	[[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(data); }
	template<class T>
	[[nodiscard]] decltype(auto) as() const {
		if constexpr (std::same_as<T, std::string_view>) {
			return std::string_view{std::get<std::string>(data)};
		} else {
			return std::get<T>(data);
		}
	}
	[[nodiscard]] Array &as_array() { return std::get<Array>(data); }
	[[nodiscard]] Array const &as_array() const { return std::get<Array>(data); }
	[[nodiscard]] Object &as_object() { return std::get<Object>(data); }
	[[nodiscard]] Object const &as_object() const { return std::get<Object>(data); }
	void set(
		std::string_view key,
		TmplValue val) {
		auto &obj = std::get<Object>(data);
		for (auto &[k, v]: obj) {
			if (k == key) {
				v = std::move(val);
				return;
			}
		}
		obj.emplace_back(std::string{key}, std::move(val));
	}
	void erase(
		std::string_view key) {
		auto &obj = std::get<Object>(data);
		std::erase_if(obj, [key](auto const &p) { return p.first == key; });
	}
	void push_back(
		TmplValue val) {
		std::get<Array>(data).push_back(std::move(val));
	}
	[[nodiscard]] bool operator ==(TmplValue const &) const = default;

	[[nodiscard]] std::string dump() const;
};
struct TemplateRenderCheckCase {
	std::string label;
	std::string template_name;
	TmplValue context{TmplValue::Object{}};
};

} // namespace conflux::templates

namespace conflux::templates {

std::vector<std::string> split_args(std::string_view s);
CompiledExpr compile_expr(std::string const &expr);
TmplValue node_to_tmpl(json::NodeRef n);
TmplValue const *obj_find(TmplValue const &obj, std::string_view key);
std::string str_replace_all(std::string_view src, std::string_view old_s, std::string_view new_s);
std::string str_capitalize(std::string s);
std::size_t find_matching_pair(std::string_view s, std::size_t open_pos, char open, char close) noexcept;
std::size_t find_top_level_token(std::string_view haystack, std::string_view needle) noexcept;
std::size_t find_top_level_char(std::string_view haystack, char needle) noexcept;
std::vector<std::pair<std::string, std::optional<TmplValue>>>
save_scope(TmplValue const &ctx, std::span<std::string const> names);
void restore_scope(TmplValue &ctx, std::vector<std::pair<std::string, std::optional<TmplValue>>> const &saved);

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Environment::Impl {
	std::string template_dir;
	EnvironmentOptions options;
	std::unordered_map<std::string, Template> cache;
	mutable std::shared_mutex cache_mtx;

	struct MacroBinding {
		std::vector<std::string> params;
		std::vector<CompiledExpr> defaults;
		NodeList body;
	};
	Template parse(std::string const &name, std::string const &source) const;
	TmplValue eval_expr(std::string const &expr, TmplValue const &context) const;
	TmplValue eval_expr(CompiledExpr const &expr, TmplValue const &context) const;
	TmplValue eval_fallback_base(std::string const &base, TmplValue const &context) const;
	std::optional<TmplValue> eval_fallback_literal(std::string_view base) const;
	std::optional<TmplValue> eval_fallback_collection(std::string_view base, TmplValue const &context) const;
	std::optional<TmplValue> eval_fallback_operator(std::string_view base, TmplValue const &context) const;
	TmplValue eval_fallback_path(std::string_view base, TmplValue const &context) const;
	TmplValue eval_base(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_base_operand(CompiledBaseExpr const &base, std::size_t index, TmplValue const &context) const;
	TmplValue eval_base_object(CompiledBaseExpr const &base, TmplValue const &context) const;
	template<class EvalRight>
	TmplValue eval_template_or(TmplValue left, EvalRight eval_right) const;
	template<class EvalRight>
	TmplValue eval_template_and(TmplValue left, EvalRight eval_right) const;
	TmplValue eval_template_not(TmplValue const &value) const;
	TmplValue eval_base_binary_or(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_base_binary_and(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_base_compare(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_base_concat(CompiledBaseExpr const &base, TmplValue const &context) const;
	TmplValue eval_path(std::vector<CompiledPathSegment> const &path, TmplValue const &context) const;
	template<class EvalArg>
	TmplValue apply_template_get_method(TmplValue const &val, std::size_t arg_count, EvalArg eval_arg) const;
	template<class EvalArg>
	TmplValue apply_template_replace_method(TmplValue const &val, EvalArg eval_arg) const;
	TmplValue apply_template_title_method(TmplValue const &val) const;
	TmplValue apply_template_upper_method(TmplValue const &val) const;
	TmplValue apply_template_lower_method(TmplValue const &val) const;
	template<class EvalArg>
	TmplValue apply_template_startswith_method(TmplValue const &val, EvalArg eval_arg) const;
	template<class EvalArg>
	TmplValue apply_template_split_method(TmplValue const &val, std::size_t arg_count, EvalArg eval_arg) const;
	template<class EvalArg>
	TmplValue
	apply_template_method(std::string const &name, TmplValue const &val, std::size_t arg_count, EvalArg eval_arg) const;
	TmplValue apply_method(
		std::string const &name,
		TmplValue const &val,
		std::vector<CompiledExprPtr> const &args,
		TmplValue const &context) const;
	TmplValue apply_fallback_path_method(
		std::string const &name,
		TmplValue const &val,
		std::vector<std::string> const &args,
		TmplValue const &context) const;
	TmplValue apply_filter(CompiledFilter const &filter, TmplValue const &val, TmplValue const &context) const;
	static std::string value_to_string(TmplValue const &v);
	static bool is_truthy(TmplValue const &v);
	static constexpr int kMaxTemplateDepth = 256;
	std::string render_expr_node(
		ExprNode const &node,
		TmplValue &context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth) const;
	std::string render_block_node(
		BlockNode const &node,
		TmplValue context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth) const;
	std::string render_include_node(
		IncludeNode const &node,
		TmplValue context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		int depth) const;
	void render_set_node(SetNode const &node, TmplValue &context) const;
	std::string render_for_node(
		ForNode const &node,
		TmplValue &context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth) const;
	std::string render_if_node(
		IfNode const &node,
		TmplValue context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth) const;
	static void register_macro_node(MacroNode const &node, std::unordered_map<std::string, MacroBinding> &macros);
	static void import_macro_node(
		FromImportNode const &node,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, MacroBinding> &macros);
	std::string render_nodes(
		NodeList const &nodes,
		TmplValue context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *blocks,
		std::unordered_map<std::string, MacroBinding> *macros,
		int depth = 0) const;
	std::string render_template(
		Template const &tmpl,
		TmplValue context,
		std::unordered_map<std::string, Template> const &active_cache,
		std::unordered_map<std::string, NodeList> const *child_blocks = nullptr,
		int depth = 0) const;
	std::expected<std::unordered_map<std::string, Template>, TemplateBuildReport> build_cache_from_directory() const;
	std::expected<void, TemplateBuildReport>
	reload_all_checked(std::span<TemplateRenderCheckCase const> cases = {}, TemplateRenderCheckOptions opts = {});
	void check_render(
		std::unordered_map<std::string, Template> const &candidate,
		std::span<TemplateRenderCheckCase const> cases,
		TemplateRenderCheckOptions opts,
		TemplateBuildReport &report) const;
	void validate_links(std::unordered_map<std::string, Template> const &candidate, TemplateBuildReport &report) const;
	void reload_path(std::string const &path);
	void remove_path(std::string const &path);
	bool extension_allowed(std::filesystem::path const &path) const;
};

} // namespace conflux::templates
