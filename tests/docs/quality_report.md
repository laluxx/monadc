# Test quality report

Generated from `tests/run.py --list` discovery after test-tier cleanup.

## Counts

- Total registered tests: **2782**
- `regression`: **1267**
- `known-fail`: **780**
- `future`: **735**

## Default gate

`tests/run.py` now runs only the `regression` tier by default. Use `--all-tiers` for the full corpus or `--tier known-fail` / `--tier future` to inspect quarantined tests.

## Context link status

- TEST-CONTEXT links: **5282**
- Unique context IDs used: **77**
- Missing context IDs: **0**

## Main cleanup performed

- Canonicalized reviewed reader corpora: `reader-atoms.tsv` and `reader-interactions.tsv`.
- Removed generated run output, last-failure directories, fuzzing last-run artifacts, and Python bytecode from the distributable tree.
- Added `TEST-TIER`, `TEST-STATUS`, and `TEST-KNOWN-FAILURE` metadata to fixture tests.
- Added tier/status columns to TSV corpora.
- Added context-link validation to both `tests/run.py` and `tests/test_context_refs.py`.

