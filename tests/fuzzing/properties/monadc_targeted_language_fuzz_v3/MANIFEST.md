# monadc targeted language fuzz pack v3

Total properties: 141

This pack targets compiler/language feature paths described in context.tar.gz, especially codegen dispatch paths rather than only algebraic helper laws.

## Sections
- arithmetic-dispatch: 11
- bitwise: 15
- collections: 28
- comparison-variadic: 10
- control-binding: 16
- feature-interactions: 15
- logic: 11
- logic-coercion: 6
- predicates-conversions: 17
- quote-null-path: 12

## Targeted context areas
- codegen operation dispatch: arithmetic, bitwise, comparison, logic, type predicates, type conversions, control, binding, quote/quasiquote, collections, arrays, strings, null-coalescing
- runtime-sized heap arrays and array-as-function paths
- first-class Path literal values
- nested interactions: with + set!, loops + mutation, if PHI results + arrays, quote/quasiquote + predicates, set + loops

## Deliberately not included as runnable .fuzz properties
- registered-only no-strcmp functions such as concat/substr/reverse/nth/length/reduce/fn/lambda/cond/let/let*/letrec/import/export/module/match/pipe
- known broken list variadic construction `(list 1 2 3)` and list `tail` segfault path
- untyped map/set literal runtime construction gaps; this pack uses `(set ...)` dispatcher, not map literals
- inline asm naked propagation and closure construction gaps

## Installation
Extract from the repository root. With the recursive property loader, these files are discovered under `tests/fuzzing/properties/monadc_targeted_language_fuzz_v3/properties/`.

```sh
tar -xzf monadc_targeted_language_fuzz_v3.tar.gz
make fuzzing
```

If your fuzz loader still uses `PROPERTY_ROOT.glob("*.fuzz")`, change it to `PROPERTY_ROOT.rglob("*.fuzz")` first, otherwise nested packs will not run.
