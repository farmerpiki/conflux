# Public API map

New HTTP users should start with `import conflux.http;`.

For copy/allocation boundaries, borrow lifetimes, and blocking placement, see
[`cost-lifetime-model.md`](cost-lifetime-model.md).

| Tier | Import | Use |
| --- | --- | --- |
| Stable candidate | `import conflux;` | Curated common surface. |
| Stable candidate | `import conflux.http;` | Normal HTTP app API. |
| Stable candidate | `import conflux.json;` | JSON DOM/view/serde. |
| Advanced | `import conflux.runtime;` | Advanced runtime. |
| Advanced | `import conflux.uring;` | Low-level Linux/io_uring. |

Lower-level `conflux.net.*`, partition, generated, and detail imports are
experimental or internal/detail unless a focused document says otherwise.
