# Proposal: Independent SIMD Direct Builds and Runtime Dispatch — Stage 1 Update

Status: open TODO / under validation.
Branch: `simd/dispatch-independence-stage1`

This is the tightened Stage 1 proposal after verification against the current tree. It keeps the original goal, narrows the first implementation step, and fixes two wording issues:

- `AUTO` resolves to direct binding; explicit `RUNTIME` is the portable-distribution mode.
- `DIRECT + STDX/STD26` is currently AVX2-specific for both generic SIMD objects and JSON SIMD objects because those objects are compiled with `-mavx2` today.

## Verdict

Implement Stage 1 now.

The current code has useful SIMD plumbing, but backend availability and binding policy are still coupled at hot call sites. `CONFLUX_USE_STDSIMD` selects whether `std::simd` / `std::experimental::simd` implementation objects are available, while `CONFLUX_SIMD_SELECTION` controls direct versus runtime binding.

Stage 1 should make that policy explicit without adding per-ISA object families, IFUNC resolvers, or a large public SIMD matrix.

## TODO Goals

- [ ] Add one normalized SIMD binding knob: `CONFLUX_SIMD_SELECTION=AUTO|DIRECT|RUNTIME`.
- [ ] Preserve `CONFLUX_USE_STDSIMD=AUTO|STD26|STDX|ON|OFF` as the backend dialect selector.
- [ ] In direct mode, remove `conflux_cpu_supports_avx2()` from stdsimd hot call sites.
- [ ] In runtime mode, preserve the current guarded behavior.
- [ ] Add object-shape tests proving direct hot objects do not retain AVX2 CPU-probe relocations.
- [ ] Keep AES-GCM and other crypto ISA policy separate from generic SIMD cleanup.

## Non-goals

- Do not add `CONFLUX_SIMD_BACKEND` yet.
- Do not add `CONFLUX_SIMD_DIRECT_ISA` yet.
- Do not add `CONFLUX_SIMD_RUNTIME_ISAS` yet.
- Do not add IFUNC/table dispatch yet.
- Do not document direct `STDX/STD26` as portable baseline while the SIMD objects compile with `-mavx2`.

## CMake surface

Add:

```cmake
CONFLUX_SIMD_SELECTION=AUTO|DIRECT|RUNTIME
```

Resolution:

```text
AUTO
  Resolve to DIRECT.

DIRECT
  Bind selected stdsimd call sites directly without runtime AVX2 CPU probes.
  With the current object flags, STDX/STD26 direct builds are AVX2-specific.

RUNTIME
  Preserve current call-site guard behavior for now. Runtime CPU-feature probes are enabled by the resolved SIMD selection even when the legacy CPU-dispatch knob is explicitly OFF; otherwise the guard would compile but return unconditional true.
```

Keep:

```cmake
CONFLUX_USE_STDSIMD=AUTO|STD26|STDX|ON|OFF
```

`CONFLUX_USE_STDSIMD` answers “which SIMD dialect is available?”  
`CONFLUX_SIMD_SELECTION` answers “how is selected SIMD bound?”

## Internal compile definitions

The normalized policy produces:

```cpp
CONFLUX_SIMD_SELECTION_DIRECT=0|1
CONFLUX_SIMD_SELECTION_RUNTIME=0|1
CONFLUX_CPU_FEATURE_PROBES_RUNTIME=0|1
```

These are internal build-shape definitions. Public API code should not branch behaviorally on them except for implementation dispatch. `CONFLUX_CPU_FEATURE_PROBES_RUNTIME` is derived from the resolved SIMD selection, so runtime mode makes the shared CPU probe functions perform real detection.

## Source changes

For each generic/stdsimd call site, split the current shape:

```cpp
#if defined(CONFLUX_STDSIMD)
if (n >= threshold && conflux_cpu_supports_avx2()) {
    return stdsimd_impl(...);
}
#endif
```

into:

```cpp
#if defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
if (n >= threshold) {
    return stdsimd_impl(...);
}
#elif defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
if (n >= threshold && conflux_cpu_supports_avx2()) {
    return stdsimd_impl(...);
}
#endif
```

Apply this to:

- `src/json_api.cxx` — JSON string scan.
- `src/json_dump.cxx` — JSON dump safe-run scan.
- `src/utils.cxx` — URL plain-run scan and ASCII lowercase.
- `src/crypto.cxx` — `constant_time_eq` only.
- `src/net/realtime.cxx` — WebSocket unmask.

## Crypto boundary

Only `constant_time_eq` participates in generic stdsimd Stage 1 cleanup.

AES-GCM remains on its existing AES-NI/PCLMUL policy because its correctness/security profile differs from generic scan/vector helpers. Any future AES policy change needs separate tests and a separate proposal.

## Verification

Required configure/build lanes:

```text
scalar:
  CONFLUX_USE_STDSIMD=OFF

direct:
  CONFLUX_USE_STDSIMD=STDX or STD26 when available
  CONFLUX_SIMD_SELECTION=DIRECT

runtime:
  CONFLUX_USE_STDSIMD=STDX or STD26 when available
  CONFLUX_SIMD_SELECTION=RUNTIME
  expected: CONFLUX_CPU_FEATURE_PROBES_RUNTIME=1 and runtime guards perform real detection
```

Required direct object-shape proof:

```text
json_api.cxx.o     no unresolved conflux_cpu_supports_avx2
json_dump.cxx.o    no unresolved conflux_cpu_supports_avx2
utils.cxx.o        no unresolved conflux_cpu_supports_avx2
crypto.cxx.o       no unresolved conflux_cpu_supports_avx2 from constant_time_eq
realtime.cxx.o     no unresolved conflux_cpu_supports_avx2
```

The shape test should inspect source/module build objects where these object files exist. Header-interface smoke builds may not expose the same object layout and should not be the only proof. CTest should drive a dedicated build target for this check so the inspected static-library objects are produced before `nm` runs; a plain script-only test can fail on clean build trees.

## Documentation rule

Until per-ISA objects exist, document:

```text
DIRECT + STDX/STD26 currently means AVX2-specific direct stdsimd objects.
Use RUNTIME for portable distribution builds that include AVX2 fast paths.
Use OFF/scalar for baseline-safe builds without those SIMD objects.
```

## Acceptance criteria

- [ ] Configure rejects invalid `CONFLUX_SIMD_SELECTION` values.
- [ ] `AUTO` resolves to direct mode.
- [ ] Direct mode compiles hot stdsimd call sites without `conflux_cpu_supports_avx2()` calls.
- [ ] Runtime mode preserves existing runtime guard behavior, including real CPU-feature detection.
- [ ] Scalar mode still builds without stdsimd objects.
- [ ] Direct object-shape CTest exists.
- [ ] Existing correctness tests pass in the required sandbox build mode.
- [ ] No speedup claim is made without representative benchmarks.
