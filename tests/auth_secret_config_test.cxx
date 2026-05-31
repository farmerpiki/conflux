// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.password_hash;
import conflux.net.jwt;
import conflux.net.cookie_signing;

using conflux::http::Config;
using conflux::http::config_from_ini;

namespace {

class TempIni {
	std::filesystem::path path_;

public:
	explicit TempIni(
		std::string_view body) {
		auto const name = std::format(
			"conflux-auth-secret-config-test-{}-{}.ini",
			std::chrono::steady_clock::now().time_since_epoch().count(),
			reinterpret_cast<std::uintptr_t>(this));
		path_ = std::filesystem::temp_directory_path() / name;
		std::ofstream out{path_};
		if (!out) {
			throw std::runtime_error{"failed to create temp ini"};
		}
		out << body;
	}
	~TempIni() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	TempIni(TempIni const &) = delete;
	TempIni &operator =(TempIni const &) = delete;
	[[nodiscard]] char const *c_str() const noexcept { return path_.c_str(); }
};

} // namespace

TEST_CASE(
	"auth secret config: missing required production secrets fail closed",
	"[auth][config]") {
	Config cfg{};

	CHECK_FALSE(conflux::http::password_hash_secrets_from_config(cfg).has_value());
	CHECK_FALSE(conflux::http::jwt_options_from_config(cfg).has_value());
	CHECK_FALSE(conflux::http::cookie_signing_options_from_config(cfg).has_value());
}

TEST_CASE(
	"auth secret config: typed sources resolve password pepper and rotated JWT/cookie/session secrets",
	"[auth][config]") {
	TempIni ini{R"ini(
[auth]
password_verifier_secret = password-pepper-16-bytes
password_verifier_min_secret_bytes = 16
jwt_secret = jwt-active-secret-16-bytes
jwt_previous_secret = jwt-old-secret-16-bytes
jwt_min_secret_bytes = 16
cookie_secret = cookie-active-secret-16
cookie_previous_secret = cookie-old-secret-16
cookie_min_secret_bytes = 16
session_secret = session-active-secret-16
session_previous_secret = session-old-secret-16
session_min_secret_bytes = 16
)ini"};

	auto cfg = config_from_ini(ini.c_str());

	auto password_secrets = conflux::http::password_hash_secrets_from_config(cfg);
	REQUIRE(password_secrets.has_value());
	CHECK(password_secrets->verifier_secret == "password-pepper-16-bytes");

	auto jwt_opts = conflux::http::jwt_options_from_config(cfg, conflux::http::JwtOptions{.verify_exp = false});
	REQUIRE(jwt_opts.has_value());
	CHECK(jwt_opts->secrets.active == "jwt-active-secret-16-bytes");
	REQUIRE(jwt_opts->secrets.previous.size() == 1);
	CHECK(jwt_opts->secrets.previous[0] == "jwt-old-secret-16-bytes");

	auto old_token = conflux::http::jwt_sign(R"({"sub":"rotated"})", "jwt-old-secret-16-bytes");
	auto decoded_old = conflux::http::jwt_decode(old_token, *jwt_opts);
	REQUIRE(decoded_old.has_value());
	CHECK(decoded_old->sub == "rotated");

	auto cookie_opts = conflux::http::cookie_signing_options_from_config(cfg);
	REQUIRE(cookie_opts.has_value());
	CHECK(cookie_opts->secrets.active == "cookie-active-secret-16");
	REQUIRE(cookie_opts->secrets.previous.size() == 1);
	CHECK(cookie_opts->secrets.previous[0] == "cookie-old-secret-16");

	auto signed_old_cookie = conflux::http::sign_cookie("user42", "cookie-old-secret-16");
	auto verified_old_cookie = conflux::http::verify_cookie(signed_old_cookie, cookie_opts->secrets);
	REQUIRE(verified_old_cookie.has_value());
	CHECK(*verified_old_cookie == "user42");

	auto session = resolve_secret_rotation(cfg.auth_secrets.session, "session");
	REQUIRE(session.has_value());
	CHECK(session->active == "session-active-secret-16");
	REQUIRE(session->previous.size() == 1);
	CHECK(session->previous[0] == "session-old-secret-16");
}
