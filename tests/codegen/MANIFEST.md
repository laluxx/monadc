# Codegen recursive test corpus

This corpus is organized as a CLI menu. Root targets are directories:

- `branches/` for positive branch behavior
- `errors/` for reader/env/codegen diagnostics
- `types/` for HM, dependent type, and dep/HM bridge coverage
- `docs/` for generated indexes

Current totals:

- `.mon` fixtures: 1756
- type-system fixtures: 200
- new type-system fixtures in this pass: 200
- fail/compile fixtures: 760
- pass/run fixtures: 978
- atom nodes: 1756
- graph edges: 13029

Useful CLI-style targets:

```sh
monad test codegen
monad test codegen errors
monad test codegen types
monad test codegen types hm
monad test codegen types dep
monad test codegen types bridge
monad test codegen branches
```

Docs:

- `docs/inventory.json`
- `docs/menu.tsv`
- `docs/cli_targets.tsv`
- `docs/menu_tree.md`
- `docs/codegen_graph.tsv`
- `docs/atom_graph.json`
- `docs/type_system_matrix.tsv`
- `docs/infer_error_matrix.tsv`
- `docs/dep_error_matrix.tsv`
- `docs/codegen_branch_matrix.tsv`
- `docs/category_manifest.tsv`
- `docs/quality_report.md`

## Tiering note

This corpus is now tiered by runner metadata:

- `regression`: fixtures that passed in the 2026-06-15 baseline.
- `known-fail`: executable bug witnesses from that baseline.
- `future`: newly registered or aspirational contracts.

Use `python3 tests/run.py codegen` for the default green codegen gate, or
`python3 tests/run.py --all-tiers codegen` for the full codegen corpus.
