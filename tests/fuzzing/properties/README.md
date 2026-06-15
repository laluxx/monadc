# monadc fuzz properties — signal graph v13

This archive is a signal-raising pass over the uploaded properties corpus.
It keeps the runner-compatible `.fuzz` format, but treats properties as a graph of language atoms.

## Counts

- Active `.fuzz` properties: 2000
- Disabled `.fuzz.disabled` properties: 583
- New v13 active/future properties: 627
- Exact duplicate active properties merged into disabled records: 556
- Existing properties retrofitted with atom/signal metadata fields: 7521
- Legacy fixed/no-argument stable smoke properties demoted to experimental: 172

## New graph metadata

Each property may now include ignored-by-runner metadata:

- `atom:` — the atom proved by a foundation property.
- `proves:` — semantic atom established by the property.
- `uses-atoms:` — primitive atoms used by the property.
- `depends-on:` — stronger atom laws a derived property intentionally relies on.
- `related-properties:` — concrete properties proving related atoms, e.g. `+` properties link to plus commutativity/associativity/identity.
- `signal:` — high/medium/low-medium/legacy-smoke/disabled-duplicate/future.
- `corpus-role:` — atom-foundation, derived-atom-linked, program-semantics, oracle, negative-diagnostic, stress-program, legacy-smoke, etc.

The fuzzer ignores these keys today, but the catalog docs use them to build the web.

## Important docs

- `docs/v13-atom-index.tsv` — atom to property/frequency map.
- `docs/v13-property-links.tsv` — giant web: property -> atoms -> dependencies -> related properties.
- `docs/v13-added-properties.tsv` — all new v13 properties.
- `docs/v13-merge-report.tsv` — exact duplicates moved to disabled records.
- `docs/v13-quality-report.tsv` — counts and signal distribution.
- `docs/inventory.json` — machine-readable inventory.

## Design note

This pass intentionally raises average signal instead of only increasing count:

1. It adds atom-foundation properties for core facts like `+` commutativity and identity.
2. It makes derived properties link to those atom foundations using metadata.
3. It adds program-kind, oracle, compile-fail, and stress properties using the latest runner fields.
4. It merges exact duplicates by disabling redundant copies with a `merged-into` pointer.
5. It keeps corpus coverage, but demotes no-argument fixed smoke tests from default stable to experimental.
