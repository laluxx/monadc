# monadc deep codegen fuzz v6

This pack is intentionally compiler-path targeted rather than generic algebra.

## Contents

- 175 legacy-compatible `.fuzz` properties under `tests/fuzzing/properties/monadc_deep_codegen_fuzz_v6/properties/`
- 2 oracle examples outside the auto-loaded property tree, for future String/ArrInt oracle fuzzing

## Targeted codegen/context paths

- Layer-1 special forms: inline arrays, heap arrays, string indexing, quoted runtime values
- Layer-2 strcmp dispatch: arithmetic, modulo, bitwise, comparison, logic, predicates, casts, collections, list, string, control, binding, quote, null-coalescing
- Layer-3 env lookup: array-as-function through `with [xs [...]] (xs i)`
- Control-flow BB/PHI paths: `if`, `when`, `unless`, `while`, numeric `for`
- Mutation/lvalue paths: `with` + `set!`, branch guarded mutations, loop-carried state
- Runtime boxed values: `equal?`, `quote`, `quasiquote`, `set`, `cons`, `make-list`, `count`, `head`, `contains?`

## Deliberately avoided known gaps

- paren-mode `lambda`, `fn`, `defn`, `cond`, `let`, `let*`, `letrec`, `match`, `pipe`
- broken variadic `(list 1 2 3)`; this pack uses `(list)`, `cons`, and `make-list` instead
- `tail` on lists
- map literals and untyped map mutation paths
- naked inline asm
- superscript `^` sugar and uppercase `AND/OR/NOT` aliases

## Validation performed here

- unique property names
- required schema fields
- placeholder/arg consistency
- legacy arg types restricted to `Int`/`Bool` for auto-loaded properties

I could not run the local `monad` compiler from this sandbox.
