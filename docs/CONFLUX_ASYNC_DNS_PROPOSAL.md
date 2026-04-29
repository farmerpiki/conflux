# conflux.net.dns — Async DNS Resolver Proposal (v2)

New module providing async name resolution for `conflux.net.client`, `conflux.net.smtp`, and any future network module. Replaces blocking `::getaddrinfo` in two confirmed call sites.

**v2 changes** (after adversarial review):
- Drop process-wide `default_resolver()` singleton — explicit ring + completion table at construction.
- Fix happy-eyeballs: A and AAAA queries fired **in parallel**; staggering belongs at the connect layer.
- Promote EDNS0 OPT (4096 advertised UDP size) to v1 requirement.
- Match existing Flow<T> throws-based convention (FileIoError, db) — `Flow<ResolveResult>` throwing `DnsError`. The `resolve_blocking` synchronous façade returns `expected<…, DnsError>`.
- In-flight query coalescing in v1 (deduplicate concurrent identical lookups).
- Per-query ephemeral source ports (RFC 5452 mitigation against Kaminsky-class attacks).
- Hostname validator relaxed to RFC 2181 §11 (any label bytes, length-only checks).
- Nameserver overrides typed as `NameserverEndpoint`, not `string`.
- Effort sizing rebudgeted: ~4.5 K LOC, ~6 work-weeks single-engineer.

**Confirmed call sites**:
- `src/net/client.cxx:162-213` `resolve_and_connect` → `::getaddrinfo` at `:176`
- `src/net/smtp.cxx:37-80` `open_connected_socket` → `::getaddrinfo` at `:47`

**Goals**:
- Non-blocking under Carrier; cancellable; timed.
- RFC 8305 happy-eyeballs (parallel queries, staggered connects).
- TTL-respecting in-process cache + in-flight coalescing.
- Drop-in replacement; old `getaddrinfo` paths deleted.

**Non-goals (v1)**: DNSSEC, DoH/DoT, SRV/MX, IDNA, mDNS, full nsswitch.

## Implementation Status (2026-04-29)

- Completed: `conflux.net.client` and `conflux.net.smtp` integration; blocking `getaddrinfo` removed from those paths.
- Completed: response validation hardening (ID/question/source checks) and negative caching.
- Completed: `resolv.conf` `nameserver/search/options` parsing, reload support, attempts handling, and default timeout adoption.
- Completed: search-domain candidate expansion for both `resolve_blocking()` and async `resolve()`.
- Completed: TC handling now reports `DnsErrorKind::truncated` when UDP is truncated and TCP fallback fails.
- Completed: cancellation mapping/cleanup for in-flight async resolver flows (`DnsErrorKind::cancelled` fanout + coalesced waiter rejection).
- Completed: explicit TCP fallback timeout wiring in `tcp_single_query` (socket send/recv timeouts).
- Completed: caller-side connect-attempt staggering in `conflux.net.client`.
- Remaining from this proposal: none for v1 scope documented here.

---

## Architecture

Two backends behind one front:

```
┌─────────────────────────────────────────────────────────────┐
│  conflux::net::dns::Resolver                                │
│  Flow<ResolveResult> resolve(host, port, opts)              │
│  expected<ResolveResult, DnsError> resolve_blocking(...)    │
└─────────────────────────────────────────────────────────────┘
        │                                          │
        ▼                                          ▼
┌────────────────────────┐              ┌────────────────────────┐
│ Native UDP backend     │              │ Thread-pool backend    │
│ (io_uring sendmsg/recv)│              │ (WorkPool getaddrinfo) │
│ — RFC 1035 codec       │              │ — fallback / opt-in    │
│ — EDNS0 OPT (4096)     │              │ — preserves NSS chain  │
│ — TCP fallback on TC   │              │ — no ring required     │
│ — /etc/hosts shortcut  │              │                        │
│ — /etc/resolv.conf     │              │                        │
└────────────────────────┘              └────────────────────────┘
```

