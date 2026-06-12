<div align="center">

<img src="./etc/logo.png" alt="Monad Logo" width="200"/>

### A Pure Functional Language from High-Level Abstraction to Bare Metal

*Mathematical purity. Lisp soul. Zero-overhead hardware control.*

</div>

> **Editor Support:** For the best experience, use [monad-mode](https://github.com/laluxx/monad-mode) — a dedicated Emacs major mode providing syntax highlighting, REPL integration, inline assembly support, linting and much more!

-----

## 1. Repository Map

```
.
├── *.c / *.h          — Compiler source (37 translation units)
│   reader.c/h         │   Lexer, parser, AST, pattern-match desugar
│   wisp.c/h           │   Wisp syntax → S-expression expander
│   dep.c/h            │   Dependent type checker (ITT kernel, NbE, bidir)
│   infer.c/h          │   Hindley-Milner type inference
│   codegen.c/h        │   LLVM IR codegen (83+ dispatch symbols)
│   types.c/h          │   Type representation and operations
│   env.c/h            │   Environment (symbol table)
│   runtime.c/h        │   Runtime support (lists, sets, maps, GC)
│   arena.c/h          │   Arena allocator
│   asm.c/h            │   Inline assembly translator
│   ffi.c/h            │   C FFI (libclang header parsing)
│   pmatch.c/h         │   Pattern-match compilation
│   macro.c/h          │   Macro expansion
│   module.c/h         │   Module system
│   module_export.c/h  │   Module export resolution
│   cli.c/h            │   CLI argument parsing
│   repl.c/h           │   Interactive REPL
│   features.c/h       │   Conditional compilation (*features*)
│   buildsystem.c/h    │   Package build system (package.yaml)
│   typeclass.c/h      │   Type class support
│   typst_emit.c/h     │   Typst math document emission
│   main.c             │   Entry point, pipeline orchestration
│
├── core/              — Standard library (Monad source)
│   ├── prelude/       │   Coll.mon, Test.mon
│   ├── text/          │   Ascii.mon, String.mon
│   ├── List.mon       │
│   ├── Set.mon        │
│   ├── Math.mon       │
│   ├── Random.mon     │
│   ├── Assert.mon     │
│   ├── json.mon       │
│   └── TODO.mon       │
│
├── tests/             — Test suite (1048 tests)
│   ├── run.py         │   Formatted test runner
│   ├── codegen/       │   200+ compile/run .mon + .stdout pairs
│   ├── language/      │   600+ language feature tests
│   └── sugar/         │   100+ sugar desugar tests
│
├── how_to/            — How-to guides (executable .mon files)
│   ├── AlgebraicDataTypes.mon
│   ├── Layouts.mon
│   ├── Macros.mon
│   ├── RefinementType.mon
│   ├── Syntax.mon
│   ├── Test.mon
│   ├── Iter.mon
│   ├── RaylibSpeedrun.mon, GlfwSpeedrun.mon, SdlSpeedrun.mon
│   └── Term/           — Dependent type examples
│
├── context/           — Context system (LLM external memory)
│   ├── context.org    │   Root entry
│   ├── index.org      │   Schema, node index
│   ├── reader.org     │   Reader/parser context
│   ├── language.org   │   Language formalization
│   ├── build.org      │   Build/install
│   ├── tests.org      │   Test metadata, coverage
│   ├── workflow.org   │   User preferences
│   ├── rules.org      │   Repository rules
│   ├── todo.org       │   TODO items
│   ├── visualizer.org │   Context visualizer docs
│   ├── opinions.org   │   LLM critique
│   ├── philosophy.org │   Language philosophy
│   └── commit-format.org
│
├── etc/               — Assets (logo, screenshots)
├── Makefile           — Build (gcc + LLVM)
├── monad              — Compiled compiler binary
└── libmonad.a         — Static runtime library
```

-----

## 2. Philosophy

Monad is built on a conviction: the choice between high-level mathematical safety and low-level hardware control is a false dichotomy. A language should give you dependent types AND inline assembly, refinement types AND naked functions, lazy infinite lists AND C-compatible arrays — without binding generators, FFI wrappers, or runtime overhead.

See `context/philosophy.org` for the full witnessed philosophy, and `context/opinions.org` for LLM critique.

**Core principles:**

- **No trade-off between safety and control** — The dual type system (HM + ITT), the `TERM_EMBED` bridge, naked functions, and inline assembly serve a single goal: you should not have to choose between dependent types and hardware access.
- **Purity is practical** — Functions are relations, collections have set-theoretic semantics, and types are first-class. The compiler can reason about programs in ways imperative languages cannot, enabling compile-time predicate checking and zero-cost monomorphization.
- **Two syntaxes are one language** — S-expressions and Wisp are interchangeable notations for the same semantics. They mix freely within a single file.
- **Sets are the model** — Types are sets. Functions are relations. Refinement types are subsets. Universes are sets of sets.
- **Zero to metal** — Start with `show "Hello, World!"`, add type annotations, introduce polymorphism, prove invariants with dependent types, drop to layout-based structs, and write inline assembly — all within the same function.

-----

## 3. Compiler Pipeline

The compiler runs 12 sequential phases, orchestrated by `main.c:compile_one()`:

```
source.mon
    │
    ▼
┌──────────────────────────────────────────────────────────────┐
│ Phase 0: FFI Pre-pass                                        │
│   Scan includes, parse C headers (libclang), register        │
│   function arities with wisp expander                        │
├──────────────────────────────────────────────────────────────┤
│ Phase 1: Wisp Expansion + Parsing                            │
│   wisp_parse_all(source) → desugared AST list                │
│   Also: optional JSON or Typst emission at this point        │
├──────────────────────────────────────────────────────────────┤
│ Phase 2: Module/Import Declarations                          │
│   Extract (module ...) and (import ...) forms                │
│   Recursively compile dependencies via compile_one()         │
├──────────────────────────────────────────────────────────────┤
│ Phase 3: LLVM Setup + Builtins                               │
│   LLVMInitializeNativeTarget/AsmPrinter/AsmParser            │
│   register_builtins(&ctx) — 94 codegen builtins              │
│   dep_register_builtins(dep_ctx) — dependent type builtins   │
│   declare_runtime_functions(&ctx) — GC, list, set, map       │
├──────────────────────────────────────────────────────────────┤
│ Phase 4: External Declarations                               │
│   For each import, declare externals from CompiledModule     │
├──────────────────────────────────────────────────────────────┤
│ Phase 5: Init/Main Function                                  │
│   Create LLVM function (main for top, __init_<mod> for libs) │
│   Position builder at entry block                            │
├──────────────────────────────────────────────────────────────┤
│ Phase 6: *features* Global                                   │
│   Build feature keyword list at compile time                 │
│   detect_features() → feature-conditional compilation        │
├──────────────────────────────────────────────────────────────┤
│ Phase 6.5: Dependent Type Checking (Shadow Pass)             │
│   dep_toplevel() — bidir type checking with NbE              │
│   Fatal on error; does not produce code                      │
├──────────────────────────────────────────────────────────────┤
│ Phase 7: Codegen                                             │
│   For each expression: codegen_expr(&ctx, expr)              │
│   AST_LIST dispatch (83+ strcmp symbols) → LLVM IR           │
│   register_builtins entries (94 total)                       │
├──────────────────────────────────────────────────────────────┤
│ Phase 8: Function Termination                                │
│   main → return 0 (or last expression value)                 │
│   library → ret void                                         │
├──────────────────────────────────────────────────────────────┤
│ Phase 9: Registry + Name Mangling                            │
│   Build CompiledModule with exports, layouts                 │
│   mangle(mod_name, name) — LLVM symbol renaming              │
├──────────────────────────────────────────────────────────────┤
│ Phase 10: Verify + Optional Emit                             │
│   LLVMVerifyModule, then optionally emit:                    │
│   .ll (IR), .s (asm), .bc (bitcode), .o (object)            │
├──────────────────────────────────────────────────────────────┤
│ Phase 11: Object File Emit                                   │
│   emit_object() → .o (skipped if up-to-date)                │
└──────────────────────────────────────────────────────────────┘
    │
    ▼
  executable (main)  or  .o library
```

Phases are timed with `PHASE_START()`/`PHASE_END()` macros and logged to stderr. See `main.c:525-1290` for the full pipeline implementation.

-----

## 4. Source File Guide

### 4.1 Reader & Parser (`reader.c/h`, `wisp.c/h`)

| File | Purpose |
|------|---------|
| `reader.c` | Lexer, S-expression parser, AST construction (20 AST node types), pattern-match desugar, comment maps |
| `wisp.c` | Wisp syntax expander (indentation-driven → S-expressions), arity table, quote/backtick desugar |

**AST node types** (`reader.h:39-63`):

| Node | Example | Purpose |
|------|---------|---------|
| `AST_NUMBER` | `42`, `3.14` | Numeric literal |
| `AST_SYMBOL` | `x`, `+` | Identifier |
| `AST_STRING` | `"hello"` | String literal |
| `AST_PATH` | `./file`, `~/path` | Filesystem path literal |
| `AST_CHAR` | `'c'`, `#\A` | Character literal |
| `AST_LIST` | `(f x y)`, `'(1 2 3)` | S-expression list |
| `AST_KEYWORD` | `:keyword` | Keyword literal |
| `AST_RATIO` | `1/3` | Rational literal |
| `AST_ARRAY` | `[1 2 3]` | Array literal |
| `AST_SET` | `{1 2 3}` | Set literal |
| `AST_MAP` | `#{"k" v}` | Map literal |
| `AST_LAMBDA` | `(lambda ...)` | Lambda expression |
| `AST_ASM` | `(asm ...)` | Inline assembly |
| `AST_REFINEMENT` | `(type Positive ...)` | Refinement type |
| `AST_TESTS` | `(tests ...)` | Test block |
| `AST_ADDRESS_OF` | `&x` | Address-of operator |
| `AST_RANGE` | `(0..10)` | Range literal |
| `AST_LAYOUT` | `(layout Point ...)` | Struct layout |
| `AST_PMATCH` | pattern clauses | Pattern matching |
| `AST_DATA` | `(data Color ...)` | Algebraic data type |
| `AST_CLASS` | `(class Eq a ...)` | Type class declaration |
| `AST_INSTANCE` | `(instance Eq Int ...)` | Type class instance |
| `TOK_LAMBDA_LIT` | `λx. body` | Pure lambda literal |

### 4.2 Type System

The type system has two interlocking layers, implemented across 4 source files:

| File | Responsibility |
|------|---------------|
| `types.c/h` | Type representation, kind system (26 type kinds), type operations (clone, free, unify, subst, to_llvm) |
| `infer.c/h` | Hindley-Milner inference: constraint gen → unification → generalization → instantiation → zonking |
| `dep.c/h` | Dependent type checker: ITT kernel with NbE, bidir checking, metavariables, elaboration |
| `typeclass.c/h` | Type class resolution and instance derivation |

**Hindley-Milner layer** (`infer.c`) — automatic inference with polymorphic schemes:

```monad
(define (id x) x)          ; => ∀a. a -> a
(define (const x y) x)     ; => ∀a b. a -> b -> a

(id 42)                    ; Int -> Int
(id "hi")                  ; String -> String
```

Polymorphism is compiled via monomorphization — zero-cost abstraction.

**ITT kernel** (`dep.c`) — full Intensional Type Theory with:

| Concept | Implementation | Detail |
|---------|---------------|--------|
| Universe hierarchy | `Type u : Type (u+1)` | Predicative, cumulative |
| Dependent functions | `Π(x:A). B` | Bidirectional checking |
| Dependent pairs | `Σ(x:A). B` | With `fst`/`snd` projections |
| Equality | `t ≡ s : A` | `refl` + `subst` |
| Natural numbers | `Nat`, `zero`, `succ`, `Nat-elim` | Primitive recursion |
| Definitional equality | Normalization by Evaluation | Fuel: 100000 steps |
| Representation | Locally nameless | De Bruijn indices + global names |
| Elaboration | Metavariables + implicit args | `dep_conv` solver |
| Ground bridge | `TERM_EMBED` | HM ↔ ITT interop |

**Ground type hierarchy** (`types.h:8-49`):

| Category | Types |
|----------|-------|
| Primitives | `Int`, `Float`, `Char`, `String`, `Symbol`, `Bool` |
| Numeric | `Hex`, `Bin`, `Oct`, `Ratio` |
| Fixed-width | `I8`, `U8`, `I16`, `U16`, `I32`, `U32`, `I64`, `U64`, `I128`, `U128` |
| Extended | `F32`, `F80` |
| Compound | `Arr`, `List`, `Set`, `Map`, `Fn`, `Layout` |
| Pointer | `Ptr :: T` |
| Optional | `T?` (sum-type encoding) |
| Application | `Maybe Float`, `Arr :: Int :: 3` |
| Refinement | `Natural { x ∈ Int | x >= 0 }` |

**Refinement types** attach predicates to ground types as compile-time-checked subtypes, automatically generating predicate functions and materialized `Set` values.

```monad
type Natural { x ∈ Int | x >= 0 }
  :alias ℕ

(define (isNatural [x :: ℕ] -> Bool)
  (Natural? x))

show (isNatural 1)   ; => True
```

**Type classes and ADTs:**

```monad
(class Eq a where
  (=)  :: a -> a -> Bool
  (!=) :: a -> a -> Bool
  (x != y) => (not (x = y)))

data TrafficLight Red | Yellow | Green

data Shape
  Circle    Float
  Rectangle Float Float
  Triangle  Point Point Point
```

### 4.3 Codegen (`codegen.c/h`)

The codegen module translates desugared AST into LLVM IR. It is the largest file in the compiler (~13,000 lines).

**Dispatch architecture:**

- `codegen_expr()` switches on AST type, then for `AST_LIST` heads dispatches via 83+ `strcmp` calls (lines 3889-12492)
- 17 type predicates handled at `codegen.c:5458-5488`
- 94 builtins registered via `register_builtins()` at `codegen.c:12960-13119`
- 2nd registration layer: `infer_register_builtins()` adds type-annotated variants

**Callability rule:** A function is callable from paren-mode code ONLY if it has a `strcmp` handler. Functions that only appear in `register_builtins()` (like `concat`, `substring`, `reverse`, `nth`, `length`, `reduce`, `fn`/`lambda`, `cond`, `let`) are NOT directly callable — they lack the strcmp dispatch entry despite being registered in the env.

**Working dispatch categories** (all now covered by tests):

| Category | Count | Key symbols |
|----------|-------|-------------|
| Arithmetic | 10 | `+`, `-`, `*`, `/`, `%`, `mod`, `1/4` ratio-as-function |
| Bitwise | 6 | `&`, `~`, `bit-xor`, `<<`, `>>`, `>>>` |
| Comparison | 7 | `=`, `!=`, `<`, `>`, `<=`, `>=`, `equal?` |
| Logic | 3 | `and`, `or`, `not` |
| Control | 4 | `if`, `begin`, `unless`, `while` |
| Type preds | 18 | `Int?`, `Float?`, `Number?`, `String?`, `nil?`, etc. |
| Type convs | 8 | `Int`, `Float`, `Char`, `Hex`, `Oct`, `Bin`, `Arr`, etc. |
| String | 6 | `make-string`, `count`, `starts-with?`, `ends-with?`, `contains?`, indexing |
| List | 10 | `list`, `car`, `cdr`, `cons`, `.`, `++`, `empty?`, `nil?`, `pair?`, `make-list` |
| Set | 8 | `conj`, `disj`, `conj!`, `disj!`, `contains?`, `Set?`, `collection?`, set-as-function |
| Other | 10 | `quote`, `define`, `set!`, `show`, `??`, `address-of`, array-index, etc. |

### 4.4 Runtime (`runtime.c/h`, `arena.c/h`)

| File | Purpose |
|------|---------|
| `runtime.c` | Cons cells, list ops, set/map ops, GC (ref-count + cycle detection), `show` printer, keyword/value/ratio constructors |
| `arena.c` | Bump allocator for compiler-internal allocations |

**Known runtime limitations** (test suite verifies these):

- `empty?` on set literal `{}` returns `False` (should be `True`)
- `tail` on lists segfaults
- Map literal `#{"k" v}` and set literal `{1 2 3}` produce garbage values without type annotation (`:: Map` / `:: Set`)
- `assoc`, `dissoc`, `merge`, `keys`, `vals`, `find` dispatch correctly but produce garbage due to map runtime issues
- List `length` and `reverse` are registered builtins but lack strcmp dispatch — unreachable from paren-mode

### 4.5 FFI and Inline Assembly (`ffi.c/h`, `asm.c/h`)

| File | Purpose |
|------|---------|
| `ffi.c` | C header parsing via libclang, type mapping (C → Monad), arity registration |
| `asm.c` | Inline assembly IR generation, register allocation, naked function support |

**Direct C header inclusion — no bindings needed:**

```monad
include <raylib.h>

InitWindow 1920 1080 "No Bindings!?"

define color Color 33 33 33 0

until WindowShouldClose
  BeginDrawing
  ClearBackground color
  DrawFPS 600 600
  EndDrawing
```

**Inline assembly:**

```monad
(define (rdtsc -> Int)
  "Read the CPU timestamp counter."
  (asm rdtsc
       shl 32   %rdx
       or  %rdx %rax))
```

**Naked functions** (`:naked True`) remove prologue/epilogue for full control.

### 4.6 Module System (`module.c/h`, `module_export.c/h`, `buildsystem.c/h`)

| File | Purpose |
|------|---------|
| `module.c` | Module/import declaration parsing, dependency compilation, registry |
| `module_export.c` | Export resolution, visibility rules |
| `buildsystem.c` | `package.yaml` parsing, `monad new/build/run/install/clean` |

**Import variants:**

```monad
(import Ascii)                           ; everything
(import Math hiding [cos sin])           ; except
(import Math [sqrt log])                 ; only
(import qualified Math :as M)            ; qualified as M.sqrt
```

**Package format** (`package.yaml`):

```yaml
name:                my-package
version:             0.1.0.0
dependencies:
- core >= 0.1 && < 1.0
executables:
  my-package:
    main: Main.mon
    source-dirs: src
```

### 4.7 Other Compiler Modules

| File | Purpose |
|------|---------|
| `env.c/h` | Symbol table (hash map), entry kinds (VAR, FUNC, BUILTIN, LAYOUT, MACRO, MODULE) |
| `pmatch.c/h` | Pattern-match compilation: coverage checking, clause desugaring to if/case chains |
| `macro.c/h` | Macro expansion (define-macro, syntax-rules) |
| `features.c/h` | Conditional compilation via `#+TAG` / `#---` directives, `*features*` global |
| `cli.c/h` | CLI argument parsing, help text |
| `repl.c/h` | Interactive REPL with readline support |
| `typst_emit.c/h` | AST → Typst math document → PDF pipeline |
| `main.c` | Entry point, pipeline orchestration, LLVM module management |

-----

## 5. Syntax: S-Expressions and Wisp

Every construct can be written in two equivalent forms:

```monad
;; S-expression form
(define [var :: Int] 3)
(define (square [x :: Int] -> Int) (* x x))

;; Wisp form (identical semantics)
define var :: Int 3
define square :: Int -> Int
  x -> x * x
```

See `how_to/Syntax.mon` for comprehensive examples.

### Wisp Sugar Notes

- **`?`-sugar:** `(contains? var arg)` transforms to `(var contains? arg)` when `var` is an identifier (method-call sugar). Workaround: use a literal/non-identifier first arg.
- **`!`-sugar:** `(conj! var arg)` → `(var conj! arg)`. Workaround: use `(conj! {values} arg)`.

-----

## 6. Type System (Reference)

### 6.1 Hindley-Milner Inference

The HM layer (`infer.c`) provides automatic type inference with polymorphic schemes via:
1. **Constraint generation** — traverse AST, emit type equations
2. **Unification** — Robinson's algorithm
3. **Generalization** — quantify free type variables
4. **Instantiation** — fresh variables at call sites
5. **Zonking** — propagate substitution onto AST

### 6.2 Dependent Types (ITT)

The dependent type layer (`dep.c`) implements a full ITT kernel:

- **Bidirectional checking** (Löh, McBride, Swierstra 2010):
  - Inference (`Γ ⊢ t ⇒ A`): variables, annotations, applications, type constants
  - Checking (`Γ ⊢ t ⇐ A`): lambda bodies, pair introductions, `refl`
- **Definitional equality** via NbE with fuel limit (`DEP_CONV_FUEL = 100000`)
- **Locally nameless** representation — De Bruijn indices + global names
- **Metavariables** for elaboration — holes (`_`) and implicit args (`{x:A}`)
- **Ground type embedding** via `TERM_EMBED` / `dep_ground_of_value`

```monad
;; Dependent function
(define (append [xs :: List a] [ys :: List a] -> List a)
  ...)

;; Implicit arguments
(define (id {A :: Type} [x :: A] -> A) x)
```

### 6.3 Refinement Types

```monad
type Positive { x ∈ Int | x > 0 }
type Even     { x ∈ Int | x % 2 = 0 }

(type PositiveEven
  { n ∈ Int
  | ((Positive? n) and (Even? n)) })
```

Each refinement generates a predicate (`Positive?`) and a materialized `Set` (visualized as `{1 2 3 4 ...}` in the REPL).

### 6.4 Type Classes

See `typeclass.c` for the compiler implementation and `how_to/AlgebraicDataTypes.mon` for examples.

-----

## 7. Quick Start

**Installation:**

```bash
git clone https://github.com/laluxx/monadc.git
cd monadc
sudo make install
```

**First program** (`Hello.mon`):

```monad
show "Hello, World!"
```

```bash
monad Hello.mon
```

**REPL:**

```bash
monad -i
```

**Run tests:**

```bash
python3 tests/run.py
```

**Context visualizer dashboard:**

```bash
python3 context-visualizer.py
```

**Package management:**

```bash
monad new my-project
monad build
monad run
monad install
```

-----

## 8. CLI Reference

All commands and flags from `cli.h` / `cli.c`:

### Modes

| Command | Description |
|---------|-------------|
| `monad <file.mon>` | Compile source file to executable |
| `monad -i` | Start interactive REPL |
| `monad new <name>` | Scaffold a new package |
| `monad build` | Build package (requires package.yaml) |
| `monad run` | Build and run package |
| `monad clean` | Remove build/ + *.o *.ll *.s |
| `monad install` | Install to `~/.local/bin/` |
| `monad test <file.mon>` | Compile with tests, run, delete test binary |
| `monad --test <file>` | Compile with tests embedded, keep binary |

### Flags

| Flag | Effect |
|------|--------|
| `-o <file>` | Output file name |
| `--emit-ir` | Emit LLVM IR (`.ll`) |
| `--emit-bc` | Emit LLVM bitcode (`.bc`) |
| `--emit-asm` | Emit assembly (`.s`) |
| `--emit-obj` | Emit object file (`.o`) |
| `--emit-json` | Emit AST as JSON (`.json`) |
| `--emit-typst` | Emit Typst math document (`.typ`) and compile to PDF |
| `--test` | Compile with test blocks embedded |
| `-Wall` | Accepted (no-op) |
| `-Wextra` | Accepted (no-op) |
| `-h`, `--help` | Show help message |

-----

## 9. Language Tour

### Variables and Built-in Types

```monad
(define i    20)      ; Int
(define f    40.0)    ; Float
(define c    'c')     ; Char
(define s    "hello") ; String
(define frac 1/3)     ; Ratio — true rational arithmetic
(define h    0xFF)    ; Hex   (255)
(define b    0b1010)  ; Bin   (10)
(define o    0o755)   ; Oct   (493)
```

Types are first-class casting functions:

```monad
(Int   40.3)   ; => 40
(Float 20)     ; => 20.0
(Char  65)     ; => 'A'
(Hex   65)     ; => 0x41
(Arr   20)     ; => [20]
```

### Functions

```monad
;; Required parameters (→)
(define (multf [x :: Float] → [y :: Float] → Int)
  "Multiply X and Y."
  (* x y))

;; Optional parameters (⇒)
(define (multh [x :: Hex] ⇒ [y :: Hex] → Int)
  (if (unspecified? y) x (* x y)))

;; Automatic infix — no backticks needed
(newcomers into players)   ; => (into players newcomers)
(3 + 4)                    ; => (+ 3 4)

;; Function metadata
define pow :: Float → Float → Float
  :doc   "Raise BASE to the power EXP."
  :alias ^
  base exp → if exp = 0.0 1.0 else base * base ^ (exp - 1.0)
```

### Pattern Matching

```monad
(define (last List → a)
  []     → nil
  [x]    → x
  [_|xs] → (last xs))
```

Guards, wildcards, variable binding, list destructuring, constructor patterns — all compiled via `pmatch_desugar` with exhaustive coverage verification.

### Collections

| Type | Syntax | Evaluation | Memory |
|------|--------|------------|--------|
| Lists | `'(1 2 3)` | Lazy | Thunks → memoized |
| Arrays | `[1 2 3]` | Strict | C-compatible |
| Vectors | `#[...]` | SIMD-aligned | Homogeneous |

```monad
define naturals (0..)     ; infinite lazy list — O(1) memory
take 5 naturals           ; => '(0 1 2 3 4)

define [arr :: Arr :: Int :: 3] [1 2 3]

;; Boolean masking
(define v #[10 20 30 40 50])
(define mask (> v 25))    ; => #[#f #f #t #t #t]
(v mask)                  ; => #[30 40 50]
```

### Maps, Sets, and Relations

```monad
(define scores #{"Fred" 1400  "Bob" 1240})
(assoc  scores "Sally" 0)   ; add
(dissoc scores "Bob")       ; remove
(scores "Fred")             ; => 1400

;; Relational algebra
(define ages {("Alice", 30) ("Bob", 25)})
(domain   ages)          ; => {"Alice" "Bob"}
(image    ages "Alice")  ; => {30}
(ages ∘ life-stages)      ; Relational composition
```

### Layouts (Structs)

```monad
(layout Point
  [x :: Float]
  [y :: Float])

(define p (Point 1.0 2.0))
p.x  ; => 1.0
p.y  ; => 2.0
```

Layouts support pointer fields, inline nesting, packed alignment, and array-of-layout fields. See `how_to/Layouts.mon` for details.

### Conditional Compilation

```monad
#+WINDOWS
(show "Compiled for Windows")
#---

#+LINUX
#+X86-64
(show "Compiled for Linux x86-64")
#---
```

### Per-Module Test Suites

```monad
(tests
  (assert-eq (inc 0)   1  "inc 0")
  (assert-eq (sum (filter even? '(1..20))) 110 "sum of evens 1..20"))
```

```bash
monad test Module.mon
```

See `how_to/Test.mon` for guide.

-----

## 10. Context System

The context system is a **file-system-based hippocampus** — an external, persistent memory that survives across LLM instantiations. It is a corpus of Org-mode files recording every durable fact, decision, observation, inference, and TODO about the compiler, structured as a category-theoretic graph.

### 10.1 Category Theory Foundation

| Category concept | Repository artifact | Example |
|------------------|-------------------|---------|
| Object | context heading, TODO, test, source anchor | `tests.reader.path-heap-literals` |
| Morphism | typed relation between objects | `verifies`, `supports`, `evidenced by` |
| Composition | chained meaning preservation | test verifies context, context links source |
| Functor | executable test suite view from semantic context | context contract → automated check |

### 10.2 Context Visualizer

A live browser-based dashboard renders the entire graph:

```bash
python3 context-visualizer.py
```

Features: animated force graph, fuzzy search (`C-n`/`C-p`), Hoogle method browser, TODO dashboard, editor integration, morphism arrows.

### 10.3 Test Metadata

Every test fixture carries metadata linking it to context nodes:

```
;; TEST-ID: tests.codegen.rt-hm-polymorphic-identity
;; TEST-CONTEXT: monadc.context.language.type-system-surface
;; TEST-PURPOSE: polymorphic identity instantiates at Int and String
;; TEST-ATOM: (define (id x) x) works at Int and String call sites
;; TEST-EXPECT: compile, run
```

### 10.4 Context File Index

See `context/index.org` for the full schema specification.

| File | Purpose |
|------|---------|
| `context.org` | Root entry point, includes full index |
| `index.org` | Schema, format specification, node index |
| `reader.org` | Reader/parser context (AST, tokens, Wisp) |
| `language.org` | Language formalization and type system |
| `build.org` | Build/install commands and verification |
| `tests.org` | Test metadata, philosophy, context links |
| `workflow.org` | Standing user preferences and workflow |
| `rules.org` | Standing repository rules |
| `todo.org` | Project TODO notes with confidence levels |
| `visualizer.org` | Visualizer implementation docs |
| `opinions.org` | LLM critique and suggestions for context |
| `philosophy.org` | Language and project philosophy (witnessed) |
| `commit-format.org` | Commit message format conventions |

**Current corpus:** 1258 nodes, 1351 edges — 962 test nodes, all connected through verified morphisms.

-----

## 11. Test Infrastructure

| Layer | Artifact | Size | Role |
|-------|----------|------|------|
| Atoms | `tests/reader-atoms.tsv` | 772 | Parser conformance atoms |
| Sugar | `tests/reader-sugars.tsv` | 192 | Sugar desugar tests |
| Codegen | `tests/codegen/*.mon` | 200+ | Compile/run fixtures |
| Language | `tests/language/*.mon` | 600+ | Language feature tests |
| Runner | `tests/run.py` | — | Formatted execution engine |

**Current status:** 1048 tests, 980 pass, 68 fail (pre-existing architectural issues).

Run the suite:

```bash
python3 tests/run.py
```

See `context/tests.org` for coverage analysis and known failures.

**How-to guides** in `how_to/` are also executable `.mon` files that serve as both documentation and integration tests:

- `AlgebraicDataTypes.mon` — ADT definition and pattern matching
- `Layouts.mon` — Struct layout and field access
- `Macros.mon` — Macro system walkthrough
- `RefinementType.mon` — Refinement type usage
- `Syntax.mon` — Syntax comparison (S-expr vs Wisp)
- `Test.mon` — Test framework usage
- `RaylibSpeedrun.mon` — Real-world raylib application
- `Term/` — Dependent type examples

-----

## 12. Build System

```bash
make              # Debug build (default)
make release      # Release build (-O2)
make asan         # Address sanitizer build
make install      # Install to /usr/local
make uninstall    # Remove installed files
make clean        # Remove build artifacts
```

**Dependencies:** `gcc`, `llvm-dev`, `libclang-dev`, `libreadline-dev`, `libgmp-dev`, `pkg-config`.

The static runtime `libmonad.a` (archive of `runtime.o` + `arena.o`) is linked into every binary. No shared library or `ldconfig` needed.

-----

## 13. Known Limitations

These are documented architectural gaps — not bugs in the test suite:

| Area | Issue | Root Cause |
|------|-------|-----------|
| Closures | `fn`/`lambda`/`defn` not callable | No strcmp dispatch handler; no closure allocation in codegen |
| Lists | `tail` segfaults | Runtime list implementation bug |
| Sets | `empty?` returns False on `{}` | Runtime set predicate bug |
| Maps | `#{"k" v}` produces garbage without `:: Map` | Runtime map construction bug |
| Map ops | `assoc`/`dissoc`/`merge`/`keys`/`vals`/`find` | Produce garbage via map runtime |
| Dep phase | `concat`/`substring`/`reverse`/`nth`/`length`/`reduce` | Register_builtins-only — lack strcmp dispatch |
| Higher-order | `map`/`filter`/`apply`/`partial` | Require closures |
| Cond | `cond` form not recognized | Parser treats as ordinary variable |
| Let | `let`/`let*`/`letrec` not dispatched | No strcmp handler |

-----

## 14. TODO

See `context/todo.org` for the full priority-ranked list with confidence levels. Highlights:

- Full dependent type elaboration pipeline (surface → core ITT → codegen)
- Refinement type static predicate evaluation integration
- Type class instance derivation (Eq, Enum, Ord, Bounded)
- Closure support (fn/lambda/defn)
- Wisp syntax parity with s-expression forms
- Context visualizer performance optimization (60fps with 1258+ nodes)
- `THINK` record type for speculative reasoning

-----

## 15. Contributing & License

Every feature, bug fix, or test addition should be accompanied by context records explaining why it exists and what it verifies.

**Key principles:**

- **Smallest atoms** — Each test proves exactly one behavior.
- **Context-linked** — Every test links to the context nodes that explain it.
- **No fix without coverage** — Bug fixes require a test atom first.
- **Append, don't rewrite** — Use superseding records instead of silently changing history.

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
