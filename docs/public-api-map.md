# Public API map

New HTTP users should start with `import conflux;` under the default
`CONFLUX_API_SURFACE=curated`, or with the explicit `import conflux.http;`
façade.

For copy/allocation boundaries, borrow lifetimes, and blocking placement, see
[`cost-lifetime-model.md`](cost-lifetime-model.md). For aggregate-profile
semantics, see [`api-surface-profiles.md`](api-surface-profiles.md).

| Tier | Import / include | Use |
| --- | --- | --- |
| Stable candidate | `import conflux;` / `<conflux.hxx>` | Selected aggregate surface, controlled by `CONFLUX_API_SURFACE`. |
| Stable candidate | `import conflux.curated;` / `<conflux/curated.hxx>` | Recommended polished app/library API. |
| Stable candidate | `import conflux.http;` / `<conflux/http.hxx>` | Normal HTTP app API. |
| Stable candidate | `import conflux.json;` / `<conflux/json.hxx>` | JSON DOM/view/serde. |
| Advanced | `import conflux.extended;` / `<conflux/extended.hxx>` | Stable extension points and production customization. |
| Advanced | `import conflux.work;` / `<conflux/work.hxx>` | Task/runtime primitives. |
| Low-level public | `import conflux.complete;` / `<conflux/complete.hxx>` | Documented low-level escape hatches. |
| Low-level public | `import conflux.uring;` / `<conflux/uring.hxx>` | Raw Linux/io_uring machinery. |

Lower-level `conflux.net.*`, partition, generated, and detail imports are
experimental or internal/detail unless a focused document says otherwise.

`CONFLUX_FEATURE_SET` selects what is built. `CONFLUX_API_SURFACE` selects what
`import conflux;` re-exports from the built aggregate; it does not build more
components and does not block explicit imports of built leaf modules.
