// Request tracing middleware: W3C traceparent propagation + before/after hooks.
// Parses incoming traceparent header (trace-id, parent-id), generates a new
// std::span-id for this hop, and exposes the context to before/after callbacks.
// The outgoing traceparent is set in the response header.
export module conflux.net.tracing;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;

template<typename>
class TraceCallback;
template<typename R, typename... Args>
class TraceCallback<R(Args...)> {
	struct Concept {
		virtual ~Concept() = default;
		virtual R invoke(Args... args) = 0;
		[[nodiscard]] virtual std::unique_ptr<Concept> clone() const = 0;
	};
	template<typename F>
	struct Model final : Concept {
		F fn;
		explicit Model(
			F f)
			: fn(std::move(f)) {}
		R invoke(
			Args... args) override {
			if constexpr (std::is_void_v<R>) {
				fn(std::forward<Args>(args)...);
			} else {
				return fn(std::forward<Args>(args)...);
			}
		}
		[[nodiscard]] std::unique_ptr<Concept> clone() const override { return std::make_unique<Model>(fn); }
	};
	std::unique_ptr<Concept> impl_{};

public:
	TraceCallback() = default;
	template<typename F>
		requires(!std::same_as<std::remove_cvref_t<F>, TraceCallback> && std::invocable<F &, Args...>)
	TraceCallback(
		F &&fn)
		: impl_(std::make_unique<Model<std::remove_cvref_t<F>>>(std::forward<F>(fn))) {}
	TraceCallback(
		TraceCallback const &other)
		: impl_(other.impl_ ? other.impl_->clone() : nullptr) {}
	TraceCallback(TraceCallback &&) noexcept = default;
	TraceCallback &operator =(
		TraceCallback const &other) {
		if (this == &other) {
			return *this;
		}
		impl_ = other.impl_ ? other.impl_->clone() : nullptr;
		return *this;
	}
	TraceCallback &operator =(TraceCallback &&) noexcept = default;
	explicit operator bool() const noexcept { return static_cast<bool>(impl_); }
	R operator ()(
		Args... args) const {
		return impl_->invoke(std::forward<Args>(args)...);
	}
};
export struct TraceContext {
	std::string trace_id; // 32 hex chars
	std::string span_id; // 16 hex chars (this hop)
	std::string parent_id; // 16 hex chars (caller's std::span), may be empty
	std::string traceparent; // full W3C header value: "00-trace-std::span-01"
};
export struct TracingOptions {
	// Called before the downstream handler. May modify the request (e.g. inject trace headers).
	TraceCallback<void(HttpRequest &, TraceContext const &)> on_start{};
	// Called after the downstream handler. May modify the response (e.g. add trace headers).
	TraceCallback<void(HttpRequest const &, HttpResponse &, TraceContext const &)> on_end{};
	// Forward the traceparent header in the response.
	bool propagate_in_response{true};
};
namespace tracing_detail {

std::string gen_hex(
	std::size_t nbytes) {
	std::vector<unsigned char> buf(nbytes);
	random_bytes(buf);
	static constexpr std::string_view kHex = "0123456789abcdef";
	std::string out;
	out.reserve(nbytes * 2);
	for (auto b: buf) {
		out += kHex[b >> 4];
		out += kHex[b & 0xF];
	}
	return out;
}
// Parse W3C traceparent: "00-<trace_id>-<parent_id>-<flags>".
// Returns {trace_id, parent_id} or empty strings on parse failure.
std::pair<std::string, std::string> parse_traceparent(
	std::string_view tp) {
	// version(2)-trace_id(32)-parent_id(16)-flags(2) separated by '-'
	if (tp.size() < 55 || tp[2] != '-' || tp[35] != '-' || tp[52] != '-') {
		return {};
	}
	auto is_hex = [](std::string_view s) {
		return std::ranges::all_of(s, [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		});
	};
	auto trace_id = tp.substr(3, 32);
	auto parent_id = tp.substr(36, 16);
	if (!is_hex(trace_id) || !is_hex(parent_id)) {
		return {};
	}
	return {std::string{trace_id}, std::string{parent_id}};
}

} // namespace tracing_detail
export Router::Middleware tracing_middleware(
	TracingOptions opts = {}) {
	return [opts = std::move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		TraceContext ctx;
		// Parse incoming traceparent.
		auto incoming_tp = req.headers["traceparent"];
		if (!incoming_tp.empty()) {
			auto [tid, pid] = tracing_detail::parse_traceparent(incoming_tp);
			ctx.trace_id = std::move(tid);
			ctx.parent_id = std::move(pid);
		}
		if (ctx.trace_id.empty()) {
			ctx.trace_id = tracing_detail::gen_hex(16);
		}
		ctx.span_id = tracing_detail::gen_hex(8);
		ctx.traceparent = std::format("00-{}-{}-01", ctx.trace_id, ctx.span_id);

		auto modified = req.to_owned();
		modified.headers["traceparent"] = ctx.traceparent;
		if (opts.on_start) {
			opts.on_start(modified, ctx);
		}

		auto resp = next(modified);

		if (opts.propagate_in_response) {
			resp.headers["Traceparent"] = ctx.traceparent;
		}
		if (opts.on_end) {
			opts.on_end(modified, resp, ctx);
		}
		return resp;
	};
}
