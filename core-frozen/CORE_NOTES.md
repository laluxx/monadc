# Core notes

This core is intentionally split into bootstrap-safe modules and richer library
modules. The goal is to let the compiler bootstrap on a boring, deterministic
foundation while the rest of the core keeps pressure on the language design.

## Type spelling policy

- `[a]` is the abstract collection/sequence interface.
- `[Int]`, `[Float]`, and other concrete bracketed types are concrete arrays.
- `(Int)` is the one-element list/product type.
- `(Int, Bool)` and `(Int Bool)` are tuple/product spelling.
- Optional values are `Maybe a`, not `[]`/`[x]`.
- Failure with a reason is `Either e a`, not `Maybe a`.

## Canonical homes

- Pure prelude: `prelude/Class.mon`, `prelude/Numeric.mon`,
  `prelude/Function.mon`, `prelude/Coll.mon`.
- Public facade: `Prelude.mon`.
- Pure data: `Data/*`.
- Character helpers: `Data.Char.Ascii`.
- Numeric algorithms: `Numeric.Math`.
- POSIX terminal bindings: `System.Posix.Term`.

Compatibility facades remain for `Ascii`, `Math`, and `Term`, but new code should
prefer the canonical names.
