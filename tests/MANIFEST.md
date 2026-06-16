# monadc tests directory

This directory contains the categorized compiler test corpus and runner.

## Important files

- `run.py` — main test runner with recursive menu filters, tiers, metadata validation, context-link validation, and failure artifact preservation.
- `run_core.py` — core library assertion runner.
- `reader-*.tsv` — reader atom/sugar/refinement/interaction/superset corpora.
- `codegen/` — runtime/codegen/type/error fixture corpus.
- `docs/` — generated inventory and linkage indexes.
- `TIERS.md` — tier policy and commands.

## Current counts

- Total tests: 2782
- Regression: 1267
- Known-fail: 780
- Future: 735

The runner defaults to the regression tier. Use `--all-tiers` when you want the entire specification/backlog corpus.
