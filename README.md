<div align="center">

# Monad

<img src="./etc/logo-no-bg.png" alt="Monad Logo" width="200"/>

### A Pure Functional Language from High-Level Abstraction to Bare Metal

*Mathematical purity. Lisp soul. Zero-overhead hardware control.*

</div>

> **Editor Support:** For the best experience, use [monad-mode](https://github.com/laluxx/monad-mode) — a dedicated Emacs major mode providing syntax highlighting, REPL integration, inline assembly support, linting and much more!

-----

## 1 . Overview & Philosophy

Monad is a statically typed, purely functional language with a Lisp-style s-expression syntax and a Hindley-Milner type system. It spans the full abstraction ladder: from infinite lazy lists and refinement types down to inline assembly and direct C header inclusion — with zero-overhead interop and no runtime surprises.

**Why Monad?**
Most languages force a trade-off: you either get high-level mathematical safety (Haskell, Agda) or low-level memory and hardware control (C, Zig, Rust). Monad is built for developers who want both. It is designed for systems programming, high-assurance software, and game development where you need the composability of functional programming without sacrificing the ability to drop into low-level world with inline assembly or directly call C libraries with zero binding overhead.

Key design goals:

  - **Mathematical purity** — functions are relations, collections have set-theoretic semantics, types are first-class.
  - **Practical performance** — SIMD vectors, C-compatible arrays, naked functions, inline `asm`.
  - **Two syntaxes** — every construct has both a parenthesized (s-expression) and a whitespace-sensitive (Wisp) form.
  - **Refinement types** — types can carry logical predicates, checked at compile time.

-----

## 2 . Quick Start

**Installation**

```bash
git clone https://github.com/laluxx/monadc.git
cd monadc
sudo make install
```

That's it! You have now installed the `monad` program. 

**First program**
Create a file named `Main.monad`:

Following tradition, a simple Monad program will print "Hello, World!".

Put the following code in a file named Hello.mon, and run `monad Hello.mon`: 

```monad
show "Hello, World!"
```

The words "Hello, World!" will be printed to the console, and then the program should immediately exit. You now have a working Monad program! 

Alternatively, run the program `monad` without any arguments to enter a REPL, or read-eval-print-loop. This is a mode where Monad works like a calculator, reading some input from the user, evaluating it, and printing out the result, all in an infinite loop. This is a useful mode for exploring or prototyping in Monad. 

-----

## Table of Contents

1.  [Overview & Philosophy](https://www.google.com/search?q=%231-overview--philosophy)
2.  [Quick Start](https://www.google.com/search?q=%232-quick-start)
3.  [Bare Metal: C FFI and Inline Assembly](https://www.google.com/search?q=%233-bare-metal-c-ffi-and-inline-assembly)
4.  [Refinement Types](https://www.google.com/search?q=%234-refinement-types)
5.  [Syntax: S-Expressions and Wisp](https://www.google.com/search?q=%235-syntax-s-expressions-and-wisp)
6.  [Modules and Imports](https://www.google.com/search?q=%236-modules-and-imports)
7.  [Variables and Built-in Types](https://www.google.com/search?q=%237-variables-and-built-in-types)
8.  [Functions](https://www.google.com/search?q=%238-functions)
9.  [Pattern Matching](https://www.google.com/search?q=%239-pattern-matching)
10. [Collections](https://www.google.com/search?q=%2310-collections)
11. [Maps, Sets, and Relations](https://www.google.com/search?q=%2311-maps-sets-and-relations)
12. [Type Classes and ADTs](https://www.google.com/search?q=%2312-type-classes-and-adts)
13. [Layouts and Instances](https://www.google.com/search?q=%2313-layouts-and-instances)
14. [Conditional Compilation](https://www.google.com/search?q=%2314-conditional-compilation)
15. [Contributing & License](https://www.google.com/search?q=%2315-contributing--license)

-----

## 3 . Bare Metal: C FFI and Inline Assembly

Monad's defining feature is its ability to drop instantly into low-level hardware control without leaving the language's safety guarantees behind.

### Direct C Header Inclusion

Monad requires no binding generators. `include` makes every symbol in the header available immediately.

```monad
include <raylib.h>

InitWindow 1920 1080 "No Bindings!?"

define color Color 33 33 33 0
  "Background color"

until WindowShouldClose
  BeginDrawing
  ClearBackground color
  DrawFPS 600 600
  EndDrawing
```

## Inline Assembly

The `asm` builtin allows you to drop down into low-level machine instructions while maintaining high-level variable visibility. Here is a breakdown of a hardware-level function:

```monad
(define (rdtsc -> Int)
  "Read the CPU timestamp counter."
  (asm rdtsc
       shl 32   %rdx
       or  %rdx %rax))
```

### Anatomy of the Function

* **Definition & Type Mapping:** We define a function `rdtsc` that maps the domain of its arguments (in this case, an empty set) to the codomain `Int` type. In mathematical terms, this function represents a **morphism** from a unit type to the set of all integers.
* **The Docstring:** The optional string immediately following the signature is the documentation. This is preserved by the compiler and can be queried at runtime with `(doc rdtsc)` or used by tooling to generate documentation.
* **Lisp-Style Assembly:** The `asm` block is deeply integrated into the language. Unlike traditional assembly that uses commas and rigid line structures, Monad assembly uses **S-expression** syntax. Instructions like `shl` and `or` are written in a clean, space-separated way.
* **Direct Access:** Any variable defined in the surrounding Lisp scope is directly accessible by name inside the `asm` block. The compiler handles the mapping between Lisp symbols and CPU registers or stack offsets automatically.

---

In this specific example, the `rdtsc` instruction loads the high 32 bits of the timestamp into `%rdx` and the low 32 bits into `%rax`. We then shift and bit-or them to return a single 64-bit integer.

### Naked Functions

`:naked True` removes the compiler-generated prologue and epilogue, giving you full ownership of the stack and registers.

```monad
(define (addnaked [x :: Int] -> [y :: Int] -> Int)
  :doc "Direct register addition."
  :naked  True
  (asm
    push %rbp
    mov  %rsp   %rbp
    mov  x      %rax   ; Lisp variable x → rax
    add  y      %rax   ; Lisp variable y added
    pop  %rbp
    ret))
```

### Memory and Garbage Collection

  - `&x` evaluates to the raw address of `x` as a value of type `Hex`.
  - **Reference-counting GC with cycle detection** is used exclusively for dynamic structures (Lists).
  - Arrays, vectors, and Layouts are stack-allocated or manually managed — no GC overhead.

-----

## 4. Refinement Types

A [refinement type](https://en.wikipedia.org/wiki/Refinement_type) attaches a predicate to an existing type. Violations are solved **at compile time**, giving you formal verification without a separate proof theorem prover.

```monad
;; Simplest form: type alias / subtype
(type Code List
  :doc "A list treated as source code.")

;; Predicate refinement
type Natural { x ∈ Int | x >= 0 }
  :doc "Non-negative integers"
  :alias ℕ

(define (passedNatural? [x :: ℕ] -> Bool)
  (Natural? x))

show (passedNatural? 1)   ; => True
show (passedNatural? -1)  ; error: -1 does not satisfy refinement type ℕ
```

### Strict Predicate Naming

In Monad, any function whose codomain is the `Bool` set is strictly classified as a predicate function. The compiler statically enforces that all predicate function names must end with a question mark `?`. 

Attempting to define a boolean-returning function without the `?` suffix will fail at compile time:

```monad
(define (passedNatural [x :: ℕ] -> Bool)
  (Natural? x))
-| error: ‘passedNatural’ returns Bool, making it a predicate — predicate functions must end with '?', rename to ‘passedNatural?’
```

### Compound Predicates

Refinements compose: you can build new types *refining* existing ones.

```monad
type Even { x ∈ Int | x % 2 = 0 }
  "Even integers"

type Positive { x ∈ Int | x > 0 }
  "Positive integers"

(type PositiveEven
  { n ∈ Int
  | ((Positive? n) and (Even? n)) }
  "Positive even integers")
```

When you define a refinement type, Monad automatically generates the corresponding predicate function (like `Even?` and `Positive?`) so you can use them immediately.

The compiler also automatically generates the underlying `Set` representing that type. Because sets in Monad can be infinite, you can evaluate them directly as values. For example, running `Positive` in the REPL will print the infinite set and stream it continuously until explicitly stopped with `C-c`:

```monad
Monad λ Positive
=> {1 2 3 4 5 6 7 8 9 10 11 12...
```


### Refinements of Refinements

Predicates can be any valid Monad experssions.

```monad
(type Email
  { s ∈ String
  | (and (s contains? "@")
         (s contains? ".")
         ((count s) > 5)
         (not (s contains? " "))) })

;; Refinements can inherit from other refinements
(type CompanyEmail
  { s ∈ Email
  | (s ends-with? "@company.com") })

;; Return type is enforced by the compiler
(define (corporate-address -> CompanyEmail)
  "user@company.com")
```

If you try to return a string that violates the refinement, the compiler catches it immediately. For example, compiling this function:

```monad
(define (return-string -> CompanyEmail)
  "ciao@wrong.com")
;; error: function ‘return-string’ declares return type ‘CompanyEmail’ but its return value does not satisfy the refinement
```

This enforces absolute correctness across your codebase. Conceptually, this is extremely powerful: by defining `Email` and `CompanyEmail`, you haven't just created a type check, you have effectively materialized the infinite mathematical `Set` of all possible valid emails.

-----

## 5. Syntax: S-Expressions and Wisp

Every construct in Monad can be written in two equivalent forms. The **s-expression** form uses parentheses; the **Wisp** form uses arity and indentation and drops parentheses. They are interchangeable and mixable  — pick whichever reads best for the task.

```monad
;; S-expression form
(define [var :: Int] 3)
(define [arr :: Arr :: 3 :: Int] [1 2 3])

;; Wisp form (identical semantics)
define var :: Int 3
define arr [1 2 3]  ; Inferred by the Type system
```

```monad
;; S-expression
(define (square [x :: Int] -> Int)
  (* x x))

;; Wisp
define square :: Int -> Int
  x -> x * x
```

Throughout this document both forms are used and mixed.

-----

## 6. Modules and Imports

Module names must be capitalized. The file name must match the module name, except for the file containing `Main`.

```monad
(module Main)

;; Import everything from a module
(import Ascii)

;; Import everything except specific symbols
(import Math hiding [cos sin])

;; Import only specific symbols
(import Math [sqrt log])

;; Qualified import — accessed as M.sqrt
(import qualified Math :as M)
```

### Per Module Test Suites

Each module can include an optional test suite directly at the bottom of the file using a `(tests ...)` block. 

```monad
(tests
  ;; inc / dec
  (assert-eq (inc 0)   1  "inc 0")
  (assert-eq (dec 10)  9  "dec 10")

  ;; predicates
  (assert-eq (even? 2) 1  "even? 2")
  (assert-eq (odd? 2)  0  "odd? 2")

  ;; filter pipelines
  (assert-eq (sum (filter even? '(1..20))) 110 "sum of evens 1..20"))
```

You can run the test suite for a specific module from the command line:
```bash
monad test Module.mon
```

By default, the compiler strips all test code from the final executable to ensure zero runtime bloat. If you need the test code included in your compiled binary, append the `--test` flag to your compilation command.

-----

## 7 . Variables and Built-in Types

`define` binds a name to a value. Types are inferred by Hindley-Milner or can be stated explicitly with `::`.

```monad
(define i    20)      ; Int
(define f    40.0)    ; Float
(define c    'c')     ; Char
(define s    "hello") ; String
(define frac 1/3)     ; Ratio  — true rational arithmetic
(define h    0xFF)    ; Hex    (255)
(define b    0b1010)  ; Bin    (10)
(define o    0o755)   ; Oct    (493)

;; Explicit annotations
(define [i :: Int]   20)
(define [f :: Float] 40.0)
(define [c :: Char]  'A')
(define [h :: Hex]   0xFF)
```

### Types as First-Class Values

Types evaluate to themselves and can be stored in collections. Applying a type as function with a value **casts** it.

```monad
;; Casting
(Int   40.3)   ; => 40
(Int   "hi")   ; => 209
(Int   0xFF)   ; => 255
(Float 20)     ; => 20.0
(Float 'A')    ; => 65.0
(Char  65)     ; => 'A'
(Hex   65)     ; => 0x41
(Ratio 20)     ; => 20/1
(Arr   20)     ; => [20]
```

-----

## 8 . Functions

Functions use `->` for required parameters and `=>` for optional ones.

```monad
;; Required parameters
(define (multf [x :: Float] -> [y :: Float] -> Float)
  "Multiply X and Y."
  (* x y))

;; Wisp form
define multf :: Float -> Float -> Float
  x y -> x * y
```

### Optional Parameters

Every parameter after `=>` is optional and defaults to `*unspecified*`. Use the `unspecified?` predicate to check if it was provided inside the functon body.

```monad
;; y is optional; defaults to unspecified
(define (multh [x :: Hex] => [y :: Hex] -> Int)
  (if (unspecified? y) x (* x y)))
```

### Automatic Infix Function Calls

Monad does not require special syntax (like backticks) for infix operations. The parser automatically infers your intent from context, chaining operations left-to-right.

```monad
define players {"Alice" "Bob" "Kelly"}
define newcomers ["Tim" "Sue"]

(newcomers into players) => (into players newcomers)
(3 + 4) => (+ 3 4)
(x | xs) => (cons x xs)
```

The compiler figures out intent through a strict set of rules to prevent false positives:
- If the first element is a known callable, it is treated as a standard prefix call.
- If a higher-order function expects a function argument (`PARAM_FUNC`) at that specific slot, the symbol is treated as an argument.
- Otherwise, the compiler applies the infix rewrite: `(a f b)` becomes `(f a b)`.

### Function Metadata

Functions accept keyword options for documentation and aliases:

```monad
define pow :: Float -> Float -> Float
  :doc   "Raise BASE to the power EXP."
  :alias ^
  base exp -> if exp = 0.0
                1.0
                base * base ^ (exp - 1.0)
```

-----

## 9 . Pattern Matching

Functions can be defined by exhaustive pattern clauses. The compiler enforces complete coverage.

```monad
;; S-expression form
(define (last List -> a)
  []     -> nil
  [x]    -> x
  [_|xs] -> (last xs))

;; Wisp form
define last :: List -> a
  []     -> nil
  [x]    -> x
  [_|xs] -> last xs
```

-----

## 10 . Collections

Monad provides four distinct collection kinds, each with a different trade-off between purity, laziness, and raw performance.

### Lists — Lazy by Default

Lists `'(1 2 3)` are lazy. They progress through three automatic phases:

| Phase                  | Description                          | Memory             |
|------------------------|--------------------------------------|--------------------|
| **Pure thunks**        | Infinite, unevaluated                | O(1)               |
| **Partially realized** | Memoized prefix                      | O(forced elements) |
| **Fully eager**        | Converted to array when fully forced | O(n)               |

```monad
define naturals (0..)          ; infinite list — O(1) memory
take 5 naturals                ; => '(0 1 2 3 4), rest stays lazy
```

### Arrays — C-Compatible, Strict

Arrays `[1 2 3]` are contiguous, typed, zero-initialized by default.

```monad
define [empty :: Arr :: Int :: 3]       []               ; => [0 0 0]
define [farr  :: Arr :: Float :: 3]     [0.1 0.2 0.3]
define [harr  :: Arr :: Hex :: 3]       [0xFF 0x1 0x2]
define [aarr  :: Arr :: Arr :: Int 3]   [[1] [2] [3]]   ; array of arrays
```

### TODO Vectors and Matrices — Automatic SIMD

Vectors `#[...]` are homogeneous arrays aligned for SIMD instructions. They support boolean masking and Python-style slicing.

```monad
(define v #[10 20 30 40 50])

;; Boolean masking
(define mask (> v 25))  ; => #[#f #f #t #t #t]
(v mask)                ; => #[30 40 50]

;; Indexing
(v -1)              ; => 50  (negative indices count from end)
(v (range 1 4 2))   ; => #[20 40]  (start end step)
(v (range_ 2))      ; => #[10 20]  (from start up to index 2)
```

**Matrices** extend vectors to multiple dimensions. Each index removes one dimension:

```monad
(define m #[1 2 3  10 11 12  19 20 21
            4 5 6  13 14 15  22 23 24
            7 8 9  16 17 18  25 26 27]); Inferred: [Mat :: [3x3x3] :: Int]

(m 3)      ; => 2D matrix  (27 elements)
(m 3 3)    ; => 1D vector  (3 elements: 25 26 27)
(m 3 3 3)  ; => 27         (scalar)
```

-----

## 11 . Maps, Sets, and Relations

### Maps

```monad
(define scores #{"Fred" 1400  "Bob" 1240})

(assoc  scores "Sally" 0)   ; add / replace
(dissoc scores "Bob")       ; remove
(scores "Fred")             ; lookup          => 1400
(scores :unknown -1)        ; lookup with default => -1
(contains? scores "Fred")   ; => true
(keys   scores)             ; => ("Fred" "Bob")
(merge  scores extra)       ; combine
```

### Sets

```monad
(define s {:a :b :c :d})

(conj s :e)   ; => {:a :b :c :d :e}
(disj s :d)   ; => {:a :b :c}
(s :b)        ; => :b  (sets are functions of their members)
```

### Pairs and Relational Operations

Monad natively understands ordered `(a,b)` and unordered `{a,b}` pairs, based on the Kuratowski definition. Because Maps and Functions are mathematically relations, standard relational operators apply to all of them.

```monad
(define ages {("Alice", 30) ("Bob", 25) ("Charlie", 30)})
; Inferred: [ages :: Relation]

(domain   ages)          ; => {"Alice" "Bob" "Charlie"}
(codomain ages)          ; => {25 30}
(image    ages "Alice")  ; => {30}
(preimage ages 30)       ; => {"Alice" "Charlie"}

;; Relational composition  (R ∘ S)
(ages ∘ life-stages)
```

-----

## 12 . Type Classes and ADTs

### Type Classes

Type classes define interfaces with optional default implementations.

```monad
(class Eq a where
  (=)  :: a -> a -> Bool
  (!=) :: a -> a -> Bool
  (x != y) => (not (x = y)))   ; default implementation

data TrafficLight Red | Yellow | Green

(Red =  Red)    ; => True
(Red != Green)  ; => True
```

### Algebraic Data Types

ADTs define strict sum and product types. Pattern matching on them must be exhaustive.

```monad
data Point Float Float

data Shape
  Circle    Float
  Rectangle Float Float
  Triangle  Point Point Point

(define [π :: Float] 3.141592653589793)

define area :: Shape -> Float
  [Circle r]      -> π * r * r
  [Rectangle w h] -> w * h
  [Triangle
    [Point x1 y1]
    [Point x2 y2]
    [Point x3 y3]] ->
      * 0.5
        abs (+ (x1 * (y2 - y3))
               (x2 * (y3 - y1))
               (x3 * (y1 - y2)))
```

-----

## 13 . Layouts and Instances

For struct-like records with attached methods, use `layout` and `instance`. `self` refers to the receiver inside instance methods.

```monad
(layout Point
  [x :: Float]
  [y :: Float])

(define p (Point 1.0 2.0))

p.x => 1.0
p.y => 2.0
```

-----

## 14 . Conditional Compilation

Use `#+TAG` / `#---` blocks to include code only when a feature is present. Tags can be nested.
You can inspect which platform features are active via the `*features*` variable.

```monad
#+WINDOWS
(show "Compiled for Windows")
#---

#+LINUX
#+X86-64
(show "Compiled for Linux x86-64")
#---
```

Inspect active features at any time:

```monad
show *features*
```

-----

## 15 . Contributing & License

We welcome contributions ! Please see our [CONTRIBUTING.md](https://www.google.com/search?q=CONTRIBUTING.md) for details on how to build the compiler from source, run the test suite, and submit pull requests.

This project is licensed under the MIT License - see the [LICENSE](https://www.google.com/search?q=LICENSE) file for details.
