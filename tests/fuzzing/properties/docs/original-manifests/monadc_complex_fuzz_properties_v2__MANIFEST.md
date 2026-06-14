# monadc complex fuzz property pack v2

Generated: 2026-06-14T15:12:57
New property files: 148

This pack is designed as a second wave after the broad arithmetic/order pack. It focuses on deeper feature interactions while staying compatible with the current `tests/fuzzing/fuzz_codegen.py` schema: only `Int` and `Bool` arguments, one-line `law:` entries, and only helpers already present in `STABLE_DEFS`.

## Emphasis

- branchwise equivalence of `if` across arithmetic and comparisons
- `do` erasure inside operands and conditions
- lexical `with` dependencies, shadowing, and branch-local scope
- min/max lattice distribution, median identities, and clamp laws
- arithmetic normalization with nested cancellation and factorization
- expected-`False` laws for impossible strict order cycles and unreachable strict self-comparisons

## Sections

- `arithmetic-function`: 2 properties
- `arithmetic-if`: 4 properties
- `arithmetic-negative`: 2 properties
- `arithmetic-normalization`: 12 properties
- `binding`: 2 properties
- `binding-do`: 3 properties
- `binding-if`: 8 properties
- `binding-if-arithmetic`: 1 properties
- `binding-order`: 4 properties
- `combo`: 8 properties
- `combo-negative`: 2 properties
- `control-do`: 11 properties
- `control-do-if`: 5 properties
- `control-do-negative`: 1 properties
- `control-if`: 15 properties
- `control-if-arithmetic`: 9 properties
- `control-if-order`: 4 properties
- `function-bool`: 1 properties
- `function-order`: 7 properties
- `function-order-binding`: 1 properties
- `function-order-do`: 1 properties
- `lattice`: 12 properties
- `lattice-arithmetic`: 2 properties
- `lattice-clamp`: 4 properties
- `lattice-order`: 3 properties
- `lattice-selection`: 6 properties
- `order-logic`: 14 properties
- `order-negative`: 4 properties

## Install

Extract at repository root:

```sh
tar -xzf monadc_complex_fuzz_properties_v2.tar.gz -C /path/to/monadc
make fuzzing
```

The archive path layout is `tests/fuzzing/properties/*.fuzz`, so extraction overlays only the new property files.
