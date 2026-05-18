# Template compiled cache proposal

Date: 2026-05-17

## Decision

Implement a compiled-template cache now. Keep watcher support opt-in and move it
out of the core template module. Prefer eager directory load for the default
file-backed environment, with explicit reload and atomic cache swap semantics.
Keep lazy loading as an optional policy, not the default.

## Motivation

The current template engine parses file templates during `load_all()` and then
renders from cached `Template` nodes, but the parsed shape is still string-heavy:
expressions, filter chains, macro calls, loop conditions, include names, and block
metadata are interpreted from strings at render time. `render_string(...)` remains
a cold parse+render helper and should not gain an implicit text-keyed cache.

For the rest of the project, hot paths try to avoid repeated parsing, repeated
syscalls, and hidden runtime work. Templates should follow that model: compile
all reusable template syntax once, render repeatedly from immutable compiled
state, and make reload a deliberate publication event.

## Current state

- `tmpl::Environment::load_all()` eagerly reads allowed files from a directory,
  parses them, then replaces the environment cache.
- `tmpl::Environment::render(name, json_ctx)` renders only already-loaded cached
  templates and throws if `name` is missing.
- `tmpl::Environment::render_string(source, json_ctx)` parses every call.
- Watch reload is folded into `src/template.cxx` behind `CONFLUX_HAS_FILE_WATCH`;
  it reloads or erases individual cache entries as filesystem events arrive.
- Compile errors currently escape as exceptions with string messages; callers do
  not get stable file/line/column/phase metadata suitable for startup logs,
  admin UIs, CI checks, or safe hot-reload decisions.
- Includes, extends, and imports are resolved against the environment cache
  during render, so missing dependencies can survive parse/load and fail only
  when a request hits the affected template.

## Proposed shape

### Core module

`conflux.templates` should own only parsing, compilation, cache publication, and
rendering. It should not import file-watch.

Public surfaces should move toward:

```cpp
tmpl::Environment env{template_dir};
env.blocking_load_all();
auto html = env.render("index.html", json_ctx);

auto compiled = tmpl::compile_string("inline", source);
auto html2 = compiled.render(json_ctx);
```

`load_all()` may remain as a compatibility alias before release cleanup, but new
naming should use `blocking_` for caller-thread disk I/O.

### Compiled representation

Add immutable compiled value types and render from them:

- `CompiledTemplate`
- `CompiledNode`
- `CompiledExpr`
- `CompiledFilterChain`
- `CompiledMacroCall`
- `CompiledInclude` / `CompiledExtends` references by interned/resolved name

Compilation should pre-split and pre-parse:

- expression operator structure
- dotted/indexed access paths
- method call argument lists
- filter chains and filter argument expressions
- macro parameter/default expression lists
- loop target variables and iterator expression
- condition branches
- include/import/extends names

The first implementation can keep the same semantics and error model. The key
acceptance criterion is that render no longer scans/splits the same template
expression strings on every request.


### Diagnostics and reports

Compiled reload needs a non-throwing diagnostic surface, not only exceptions.
Keep throwing convenience wrappers if desired, but make the primary build/check
path return a structured report.

Suggested exported types:

```cpp
enum class TemplateDiagnosticSeverity : u8 {
    warning,
    error,
};

enum class TemplateDiagnosticPhase : u8 {
    io,
    parse,
    compile,
    link,
    render_check,
};

struct TemplateSourceLocation {
    S template_name;
    S path;
    u32 line = 0;
    u32 column = 0;
    u32 byte_offset = 0;
};

struct TemplateDiagnostic {
    TemplateDiagnosticSeverity severity = TemplateDiagnosticSeverity::error;
    TemplateDiagnosticPhase phase = TemplateDiagnosticPhase::compile;
    TemplateSourceLocation location;
    V<TemplateSourceLocation> stack; // include/import/extends/render-check stack
    S code;                          // stable short code for tests/UI
    S message;                       // human-readable detail
};

struct TemplateBuildReport {
    V<TemplateDiagnostic> diagnostics;
    SZ templates_seen = 0;
    SZ templates_compiled = 0;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] S format_text() const;
};
```

Diagnostic requirements:

- Every parse/compile/link failure should identify the logical template name.
- File-backed templates should include path plus best-effort line/column.
- Unknown include/import/extends should be a link diagnostic during build/check,
  not only a runtime render exception.