**Default backend** depends on whether the caller supplies a ring:
- Constructor with ring + completion table → `native_udp`.
- Constructor without (thread-pool variant) → `nss_thread`.
- HttpClient/SMTP fall back to `nss_thread` automatically when no resolver is supplied.

---

## Public Surface

```cxx
export module conflux.net.dns;
import std;
import conflux.types;
import conflux.work;          // Flow<T>, FlowSource, WorkPool
import conflux.file_io;       // CompletionTable, UserDataFn (mirrors FileReader pattern)

export namespace conflux::net::dns {

// ─── data types ─────────────────────────────────────────────────────────────

enum class AddressFamily : u8 { v4, v6 };

struct Endpoint {
    sockaddr_storage addr{};
    socklen_t        addr_len{};
    AddressFamily    family{};
    // Port is encoded inside addr; resolver writes the requested port at lookup time.
};

struct ResolveResult {
    std::vector<Endpoint>     endpoints;        // family preference applied
    std::chrono::nanoseconds  elapsed{};
    bool                      from_cache{false};
    bool                      from_hosts_file{false};
    bool                      from_coalesced{false}; // attached to in-flight query
};

// ─── errors (throws-based, matching FileIoError pattern) ────────────────────

enum class DnsErrorKind : u8 {
    timeout,
    nxdomain,           // RCODE 3
    servfail,           // RCODE 2
    refused,            // RCODE 5
    formerr,            // RCODE 1
    malformed,          // unparseable response
    no_servers,         // /etc/resolv.conf empty / missing
    network,            // sendmsg/recvmsg failure (errno in os_errno)
    truncated,          // UDP TC and TCP fallback failed
    cancelled,          // Carrier cancellation
    invalid_hostname,   // length / encoding violation
    no_ring,            // native_udp backend used without a ring (programmer error)
};

struct DnsError final : std::runtime_error {
    DnsError(DnsErrorKind k, std::string msg, int err = 0, std::optional<u8> r = {});
    DnsErrorKind        kind;
    int                 os_errno;
    std::optional<u8>   rcode;
};

// ─── nameserver endpoint ────────────────────────────────────────────────────

struct NameserverEndpoint {
    sockaddr_storage addr{};
    socklen_t        addr_len{};
    u16              port{53};
};

// Helpers — accept "ip", "ip:port", "[ipv6]:port", "[ipv6]".
[[nodiscard]] std::expected<NameserverEndpoint, std::string>
parse_nameserver(std::string_view literal);

// ─── options ────────────────────────────────────────────────────────────────

enum class ResolverBackend : u8 {
    native_udp,
    nss_thread,
};

struct ResolveOptions {
    AddressFamily            prefer{AddressFamily::v6};
    bool                     allow_v4{true};
    bool                     allow_v6{true};
    std::chrono::milliseconds query_timeout{2000};
    std::chrono::milliseconds total_timeout{5000};
    bool                     bypass_cache{false};
    // Per-call nameserver override (test/CLI use); empty = use Resolver default.
    std::vector<NameserverEndpoint> override_nameservers{};
};

struct ResolverOptions {
    size_t                  cache_capacity{1024};
    std::chrono::seconds    cache_max_ttl{300};
    std::chrono::seconds    cache_negative_ttl{30};
    std::filesystem::path   resolv_conf{"/etc/resolv.conf"};
    std::filesystem::path   hosts_file{"/etc/hosts"};
    bool                    enable_etc_hosts{true};
    u16                     edns0_udp_size{4096}; // 0 disables EDNS0
    size_t                  max_in_flight_queries{4096}; // backpressure cap
    std::vector<NameserverEndpoint> override_nameservers{}; // global default
};

// ─── resolver — explicit constructor; mirrors FileReader ────────────────────

class Resolver {
 public:
    // Native-UDP construction. Ring + completion table provided by caller, exactly
    // as FileReader does. user_data encoder is the caller's responsibility (the
    // caller multiplexes opcodes through their own bit layout).
    Resolver(io_uring          *ring,
             CompletionTable   *completions,
             UserDataFn         encode_ud,
             ResolverOptions    opts = {});

    // Thread-pool construction. No ring required; runs ::getaddrinfo on a worker.
    explicit Resolver(WorkPool        &pool,
                      ResolverOptions  opts = {});

    ~Resolver();
    Resolver(Resolver const &) = delete;
    Resolver &operator=(Resolver const &) = delete;

    // Async resolve. Flow throws DnsError on failure (matches FileIoError pattern).
    [[nodiscard]] Flow<ResolveResult>
    resolve(std::string_view host, u16 port, ResolveOptions opts = {});

    // Synchronous wrapper. Returns expected to match the existing HttpClient
    // synchronous boundary convention (HttpResult). Detects when called from
    // a ring-owning thread and faults with DnsError{kind=no_ring} to prevent
    // self-deadlock.
    [[nodiscard]] std::expected<ResolveResult, DnsError>
    resolve_blocking(std::string_view host, u16 port, ResolveOptions opts = {});

    // Manual cache management.
    void invalidate(std::string_view host);
    void clear_cache();

    // Backend the resolver is using.
    [[nodiscard]] ResolverBackend backend() const noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Thread-local current resolver — used by deadlock detection in resolve_blocking
// and by HttpClient/SMTP to discover the ambient resolver. Same shape as
// current_file_reader() at file_io.cxx:2720.
[[nodiscard]] Resolver *current_resolver() noexcept;

class CurrentResolverScope {
    Resolver *prev_;
 public:
    explicit CurrentResolverScope(Resolver *next) noexcept;
    ~CurrentResolverScope();
    CurrentResolverScope(CurrentResolverScope const &) = delete;
    CurrentResolverScope &operator=(CurrentResolverScope const &) = delete;
};

}
```

