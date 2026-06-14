# monadc structured fuzz properties v9

This is the latest structured properties archive, continuing from v8.

## What changed in v9

- Added **159** new active `.fuzz` properties.
- Added **16** disabled future/oracle properties under `future-oracle-disabled/v9-runner-features/`.
- Preserved the recursive feature taxonomy from v8 and added new feature-specific leaves:
  - `10-control/phi-lowering-v9/`
  - `10-control/loop-mutation-v9/`
  - `20-binding/env-ssa-v9/`
  - `30-codegen-dispatch/operator-matrix-v9/`
  - `40-runtime-collections/arrays-indexing-v9/`
  - `40-runtime-collections/lists-pairs-v9/`
  - `40-runtime-collections/sets-mutation-v9/`
  - `50-quote-null-path/value-dispatch-v9/`
  - `60-cross-feature/compiler-paths-v9/`
  - `90-negative/compiler-invariants-v9/`

## Counts

- Active properties: **1269**
- New active properties in v9: **159**
- Disabled future/oracle examples added in v9: **16**
- Total disabled future/oracle examples: **23**
- Sections: **110**

## Quality direction

The v9 additions target compiler structure more directly than broad algebraic identities:

- branch PHI/diamond lowering with generated values;
- loop-carried mutable state across `while` and `for`;
- lexical environment and shadowing behavior around `with` and `set!`;
- direct operator dispatch, not only helper wrappers;
- array/list/set runtime operations with generated values;
- string/char/null/quote dispatch smoke paths;
- cross-feature paths where loop, array, set, mutation, predicates, quote, and arithmetic interact;
- expected-false invariants to catch runners that accidentally only validate truthy cases.

The `.fuzz.disabled` files document the next Python runner work: generated `String`, `Path`, `ArrInt`, `SetInt`, `ListInt`, dependent index/substr/key types, and `expect-python:` oracles.

## Runner requirement

Use recursive discovery:

```python
paths = sorted(PROPERTY_ROOT.rglob("*.fuzz"))
```

Disabled future files intentionally end in `.fuzz.disabled` and should not be loaded by the current runner.

See:

- `docs/inventory.json`
- `docs/v9-added-properties.tsv`
- `docs/v9-future-runner-properties.tsv`
