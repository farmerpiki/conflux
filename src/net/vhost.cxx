// Virtual hosting: dispatch to different Routers based on the Host header.
module;

export module conflux.net.vhost;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;
import conflux.work;
// VHostRouter routes requests to per-host Routers.
// The Host port (":port") is stripped before matching.
// A default Router handles requests for unknown hosts.
export class VHostRouter {
public:
	// Register a Router for an exact host name (e.g. "api.example.com").
	VHostRouter &add(
		std::string host,
		Router router) {
		router.set_work_pool(work_pool_);
		vhosts_.emplace(ascii_lower(host), std::move(router));
		return *this;
	}
	// Default Router for hosts with no explicit match.
	VHostRouter &set_default(
		Router router) {
		router.set_work_pool(work_pool_);
		default_ = std::make_unique<Router>(std::move(router));
		return *this;
	}
	VHostRouter &set_work_pool(
		std::shared_ptr<WorkPool> pool) {
		work_pool_ = std::move(pool);
		for (auto &[host, router]: vhosts_) {
			router.set_work_pool(work_pool_);
		}
		if (default_ != nullptr) {
			default_->set_work_pool(work_pool_);
		}
		return *this;
	}
	[[nodiscard]] std::shared_ptr<WorkPool> work_pool() const { return work_pool_; }
	[[nodiscard]] std::shared_ptr<WorkPool> resolved_work_pool(
		std::string_view host_header) const {
		auto host = ascii_lower(conflux::http::host_without_port(host_header));
		auto it = vhosts_.find(std::string{host});
		if (it != vhosts_.end()) {
			return it->second.work_pool();
		}
		return default_ != nullptr ? default_->work_pool() : nullptr;
	}
	[[nodiscard]] Response dispatch(
		RequestView const &req) const {
		auto host = ascii_lower(conflux::http::host_without_port(req.headers["host"]));
		auto it = vhosts_.find(std::string{host});
		if (it != vhosts_.end()) {
			return it->second.dispatch(req);
		}
		if (default_) {
			return default_->dispatch(req);
		}
		return Response::not_found(req.path);
	}
	[[nodiscard]] Response dispatch(
		Request const &req) const {
		return dispatch(RequestView{req});
	}
	[[nodiscard]] bool has_context_routes() const noexcept {
		return std::ranges::any_of(vhosts_, [](auto const &kv) noexcept { return kv.second.has_context_routes(); })
			|| (default_ && default_->has_context_routes());
	}
	[[nodiscard]] std::optional<Response> dispatch_context(
		RequestView const &req,
		RequestContext const &ctx) const {
		auto host = ascii_lower(conflux::http::host_without_port(req.headers["host"]));
		auto it = vhosts_.find(std::string{host});
		if (it != vhosts_.end()) {
			return it->second.dispatch_context(req, ctx);
		}
		if (default_) {
			return default_->dispatch_context(req, ctx);
		}
		return std::nullopt;
	}

private:
	std::unordered_map<std::string, Router> vhosts_;
	std::unique_ptr<Router> default_;
	std::shared_ptr<WorkPool> work_pool_{std::make_shared<WorkPool>()};
};
