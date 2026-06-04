# Configuration And Capability Reports

Conflux exposes runtime configuration diagnostics without changing startup defaults.

- `conflux::build_info()` returns version, compiler, stdlib, interface mode, feature set, and compiled optional features.
- `conflux::build_info_summary()` returns a compact one-line summary for logs.
- `conflux::runtime::detect_capabilities()` reports io_uring/runtime capabilities from the running host.
- `conflux::runtime::capability_report(caps)` formats the capability snapshot for diagnostics.
- `Config::summary_redacted()` and `Config::to_json_redacted()` expose effective config without printing secret material.
- `conflux::http::HttpServer::startup_report()` combines build info, capabilities, fallback policy, and redacted config for owner-controlled startup logging.

`examples/advanced/capability_report.cxx` is the minimal executable pattern for printing the build summary and runtime capability report before starting an HTTP service.

## Presets

| Preset | Intended use | Main differences |
|---|---|---|
| `Config::public_server()` | Default web-facing app/server configuration | Bounded request/body/header limits, request and TLS/plain sniff timeouts, `strict_config`, slow-handler diagnostics, explicit fallback diagnostics. |
| `Config::development()` | Local development with compatible config parsing | Keeps non-strict INI compatibility, enables slow-handler diagnostics and startup banner. |
| `Config::low_latency()` | Bounded low-latency tuning | Smaller rings, explicit taskrun/cooperative ring flags, and fail-fast feature fallback while keeping parser/body limits bounded. |

Non-production presets — not for real deployments:

| Preset | Use | Notes |
|---|---|---|
| `benchmark` | Measurement-only mode | Starts from `low_latency()`, disables startup banner and request/sniff timeouts. Use only inside a controlled benchmark harness. |
| `Config::unsafe_max_speed()` | Unsafe opt-in throughput experiments | Raises parser/body caps and enables registered send buffers / SEND_ZC; `unsafe_config_issues(...)` reports these choices and this preset must not be a production default. |

Strict checked loads report structured `ConfigIssue` values such as `config.unknown_key` with file, line, section, key, value, and hints where available.

Fallback policy is explicit through `Config::feature_fallback`:

- `fail_fast`
- `warn_and_fallback`
- `silent_fallback`

`App::validate()` includes route issues plus `config_issues` and `capability_issues`; use `summary()` or `detailed_summary()` for startup diagnostics.
`conflux::http::HttpServer::try_create()` rejects invalid config and fail-fast capability mismatches before constructing the server.
