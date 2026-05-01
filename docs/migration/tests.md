# Migration Guide — tests/

Tracks callsite migration for `tests/` as part of E1.2.

## Inventory

### `tests/work_carrier_test.cxx` — `carrier::model_b` (69 occurrences) ✓ done

All test cases in the `[carrier.model_b]` tag group used the legacy
`conflux::work::carrier::model_b` namespace. Equivalent model_a coverage
already exists in `work_carrier_test.cxx` (the Model A section) for all
scenarios: `from_task`, `from_posted`, `from_operation`, `map`, `then`,
`hop_to_posted`, `hop_to_operation`, `when_all`.

## Before / After pairs

### `tests/work_carrier_test.cxx`

**Before (E1.1):**
```cpp
import conflux.work.carrier.model_a;
import conflux.work.carrier.model_b;

namespace model_a = conflux::work::carrier::model_a;
namespace model_b = conflux::work::carrier::model_b;

// ... model_a tests ...

// Model B section (16 test cases, [carrier.model_b] tag):
TEST_CASE("carrier.model_b: from_task produces TaskChain with success", "[carrier.model_b]") { ... }
TEST_CASE("carrier.model_b: map on TaskChain transforms success", "[carrier.model_b]") { ... }
// ... 14 more model_b test cases ...
```

**After (E1.2):**
```cpp
import conflux.work.carrier.model_a;

namespace model_a = conflux::work::carrier::model_a;

// model_a tests unchanged — full coverage retained
// model_b section removed (16 test cases, 250 lines)
```
