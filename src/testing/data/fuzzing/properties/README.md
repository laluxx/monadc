# monadc fuzz properties manifest

This is the living fuzz-property corpus. It intentionally avoids versioned directory names and versioned report names.

## Layout

- `properties/` contains active `.fuzz` properties, grouped by semantic role.
- `disabled/` contains `.fuzz.disabled` records kept for provenance, future runner features, or merged duplicates.
- `MANIFEST.md` is this single human-readable manifest.

## Counts

- Active properties: 2032
- Disabled properties: 583
- Total records: 2615
- New clean graph properties added in this pass: 32
- Old versioned docs removed: yes

## Active quality distribution

- High signal: 633
- Medium signal: 1252
- Low signal: 147
- Active properties without atom metadata: 0

## Philosophy

The corpus is now treated as a web of language atoms. A foundational property can `proves:` an atom such as `atom.core.plus.commutative`. A derived property that uses `+` inside `if`, `with`, `do`, arrays, strings, or a full program links back with `uses-atoms:` and `depends-on:`. The goal is not only to test isolated facts, but to track which compiler paths rely on which language facts.

## Validation

The archive was validated for:

- required `.fuzz` fields
- unique active names
- no versioned docs
- no versioned semantic directories
- active properties parse with the uploaded fuzzer's `--list-properties --tiers all --inventory none`
- disabled-inclusive properties parse with `--include-disabled`

## Generated reports

Derived inventories and graph reports are build artifacts. Generate them under `build/test/reports/`; they are intentionally not stored in the source tree.
