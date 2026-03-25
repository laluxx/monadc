<div align="center">
<img src="./etc/logo-no-bg.png" alt="Monad Logo" width="200"/>

# Monad

### A Pure Functional Language from High-Level Abstraction to Bare Metal

*Mathematical purity. Lisp soul. Zero-overhead hardware control.*

</div>
---

> **Editor Support:** For the best experience, use [monad-mode](https://github.com/laluxx/monad-mode) — a dedicated Emacs major mode providing syntax highlighting, REPL integration, and inline assembly support.

---

## Overview

Monad is a statically typed, purely functional language with a Lisp-style s-expression syntax and a Hindley-Milner type system. It spans the full abstraction ladder: from infinite lazy lists and refinement types down to inline assembly and direct C header inclusion — with zero-overhead interop and no runtime surprises.

Key design goals:
- **Mathematical purity** — functions are relations, collections have set-theoretic semantics, types are first-class
- **Practical performance** — SIMD-ready vectors, C-compatible arrays, naked functions, inline `asm`
- **Two syntaxes** — every construct has both a parenthesized (s-expression) and a whitespace-sensitive (Wisp) form
- **Refinement types** — types can carry logical predicates, checked at compile time

---

## Table of Contents

1. [Syntax: S-Expressions and Wisp](#1-syntax-s-expressions-and-wisp)
2. [Modules and Imports](#2-modules-and-imports)
3. [Variables and Built-in Types](#3-variables-and-built-in-types)
4. [Functions](#4-functions)
5. [Pattern Matching](#5-pattern-matching)
6. [Collections](#6-collections)
7. [Maps, Sets, and Relations](#7-maps-sets-and-relations)
8. [Refinement Types](#8-refinement-types)
9. [Type Classes and ADTs](#9-type-classes-and-adts)
10. [Layouts and Instances](#10-layouts-and-instances)
11. [Bare Metal: C FFI and Inline Assembly](#11-bare-metal-c-ffi-and-inline-assembly)
12. [Conditional Compilation](#12-conditional-compilation)

---

## 1. Syntax: S-Expressions and Wisp

Every construct in Monad can be written in two equivalent forms. The **s-expression** form uses parentheses; the **Wisp** form uses arity and indentation and drops parentheses. They are interchangeable and mixable  — pick whichever reads best for the task.

```monad
;; S-expression form
(define [var :: Int] 3)
(define [arr :: Arr :: 3 :: Int] [1 2 3])

;; Wisp form (identical semantics)
define var :: Int 3
define arr [1 2 3]
```

```monad
;; S-expression
(define (square [x :: Int] -> Int)
  (* x x))

;; Wisp
define square :: Int -> Int
  x -> x `*` x
```

Throughout this document both forms are shown where the difference is illustrative.

---

## 2. Modules and Imports

Module names must be capitalized. The file name must match the module name, except for the file containing `Main`.

```monad
(module Main)

;; Import everything from a module
(import Ascii)

;; Import everything except specific symbols
(import Math hiding [square])

;; Import only specific symbols
(import Math [sqrt log])

;; Qualified import — accessed as M.sqrt
(import qualified Math :as M)
```

---

## 3. Variables and Built-in Types

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

Types evaluate to themselves and can be stored in collections. Calling a type with it with a value **casts** it.

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

---

## 4. Functions

Functions use `->` for required parameters and `=>` for optional ones.

```monad
;; Required parameters
(define (multf [x :: Float] -> [y :: Float] -> Float)
  "Multiply X and Y."
  (* x y))

;; Wisp form
define multf :: Float -> Float -> Float
  x y -> x `*` y
```

### Optional Parameters

Every parameter after `=>` is optional and defaults to *unspecified* unless a default is given. Use the `unspecified?` predicate to check if it was provided inside the functon body.

```monad
;; y is optional; defaults to unspecified
(define (multh [x :: Hex] => [y :: Hex] -> Int)
  (if (unspecified? y) x (* x y)))

;; y defaults to 3
(define (multh => [x :: Hex] -> [y :: Hex 3] -> [z :: Hex] -> Int)
  (* x y z))
```

### Infix Calls

Any function can be called infix with backticks. This is a reader-level transformation — it expands to standard prefix syntax before compilation.

```monad
(define players    {"Alice" "Bob" "Kelly"})
(define newcomers  ["Tim" "Sue"])

;; Infix
(newcomers `into` players)
;; Expands to:
(into players newcomers)
```

### Function Metadata

Functions accept keyword options for documentation and aliases:

```monad
define pow :: Float -> Float -> Float
  :doc   "Raise BASE to the power EXP."
  :alias ^
  base exp -> if exp `=` 0.0
                1.0
                base `*` base `^` (exp `-` 1.0)
```

---

## 5. Pattern Matching

Functions can be defined by exhaustive pattern clauses. The compiler enforces coverage.

```monad
;; S-expression form
(define (last [a] -> a)
  []     -> nil
  [x]    -> x
  [_|xs] -> (last xs))

;; Wisp form
define last :: [a] -> a
  []     -> nil
  [x]    -> x
  [_|xs] -> last xs
```

---

## 6. Collections

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

### Vectors and Matrices — SIMD Ready

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
            7 8 9  16 17 18  25 26 27])
; Inferred: [Mat :: [3x3x3] :: Int]

(m 3)      ; => 2D matrix  (27 elements)
(m 3 3)    ; => 1D vector  (3 elements: 25 26 27)
(m 3 3 3)  ; => 27         (scalar)
```

---

## 7. Maps, Sets, and Relations

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
(define ages {("Alice" , 30) ("Bob" , 25) ("Charlie" , 30)})
; Inferred: [ages :: Relation]

(domain   ages)          ; => {"Alice" "Bob" "Charlie"}
(codomain ages)          ; => {25 30}
(image    ages "Alice")  ; => {30}
(preimage ages 30)       ; => {"Alice" "Charlie"}

;; Relational composition  (R ∘ S)
(ages `∘` life-stages)
```

---

## 8. Refinement Types

A refinement type attaches a logical predicate to an existing type. Violations are caught **at compile time**.

```monad
;; Simplest form: type alias / subtype
(type Code List
  :doc   "A list treated as source code."
  :alias C)

;; Predicate refinement
(type Natural { x ∈ Int | x >= 0 }
  :doc   "Non-negative integers"
  :alias ℕ)

(define (passedNatural? [x :: ℕ] -> Bool)
  (Natural? x))

(show (passedNatural? 1))   ; => true
(show (passedNatural? -1))  ; compile-time error
```

### Compound Predicates

Refinements compose: you can build new types from existing ones.

```monad
(type Even     { x ∈ Int | x % 2 = 0 }   "Even integers")
(type Positive { x ∈ Int | x > 0 }       "Positive integers")

(type PositiveEven
  { n ∈ Int | (Positive? n) `and` (Even? n) }
  "Positive even integers")
```

### Refinements over Structured Types

Predicates can reference any function, including those over strings:

```monad
(type Email
  { s ∈ String
  | (and (s `contains?` "@")
         (s `contains?` ".")
         ((count s) `>` 5)
         (not (s `contains?` " "))) })

;; Refinements can inherit from other refinements
(type CompanyEmail
  { s ∈ Email
  | (s `ends-with?` "@company.com") })

;; Return type is enforced by the compiler
(define (corporate-address -> CompanyEmail)
  "user@company.com")
```

---

## 9. Type Classes and ADTs

### Type Classes

Type classes define interfaces with optional default implementations.

```monad
(class Eq a where
  (=)  :: a -> a -> Bool
  (!=) :: a -> a -> Bool
  (x `!=` y) => (not (x `=` y)))   ; default implementation

data TrafficLight Red | Yellow | Green

(Red `=`  Red)    ; => True
(Red `!=` Green)  ; => True
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
  [Circle r]      -> π `*` r `*` r
  [Rectangle w h] -> w `*` h
  [Triangle
    [Point x1 y1]
    [Point x2 y2]
    [Point x3 y3]] ->
      * 0.5
        abs (+ (x1 `*` (y2 `-` y3))
               (x2 `*` (y3 `-` y1))
               (x3 `*` (y1 `-` y2)))
```

---

## 10. Layouts and Instances

For struct-like records with attached methods, use `layout` and `instance`. `self` refers to the receiver inside instance methods.

```monad
(layout Point
  "A 2D point in Cartesian coordinates."
  [x :: Float] "Horizontal component"
  [y :: Float] "Vertical component")

(instance Point
  (define (incX [delta :: Float])
    "Translate point rightward by delta."
    (+= self.x delta)))

(define p (Point 1.0 2.0))
(p.incX 5.0)
; p.x is now 6.0
```

---

## 11. Bare Metal: C FFI and Inline Assembly

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

### Inline Assembly

Variables defined in Lisp scope are directly accessible inside `asm` blocks by name.

```monad
(define (rdtsc -> Int)
  "Read the CPU timestamp counter."
  (asm rdtsc
       shl 32   %rdx
       or  %rdx %rax))
```

### Naked Functions

`:naked True` removes the compiler-generated prologue and epilogue, giving you full ownership of the stack and registers.

```monad
(define (addnaked [x :: Int] -> [y :: Int] -> Int)
  :doc    "Direct register addition — no compiler scaffolding."
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

- `&x` evaluates to the raw address of `x` as a `Hex` value.
- **Reference-counting GC with cycle detection** is used exclusively for dynamic structures (Lists).
- Arrays, vectors, and Layouts are stack-allocated or manually managed — no GC overhead.

---

## 12. Conditional Compilation

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

---
