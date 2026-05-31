# Configuration And Capability Reports

Conflux exposes runtime configuration diagnostics without changing startup defaults.

- `conflux::build_info()` returns version, compiler, stdlib, interface mode, feature set, and compiled optional features.
- `conflux::build_info_summary()` returns a compact one-line summary for logs.
- `conflux::runtime::detect_capabilities()` reports io_uring/runtime capabilities. Mock-liburing builds report `mock_backend` instead of probing hardware.
- `conflux::runtime::capability_report(caps)` formats the capability snapshot for diagnostics.
- `Config::summary_redacted()` and `Config::to_json_redacted()` expose effective config without printing secret material.
- `conflux::http::HttpServer::startup_report()` combines build info, capabilities, fallback policy, and redacted config for owner-controlled startup logging.

`Config::development()` keeps non-strict INI compatibility. `Config::public_server()` enables `strict_config` and slow-handler diagnostics; strict checked loads report structured `ConfigIssue` values such as `config.unknown_key` with file, line, section, key, value, and hints where available.

Fallback policy is explicit through `Config::feature_fallback`:

- `fail_fast`
- `warn_and_fallback`
- `silent_fallback`

`App::validate()` includes route issues plus `config_issues` and `capability_issues`; use `summary()` or `detailed_summary()` for startup diagnostics.
`conflux::http::HttpServer::try_create()` rejects invalid config and fail-fast capability mismatches before constructing the server.
