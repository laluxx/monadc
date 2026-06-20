# Core module map

## Bootstrap prelude

| Module | File | Role |
|---|---|---|
| `Class` | `prelude/Class.mon` | Structural typeclasses and `Ordering`. |
| `Numeric` | `prelude/Numeric.mon` | Numeric class hierarchy only. |
| `Function` | `prelude/Function.mon` | Small function combinators. |
| `Coll` | `prelude/Coll.mon` | Sequence/collection interface and basic algorithms. |
| `Prelude` | `Prelude.mon` | Public facade over the safe prelude subset. |

## Pure data

| Module | File | Role |
|---|---|---|
| `Data.Maybe` | `Data/Maybe.mon` | Optional values. |
| `Data.Either` | `Data/Either.mon` | Typed failure/success values. |
| `Data.Bool` | `Data/Bool.mon` | Boolean eliminators and helpers. |
| `Data.Tuple` | `Data/Tuple.mon` | Pair/tuple helpers. |
| `Data.List` | `Data/List.mon` | Haskell-style facade over `Coll`. |
| `Data.Set` | `Data/Set.mon` | Ordered immutable set API. |
| `Data.Ord` | `Data/Ord.mon` | Ordering helpers. |
| `Data.Json` | `Data/Json.mon` | JSON value model and parser surface. |
| `Data.Char.Ascii` | `Data/Char/Ascii.mon` | ASCII predicates and conversions. |

## Control

| Module | File | Role |
|---|---|---|
| `Control.Monad` | `Control/Monad.mon` | Monadic control combinators. |
| `Control.Arrow` | `Control/Arrow.mon` | Function/arrow-style combinators. |

## Numeric and platform

| Module | File | Role |
|---|---|---|
| `Numeric.Math` | `Numeric/Math.mon` | Constants, algorithms, `Pow`, and `Vec3`. |
| `System.Posix.Term` | `System/Posix/Term.mon` | Linux/POSIX terminal bindings. |

## Compatibility facades

| Old module | New module |
|---|---|
| `Ascii` | `Data.Char.Ascii` |
| `Math` | `Numeric.Math` |
| `Term` | `System.Posix.Term` |

The compatibility facades should stay small. New code should import the canonical
module names directly.
