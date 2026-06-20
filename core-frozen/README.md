# Monad core

This directory is the source-level standard library for Monad.

The core is organized like a small Haskell/Idris-style library:

- `prelude/` contains bootstrap-safe modules that may be imported by almost
  everything: `Class`, `Numeric`, `Function`, and `Coll`.
- `Prelude.mon` is the user-facing facade over the safe prelude vocabulary.
- `Data/` contains pure data modules such as `Maybe`, `Either`, `Bool`,
  `Tuple`, `List`, `Set`, `Ord`, `Json`, and `Char/Ascii`.
- `Control/` contains control abstractions built on classes and data modules.
- `Numeric/` contains heavier numeric algorithms and math/vector helpers.
- `System/` contains platform-specific bindings. Nothing in `System/` belongs
  in the automatic prelude.
- `examples/` contains probes and runnable examples that are useful for QA but
  are not core modules.

Generated files do not belong in this tree. Do not commit `.o`, `.ll`, `.bad.ll`,
or compiled test executables into the core tarball.
