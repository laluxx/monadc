# Test-suite enhancement changelog

## 2026-06-16

- Canonicalized reviewed reader corpora:
  - `reader-atoms.fixed.tsv` -> `reader-atoms.tsv`
  - `reader-interactions.updated.tsv` -> `reader-interactions.tsv`
- Added explicit tiers:
  - `regression` for the 1,267 previously passing tests.
  - `known-fail` for the 780 saved red tests from the baseline.
  - `future` for the 735 newly registered or aspirational corpus rows.
- Updated `tests/run.py`:
  - default tier filter is now `regression`;
  - `--all-tiers`, `--tier`, `--list-tiers` added;
  - `--validate-metadata` and `--validate-context-links` added;
  - TSV alias discovery prevents reviewed corpora from silently dropping out;
  - `--no-color` now disables ANSI output;
  - result JSON records include tier.
- Added generated indexes under `tests/docs/`:
  - `inventory.json`
  - `tier_manifest.tsv`
  - `context_links.tsv`
  - `menu_tree.md`
  - `quality_report.md`
- Removed distributable run artifacts:
  - `.test-results.json`
  - `.last-first-failure.org`
  - `.last-failures/`
  - fuzzing `last/` output and fuzz result JSON files
  - Python bytecode caches
- Fixed runner unit tests to match the current `RunnerOptions` constructor.
- Added test-to-context validation in `test_context_refs.py`.
- Made context visualizer tests skip cleanly when the visualizer script is absent from a test-only checkout.
