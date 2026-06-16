# monadc test tiers

The test suite is now split into explicit tiers so the default test command gives a clean regression signal while the larger corpus still keeps every bug witness and future contract searchable.

## Tiers

- `regression`: currently passing tests. This is the default gate for `tests/run.py`.
- `known-fail`: concrete red tests from the 2026-06-15 baseline. These are valuable bug witnesses, but they should not block unrelated work.
- `future`: reviewed or newly registered tests that were not part of the previous executable baseline, plus aspirational contracts.
- `generated`: reserved for generated fuzz/minimization output. Generated last-run files are not shipped in the clean tar.

## Commands

```sh
python3 tests/run.py                  # regression tier only
python3 tests/run.py --all-tiers      # every categorized test
python3 tests/run.py --tier known-fail
python3 tests/run.py --tier future
python3 tests/run.py --list-tiers
python3 tests/run.py --validate-metadata --validate-context-links
```

## Current inventory

- Total registered tests after canonicalizing TSV corpora: 2782
- Regression gate: 1267
- Known-fail bug witnesses: 780
- Future/aspirational contracts: 735

Detailed generated indexes live in `tests/docs/`.
