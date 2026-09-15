# Fuzzing Tests

This directory contains deterministic fuzzing-style tests for compiler behavior.

The first harness, `fuzz_codegen.py`, is a small QuickCheck-style codegen
runner. It loads property files from `tests/fuzzing/properties`, generates
typed Int and Bool programs from type-directed builders, adds typed helper
definitions such as `Int -> Int -> Int` and `Int -> Int -> Bool`, compiles them
with `monad`, runs the resulting binary, and checks stdout against generated
algebraic laws.

The default property set covers stable codegen/type-system paths:
commutativity, associativity, arithmetic identities, inverse laws, typed
identity functions, comparison reflexivity/duality, equality reflexivity, `if`,
`do`, `with`, typed min/max helpers, Bool identity, and negative properties
that intentionally expect `False`. Bool helper functions are named as
predicates with a trailing `?`, matching the compiler rule for Bool-returning
functions. Direct Bool literal generation is intentionally still excluded from
the default generator because it exposed a separate `False` codegen gap.

Each `.fuzz` file is a real test asset. The runner reports the property
inventory first, then prints one row for each generated law with the source
property location, seed, expected/actual value, and microsecond generation
timing. It also prints per-seed compile and run timings. Every diagnostic-like
row starts with `path:line:column:` so Emacs compilation-mode can jump to the
property. Failing runs are copied into `tests/fuzzing/last` for replay.
On failure, the runner also performs a small source-level shrink pass: it first
tries to isolate a one-law counterexample, then tries to find the smallest
failing prefix of the generated law list.

The runner intentionally uses the same terminal style as `tests/run.py`:
shared-width Unicode dividers, aligned clickable locations, PASS/FAIL status,
and colorized durations. The `--no-rich` flag is accepted only for command
compatibility; Rich is not required.

Run it through Make. The Makefile is the front door for repository helper
scripts:

```sh
make fuzzing
```

Useful replay options:

```sh
python3 tests/fuzzing/fuzz_codegen.py --seed 41 --cases 1 --keep
python3 tests/fuzzing/fuzz_codegen.py --properties int_add_commutative,comparison_duality
python3 tests/fuzzing/fuzz_codegen.py --list-properties
python3 tests/fuzzing/fuzz_codegen.py --show-expr
python3 tests/fuzzing/fuzz_codegen.py --no-rich
```

Property file shape:

```text
name: int_add_commutative
section: arithmetic
args: a:Int b:Int
type: Bool
expect: True
description: Addition must be commutative for generated Int expressions.
law: (= (fuzz-add {a} {b}) (fuzz-add {b} {a}))
```

Negative property shape:

```text
name: comparison_lt_self_false
section: negative
args: a:Int
type: Bool
expect: False
description: Strict less-than of an Int expression against itself must be false.
law: (fuzz-lt? {a} {a})
```