- Diagnostics should carry stable codes such as `parse.missing_end_tag`,
  `link.include_not_found`, `render_check.missing_variable`, etc., so tests and
  tooling do not have to parse prose.
- Report formatting should be a helper layered on the structured data; do not
  make callers scrape exception strings.

Preferred result shape:

```cpp
std::expected<TemplateCache, TemplateBuildReport> compile_directory(...);
std::expected<CompiledTemplate, TemplateBuildReport> compile_string_checked(...);

std::expected<void, TemplateBuildReport> Environment::blocking_load_all_checked();
std::expected<void, TemplateBuildReport> Environment::blocking_reload_all_checked();
```

The existing `blocking_load_all()` / `blocking_reload_all()` convenience APIs can
throw a `TemplateBuildError` that owns the same `TemplateBuildReport`, but the
non-throwing checked APIs should be the ergonomic default for servers that want
to return/log exact reload failures.

### Link validation

After parsing/compilation, validate the whole candidate cache before publication:

- every `extends` target exists;
- every `include` target exists;
- every `{% from ... import ... %}` target exists;
- imported macro names exist in the referenced template;
- dependency cycles are detected and reported with a stack;
- any unsupported dynamic include/import names are either rejected at compile
  time or explicitly marked as runtime-only dependencies.

This pass is cheap relative to file I/O and render, catches broken trees before
startup/reload publication, and makes hot-swap semantics meaningful.

### Render-check preflight

Add an optional validation layer that renders a candidate cache against
user-supplied sample contexts before publication. This is not a type system, but
it catches the practical failures users care about: missing variables, bad filter
arguments, bad macro calls, missing loop shapes, and dependency problems that
only surface during render.

Suggested API:

```cpp
struct TemplateRenderCheckCase {
    S template_name;
    S label;
    TmplValue context;
};

struct TemplateRenderCheckOptions {
    bool require_all_templates_covered = false;
    bool discard_output = true;
    SZ max_output_bytes = 0; // 0 = unlimited; useful for runaway templates later
};

struct TemplateRenderCheckReport {
    V<TemplateDiagnostic> diagnostics;
    SZ cases_run = 0;
    SZ templates_covered = 0;

    [[nodiscard]] bool ok() const noexcept;
};

std::expected<void, TemplateRenderCheckReport>
check_render(TemplateCache const &candidate,
             std::span<TemplateRenderCheckCase const> cases,
             TemplateRenderCheckOptions opts = {});

std::expected<void, TemplateBuildReport>
Environment::blocking_reload_all_checked(std::span<TemplateRenderCheckCase const> checks,
                                         TemplateRenderCheckOptions opts = {});
```

Render-check behavior:

- It must render against the candidate cache, never the currently published one.
- It must not publish on failure.
- It should collect all case failures where practical instead of stopping at the
  first template.
- It should include the check `label`, template name, and render stack in each
  diagnostic.
- It should support output discard so checks do not allocate the full rendered
  page when the caller only cares about safety.
- `require_all_templates_covered` should be opt-in. Many directories contain
  partials/macros that are not intended as top-level render targets.

Hot-swap with checks becomes:

```cpp
auto result = env.blocking_reload_all_checked(checks);
if (!result) {
    log(result.error().format_text());
    // old cache is still active
}
```

This provides a safe manual reload path for dependency-heavy deployments and a
clean primitive for the opt-in watcher adapter.

### Cache policy

Default file-backed policy should be eager:

1. `blocking_load_all()` reads + compiles the directory.
2. If all files compile successfully, publish the new cache in one swap.
3. Run link validation across the candidate cache.
4. Optionally run caller-provided render-check cases against the candidate cache.
5. If all validation succeeds, publish the new cache in one swap.
6. If any file/compile/link/render-check step fails, keep the old cache and
   return/throw a structured diagnostic report.

This avoids partial renders when templates depend on each other and a reload sees
only half a template tree. It also lets applications reject a syntactically valid
but semantically unsafe hot-swap before users observe it.

Optional lazy policy can be added behind `EnvironmentOptions`:

```cpp
enum class TemplateLoadPolicy {
    eager,
    lazy_on_miss,
};
```

`lazy_on_miss` should remain opt-in because the expected template set is small,
eager load catches errors at startup, and eager publication is simpler for
include/extends dependency consistency.

### Reload semantics

Manual reload should be first-class:

```cpp
env.blocking_reload_all();               // throw on failure
env.blocking_reload_all_checked();       // report diagnostics, keep old cache on failure
env.blocking_reload_all_checked(checks); // compile + link + render-check + swap
env.blocking_reload(name);               // safe only when caller knows deps
env.invalidate(name);
env.clear_cache();
```

For dependency-heavy applications, `blocking_reload_all()` is the preferred safe
path. It should compile into a temporary cache and publish only after the whole
directory is valid.

### Watch support

Watcher integration should be opt-in and separate:

- `conflux.templates`: no file-watch import, no watcher member
- `conflux.templates.watch`: adapter that wires `FileWatcher` to an environment
- `conflux::template_watch`: target/module exports the adapter, not just raw
  `conflux.file_watch`

Default watcher behavior should avoid direct per-file hot replacement. Safer
behavior:

1. coalesce file events briefly;
2. call full directory reload;
3. run optional configured render-check cases against the candidate cache;
4. atomically publish the newly compiled cache if successful;
5. keep serving the old cache if reload fails and expose/report diagnostics.

Per-file reload can still be exposed for users with known-simple template graphs,
but it should not be the default watch behavior.

### Sync and async I/O

Provide blocking and async load/reload surfaces. Rendering from an already
compiled cache remains CPU-only and should stay a normal `render(...)` call.

Suggested naming:

```cpp
void blocking_load_all();
void blocking_reload_all();
void blocking_reload(S const &name);

std::expected<void, TemplateBuildReport> blocking_load_all_checked();
std::expected<void, TemplateBuildReport> blocking_reload_all_checked();
std::expected<void, TemplateBuildReport> blocking_reload_all_checked(
    std::span<TemplateRenderCheckCase const> checks,
    TemplateRenderCheckOptions opts = {});

root::Task<void> async_load_all(FileReader &files);
root::Task<void> async_reload_all(FileReader &files);
root::Task<void> async_reload(FileReader &files, S name);

root::Task<std::expected<void, TemplateBuildReport>> async_load_all_checked(FileReader &files);
root::Task<std::expected<void, TemplateBuildReport>> async_reload_all_checked(
    FileReader &files,
    std::span<TemplateRenderCheckCase const> checks = {},
    TemplateRenderCheckOptions opts = {});
```

Do not add background work inside the environment. Users choose when to reload
and which executor/ring owns async file I/O.

### Context hot path

String JSON context overloads are convenient but still parse JSON every render.
Add overloads that accept an already parsed/value context:

```cpp
S render(S const &name, TmplValue const &ctx) const;
S render(S const &name, conflux::json::NodeRef ctx) const;
S CompiledTemplate::render(TmplValue const &ctx) const;
S CompiledTemplate::render(conflux::json::NodeRef ctx) const;
```

This lets HTTP handlers decode/request-build context once and render without
forcing another JSON parse.

## Non-goals

- No implicit cache for raw `render_string(source, ctx)`.
- No per-render filesystem `stat` checks in the default path.
- No automatic background reload thread/task hidden inside the core environment.
- No watcher dependency in the core template target.
- No mandatory render-check before publication; compile+link validation should
  always run, but sample-context render checks are caller-provided and opt-in.
- No attempt to infer a complete application context schema from templates in
  this branch.

## Benchmarks

Update `conflux_template_bench` to report at least:

- cold `render_string`: parse+compile+context parse+render
- eager directory load: file read+compile+atomic publish
- warm cached render with JSON string context
- warm cached render with prebuilt/parsed context
- reload-all success path
- reload-all compile/link-failure path keeping old cache
- reload-all render-check-failure path keeping old cache
- diagnostic report formatting cost outside hot render path

The useful target is not just lower mean time, but proving no expression/filter
splitting happens in the warm render loop.

## Tests

Add coverage for:

- eager load publishes a complete cache;
- parse errors include template name plus line/column;
- missing include/extends/import reports link diagnostics before publication;
- failed reload-all keeps old rendered output;
- render-check failures keep old rendered output and report check label/template;
- successful render-check reload publishes new output;
- manual per-file reload updates the named template;
- removed file invalidation behavior is explicit;
- include/extends/import dependency graph survives full reload;
- watcher adapter is opt-in and full-reload/coalesced by default;
- `render_string(...)` remains uncached/cold;
- parsed-context render avoids JSON string parsing.

## Branching

Recommended branch: `template/compiled-cache-reload`

This should land before a pure watcher split because the watcher should call the
new reload-all publication primitive rather than preserving current per-file
mutation semantics.
