// Virtual hosting: dispatch to different Routers based on the Host header.
export module conflux.net.vhost;
import std;
import conflux.net.router;
import conflux.utils;
import conflux.work;
using namespace std;

// VHostRouter routes requests to per-host Routers.
// The Host port (":port") is stripped before matching.
// A default Router handles requests for unknown hosts.
export class VHostRouter {
	[[nodiscard]] static string_view normalized_host(
		string_view host_header) {
		if (host_header.starts_with('[')) {
			auto bracket = host_header.find(']');
			if (bracket == string_view::npos) {
				return host_header;
			}
			auto colon = host_header.find(':', bracket + 1);
			return colon != string_view::npos ? host_header.substr(0, colon) : host_header;
		}
		auto colon = host_header.rfind(':');
		return colon != string_view::npos ? host_header.substr(0, colon) : host_header;
	}

public:
	// Register a Router for an exact host name (e.g. "api.example.com").
	VHostRouter &add(
		string host,
		Router router) {
		router.set_work_pool(work_pool_);
		vhosts_.emplace(ascii_lower(host), move(router));
		return *this;
	}

	// Default Router for hosts with no explicit match.
	VHostRouter &set_default(
		Router router) {
		router.set_work_pool(work_pool_);
		default_ = make_unique<Router>(move(router));
		return *this;
	}

	VHostRouter &set_work_pool(
		shared_ptr<WorkPool> pool) {
		work_pool_ = move(pool);
		for (auto &[host, router]: vhosts_) {
			router.set_work_pool(work_pool_);
		}
		if (default_ != nullptr) {
			default_->set_work_pool(work_pool_);
		}
		return *this;
	}

	[[nodiscard]] shared_ptr<WorkPool> work_pool() const { return work_pool_; }

	[[nodiscard]] shared_ptr<WorkPool> resolved_work_pool(
		string_view host_header) const {
		auto host = ascii_lower(normalized_host(host_header));
		auto it = vhosts_.find(string{host});
		if (it != vhosts_.end()) {
			return it->second.work_pool();
		}
		return default_ != nullptr ? default_->work_pool() : nullptr;
	}

	[[nodiscard]] HttpResponse dispatch(
		HttpRequestView const &req) const {
		auto host = ascii_lower(normalized_host(req.headers["host"]));
		auto it = vhosts_.find(string{host});
		if (it != vhosts_.end()) {
			return it->second.dispatch(req);
		}
		if (default_) {
			return default_->dispatch(req);
		}
		return HttpResponse::not_found(req.path);
	}

	[[nodiscard]] HttpResponse dispatch(
		HttpRequest const &req) const {
		return dispatch(HttpRequestView{req});
	}

private:
	unordered_map<string, Router> vhosts_;
	unique_ptr<Router> default_;
	shared_ptr<WorkPool> work_pool_{make_shared<WorkPool>()};
};
