# monadc whole compiler fuzz v7

This pack is deliberately aimed at compiler/codegen surfaces rather than generic algebra.

## Contents

- 211 auto-loaded `.fuzz` properties under `tests/fuzzing/properties/monadc_whole_compiler_fuzz_v7/properties/`
- 5 optional `expect-python:` oracle examples under `oracle-examples-not-auto-loaded/`

## Main targets

- codegen expression layer 1: literals, arrays, heap arrays, string indexing, quote/quasiquote values
- strcmp dispatch layer 2: arithmetic, exact division, modulo, bitwise, comparison, logic, predicates, casts, control, binding, collection runtime, null coalescing
- environment/call layer 3: array-as-function, set/list values passed through runtime helpers, mutated allocas read by later operations
- control-flow construction: `if` PHIs, `begin`, `do`, `when`, `unless`, `while`, numeric `for`
- lvalue/mutation: `with` + `set!`, branch-local shadows, branch mutation merging, loop-carried state
- runtime values: sets, lists, arrays, strings, quoted values, quasiquote/unquote, deep equality

## Why this pack is different

Most properties are not just arithmetic laws. They intentionally cross compiler subsystems, for example:

- `when`/`unless` guarding `conj!` into a set and checking `contains?`
- loop induction variables indexing arrays and mutating accumulators
- `quasiquote` unquote holes flowing into `set` membership checks
- mutated Bool allocas controlling string indexing
- nested `with` shadowing plus later outer reads
- array and string values passed through predicates and null-coalescing

## Compatibility

The auto-loaded properties are legacy-compatible with the existing `.fuzz` schema:

```text
name:
section:
args:
type:
expect:
description:
law:
```

They use only `Int` and `Bool` generated arguments, or no generated arguments, so they should run on the current fuzzer. The oracle examples are deliberately outside `tests/fuzzing/properties` so old runners will not load them accidentally.

## Section counts

- codegen-arith-binding: 3
- codegen-arith-div-mod: 13
- codegen-arith-phi: 3
- codegen-arith-variadic: 5
- codegen-array-env: 16
- codegen-binding-env: 15
- codegen-bitwise-dispatch: 12
- codegen-compare-equality: 2
- codegen-compare-variadic: 12
- codegen-control-flow: 8
- codegen-control-loops: 21
- codegen-cross-layer-stress: 28
- codegen-logic-coercion: 11
- codegen-negative-runtime: 6
- codegen-preds-strings-null: 30
- codegen-quote-runtime: 9
- codegen-runtime-collections: 17

## Validation performed in sandbox

- schema fields present
- unique property names
- placeholder names exactly match declared args
- auto-loaded arg types restricted to Int/Bool

I could not run the local `monad` compiler in this sandbox.