**No process-wide singleton.** Each caller (HttpClient, SMTP) either receives an explicit `Resolver*` in its options or constructs a thread-pool resolver as fallback. This keeps lifetimes auditable and matches FileReader.

---

## Native UDP backend — implementation

### State

```cxx
struct Resolver::Impl {
    ResolverOptions                  opts;
    io_uring                        *ring{};      // null in thread-pool mode
    CompletionTable                 *completions{};
    UserDataFn                       encode_ud;
    WorkPool                        *pool{};      // non-null in thread-pool mode

    std::vector<NameserverEndpoint>  nameservers; // resolv.conf-derived
    HostsFile                        hosts;
    Cache                            cache;       // LRU
    InFlightTable                    in_flight;   // dedup concurrent identical queries

    std::shared_mutex                state_mtx;
    std::atomic<u16>                 next_query_id{1};
};
```

### Query lifecycle

1. **Hostname validate**:
   - Total length ≤ 253; per-label length ≤ 63; non-empty labels; ASCII bytes only.
   - **Allow** `_` and other non-preferred-syntax bytes (RFC 2181 §11). Rejecting them breaks SRV/DANE/DMARC names downstream.
2. **Numeric short-circuit**: `inet_pton` for both families; literal IP returns immediately.
3. **/etc/hosts**: lookup in pre-parsed map; hit returns immediately.
4. **Cache lookup**: by `(lowercased_host, family-set, port)`. Unexpired hit returns immediately.
5. **In-flight coalesce**: check `in_flight` table for `(lowercased_host, family-set)`. If present, attach a new Flow to the existing query state (`from_coalesced=true`). Subsequent fan-out on completion fulfills all attached Flows. **No duplicate upstream query.**
6. **Construct queries** — A AND AAAA in parallel (RFC 8305 §3). Each query gets:
   - Fresh 16-bit ID.
   - Fresh ephemeral UDP socket (per-query) bound to port 0 — kernel chooses port. Source-port randomization mitigates RFC 5452 spoofing.
   - EDNS0 OPT pseudo-RR with `udp_size = opts.edns0_udp_size` (default 4096).
7. **io_uring submit**:
   - `IORING_OP_SENDTO` per nameserver per family (parallel).
   - `IORING_OP_RECVMSG` on each query's ephemeral socket.
   - `IORING_OP_TIMEOUT_LINK` for `query_timeout`.
   - Total deadline `IORING_OP_TIMEOUT` for `total_timeout`.
