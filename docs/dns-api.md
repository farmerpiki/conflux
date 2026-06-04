# Conflux DNS API Reference

- **Module:** `conflux.net.dns`
- **Namespace:** `conflux::net::dns`
- **Component:** `conflux::dns`
- **Backend:** native UDP resolver on `SocketTaskRing` or worker-backed NSS lookup

## Resolver

```cpp
struct ResolveOptions {
    AddressFamily prefer{AddressFamily::v6};
    bool allow_v4{true};
    bool allow_v6{true};
    std::chrono::milliseconds query_timeout{2000};
    std::chrono::milliseconds total_timeout{5000};
    bool bypass_cache{false};
    std::vector<NameserverEndpoint> override_nameservers{};
};

class Resolver {
public:
    Resolver(io_uring*, CompletionTable*, UserDataFn, ResolverOptions opts = {});
    explicit Resolver(WorkPool&, ResolverOptions opts = {});

    root::Task<ResolveResult> resolve(std::string_view host,
                                      std::uint16_t port,
                                      ResolveOptions const& opts = {});

    root::Task<ResolveResult> resolve(SocketTaskRing& ring,
                                      std::string_view host,
                                      std::uint16_t port,
                                      ResolveOptions const& opts = {});

    std::expected<ResolveResult, DnsError>
    resolve_blocking(std::string_view host,
                     std::uint16_t port,
                     ResolveOptions const& opts = {});
};
```

`resolve(...)` is the async API. It returns a `root::Task<ResolveResult>` and
uses the resolver's own ring or the caller-provided `SocketTaskRing`. The
caller-provided ring must outlive the returned task and any coalesced waiters
sharing that ring.

## Cancellation And Timeouts

Async DNS cancellation is waiter-scoped. Cancelling a queued waiter completes
that waiter as cancelled, but it does not necessarily cancel a shared UDP query
that other coalesced waiters still own. Native UDP receive cancellation is
best-effort, and late DNS responses update only live waiters that still own the
query state.

`query_timeout` bounds each query attempt. `total_timeout` bounds the full
resolution attempt across candidates, retries, and address-family fallback.
`resolve_blocking(...)` reports DNS failures as `std::expected` errors; async
`resolve(...)` reports through the returned task.