8. **Response validate** (RFC 5452):
   - ID matches outstanding query.
   - Source addr matches one of our submitted nameservers.
   - Source port matches the dest port we sent to (i.e., the nameserver's port).
   - Question section echoes our QNAME + QTYPE + QCLASS.
   - Drop response (continue waiting) if any check fails.
9. **TC bit** → TCP fallback (separate connection per query, length-prefixed framing).
10. **RCODE map**:
    - 0 NOERROR → parse RDATA.
    - 1 FORMERR → `DnsError::formerr`.
    - 2 SERVFAIL → `DnsError::servfail`.
    - 3 NXDOMAIN → `DnsError::nxdomain` (cache for `cache_negative_ttl`).
    - 5 REFUSED → `DnsError::refused`.
11. **Build Endpoints** — order: preferred-family first, then other. **DNS-resolution staggering removed.** Connection-attempt staggering happens in the caller's connect loop, not here.
12. **Cache write** — TTL = min(server-TTL, `cache_max_ttl`).
13. **Fulfill all attached Flows** (in-flight coalescing).

### Connection-attempt staggering (RFC 8305 §4)

**Lives in the caller, not the resolver.** Update plan for `src/net/client.cxx:184-205`:
```cxx
// After: auto rr = resolver.resolve(...);
auto const &endpoints = rr->endpoints;
// Try preferred-family endpoint at t=0; staggered v4 connect at t=delay.
// If preferred-family connect fails before delay, jump straight to next.
// connection_attempt_delay default 250ms (RFC 8305 §8).
```

This is part of R9c (redirect-follow + connect-loop overhaul) in the main remediation plan, NOT R11.

### Cancellation

`Flow::request_cancel` → submit `IORING_OP_ASYNC_CANCEL` for outstanding sendto/recvmsg/timeout. In-flight state rejects with `DnsError{kind=cancelled}` for all attached Flows.

### Source-port randomization detail

Each outstanding query gets its own UDP socket, registered with `IORING_REGISTER_FILES` for fast SQE reuse. Socket pool size capped by `max_in_flight_queries`. On query completion or timeout, socket is closed (or returned to the registered slot for reuse). The pool size cap also serves as backpressure — `resolve()` rejects with `DnsError{kind=network, errno=EAGAIN}` if the pool is saturated.

### TCP fallback

When TC bit set:
- `IORING_OP_CONNECT` to same nameserver, TCP/53.
- 2-byte length-prefixed query (RFC 1035 §4.2.2).
- Recv length-prefixed response.
- Reuse codec; reuse RFC 5452 validation (TCP source port = 53 sanity check).
- Failure → `DnsError::truncated`.

### resolv.conf parsing

- Read once at construction; v1 has no inotify watch. Add `Resolver::reload()` method for explicit refresh.
- Honor `nameserver`, `options timeout:N attempts:N`, `options ndots:N`, `search`.
- Skip `domain`, `sortlist`, `rotate`.
- No nameservers → all queries fail with `DnsError{kind=no_servers}`.

### Hosts-file parsing

Map `lowercased_name → vector<Endpoint>` (port-less; filled at lookup). One-shot parse at construction.

### Search domain logic

Apply `search` list when host has fewer than `ndots` dots. Try each in order; first non-NXDOMAIN wins. Numeric IPs and FQDNs (trailing dot) skip search.

---

## Thread-pool (NSS) backend

```cxx
Flow<ResolveResult>
nss_thread_resolve(string_view host, u16 port, ResolveOptions opts) {
    return work_pool().submit_flow([host=string{host}, port, opts]() -> ResolveResult {
        addrinfo hints{};
        hints.ai_family = (opts.allow_v4 && opts.allow_v6) ? AF_UNSPEC :
                          opts.allow_v6 ? AF_INET6 : AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int gai = ::getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res);
        if (gai != 0) {
            throw DnsError{ map_gai(gai), gai_strerror(gai) };
        }
        // collect endpoints, freeaddrinfo, return ResolveResult
    });
}
```

Limits:
- No mid-flight cancel (POSIX limitation).
- Per-query timeout enforced via Carrier deadline, not getaddrinfo.
- Cache integration applies (consistent with native_udp).

---

## Integration with `conflux.net.client`

**Targets**: `src/net/client.cxx:162-213`.

```cxx
// new in HttpClientOptions:
Resolver* resolver{nullptr};  // null → thread-pool fallback constructed lazily

// resolve_and_connect refactored:
[[nodiscard]] std::expected<int, HttpError>
resolve_and_connect_blocking(string_view host, u16 port,
                             chrono::milliseconds timeout,
                             Resolver &resolver,
                             HttpTelemetry &tel) {
    auto t0 = chrono::steady_clock::now();
    auto rr = resolver.resolve_blocking(host, port,
        {.query_timeout = timeout, .total_timeout = timeout});
    tel.dns = chrono::steady_clock::now() - t0;
    if (!rr) {
        return std::unexpected{HttpError{
            .kind = HttpErrorKind::dns,
            .phase = HttpPhase::resolve,
            .message = rr.error().what(),
            .os_errno = rr.error().os_errno,
        }};
    }
    // Iterate rr->endpoints, ::socket + connect_with_timeout.
    // RFC 8305 §4 connection-attempt-delay applied here.
}
```

`send_streaming` (R9a) takes the async path:
```cxx
auto rr = co_await resolver.resolve(host, port, ...);
```

**Removal**: `::getaddrinfo` at `client.cxx:176` deleted.

---

## Integration with `conflux.net.smtp`

**Targets**: `src/net/smtp.cxx:37-80`. Identical pattern. `::getaddrinfo` at `:47` deleted.

For future MX support: `Resolver::resolve_mx(domain) -> Flow<vector<MxRecord>>`. Out of v1 scope.

---

## HttpError chaining

**Decision**: do not embed `optional<DnsError>` in `HttpError`. Would invert module dependency (http_types is a leaf today). Instead:

- `HttpError::kind = HttpErrorKind::dns`.
- `HttpError::phase = HttpPhase::resolve`.
- `HttpError::os_errno` carries the DNS RCODE or POSIX errno (multiplexed via sign convention or magic offset — pick at impl).
- `HttpError::message` carries the DNS error text.

Structured introspection via separate accessor `HttpClient::last_dns_error() -> optional<DnsError>` if/when needed. Out-of-band, debugging only.

---

## Caching

In-memory LRU per resolver instance:
- Key: `(lowercased_host, family_set, port)`.
- Value: `(vector<Endpoint>, expires_at)`.
- TTL clamped by `cache_max_ttl`.
- Negative cache: NXDOMAIN with `cache_negative_ttl`.
- LRU eviction at capacity.
- Concurrency: `shared_mutex`, readers fast-path.

**In-flight coalescing** is a separate table:
```cxx
struct InFlightState {
    StatePtr<ResolveResult> primary; // first caller's flow state
    std::vector<FlowSource<ResolveResult>> waiters; // subsequent attached callers
};
unordered_map<InFlightKey, shared_ptr<InFlightState>> in_flight;
```
On primary completion: fulfill all waiters under shared lock (release lock before invoking continuations).

---

## Testing strategy

### Mock nameserver
- `tests/dns_mock_server.cxx` — UDP listener on 127.0.0.1, port from `getsockname()`. Test injects via `ResolveOptions::override_nameservers`. Cases: NOERROR with N answers, NXDOMAIN, SERVFAIL, REFUSED, FORMERR, malformed, oversized → TC, slow → timeout, source-port-mismatch (drops, doesn't fail).

### Codec tests
- RFC 1035 example messages round-trip.
- Compression pointer loop detection (depth limit 16).
- Pointer to forward offset → rejected.
- RDLENGTH overrun → malformed.
- EDNS0 OPT round-trip.
- Underscored labels accepted.
- 64-byte label rejected; 63-byte accepted.
- 254-byte name rejected; 253-byte accepted.

### Resolver behavior
- Numeric IP literal short-circuit (no socket created).
- Hosts-file shortcut (no socket created).
- Cache hit on second identical lookup.
- TTL respected (sleep until expiry, second lookup misses cache).
- LRU eviction at capacity.
- NXDOMAIN cached negative-TTL.
- Truncated UDP → TCP fallback succeeds.
- TC + TCP fail → `DnsError::truncated`.
- RFC 5452 source-port mismatch → response dropped.

### Happy-eyeballs
- Both A + AAAA fired at t=0 (verify by mock server timestamps).
- AAAA returns first → result has v6 endpoint first.
- AAAA NXDOMAIN, A succeeds → v4 endpoint returned.
- Both NXDOMAIN → `DnsError::nxdomain`.

### In-flight coalescing
- Two parallel resolve calls for same name → mock server sees one query.
- Second call result has `from_coalesced=true`.

### Cancellation
- Flow cancelled mid-query → `DnsError::cancelled`, no leaked SQEs/sockets.

### Deadlock detection
- Construct Resolver with ring; install `current_resolver()` TLS; call `resolve_blocking` → throws `DnsError::no_ring` (well, a more specific kind — `cannot_block_on_owned_ring` — add to enum).

---

## Effort sizing — XL (~4.5 K LOC, ~6 weeks single-engineer)

Rebudgeted after review:
1. **DNS message codec** (RFC 1035 + EDNS0 OPT, compression pointer + safety) — **~1.2 K LOC** + tests.
2. **resolv.conf + hosts parser** — ~250 LOC.
3. **io_uring UDP submit/recv loop with timer + cancel + per-query socket pool** — ~700 LOC.
4. **Happy-eyeballs orchestration (parallel queries)** — ~150 LOC (smaller than v1 since no staggering at this layer).
5. **In-flight coalescing table** — ~100 LOC.
6. **Cache (LRU + TTL + negative)** — ~200 LOC.
7. **TCP fallback (length-prefix framing, ring-based)** — ~350 LOC.
8. **Thread-pool backend** — ~150 LOC.
9. **Mock nameserver** — ~450 LOC.
10. **Test suite** — ~1100 LOC.
11. **Integration into client.cxx + smtp.cxx + getaddrinfo removal** — ~150 LOC.

Total: **~4.8 K LOC new, ~150 removed.** ~6 work-weeks single engineer.

---

## Sequencing

- Independent of R1, R3, R5.
- Lands before R9b/R9c if true async client wanted.
- Independent of R7.
- Reuse io_uring helpers from file_io; do not duplicate ring-management.

Slot in main plan: **R11**, after R9a opens `resolve_and_connect_async`.

---

## Settled questions (from v1 review)

| Q | Resolution |
|---|---|
| Resolver lifetime / ring | Explicit constructor; mirrors FileReader. No singleton. |
| Send-from-Carrier deadlock | `resolve_blocking` checks `current_resolver()` TLS, faults if owned-ring thread. |
| UDP source port | Per-query ephemeral socket (RFC 5452). |
| Query ID space | Combined with source port + qname/qtype validation; ID alone insufficient. |
| EDNS0 | v1 required. 4096 advertised UDP size. |
| Happy-eyeballs ordering | Parallel queries; staggering moves to connect loop (R9c). |
| Flow<T> error convention | Throws-based (`DnsError`); matches `FileIoError`, `db`. |
| In-flight coalescing | v1 requirement. |
| Hostname validator | Length-only checks; allow underscores. RFC 2181 §11. |
| `override_nameservers` type | `vector<NameserverEndpoint>` with `parse_nameserver()` helper. |
| HttpError chaining | Flatten DnsError into existing fields; no module inversion. |

## Remaining open

- inotify watch on resolv.conf — v2.
- DoH/DoT — backend stub reserved; not implemented v1.
- SRV/MX — separate `resolve_mx`/`resolve_srv` API; v2.
- IDNA punycode — caller responsibility; v2 helper.
