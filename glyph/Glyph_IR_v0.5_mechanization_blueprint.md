# Glyph IR v0.5 — Mechanization Blueprint

## First theorem-prover target

Formalize the closed connective kernel before Monad-specific operations.

```text
PortSort
Multiplicity
PortType
Port
Edge
RegionInterface
Region
Graph
```

## Core predicates

```text
WellOwned G
WellTyped G
WellScoped G
WellLinear G
WellErased G
WellBoundaries G
```

Define:

```text
WellFormed G :=
  WellOwned G ∧
  WellTyped G ∧
  WellScoped G ∧
  WellLinear G ∧
  WellErased G ∧
  WellBoundaries G
```

## First proof sequence

1. **Alpha-renaming invariance**
   Renaming injective handles preserves `WellFormed` and semantic identity.

2. **Region composition preservation**
   Connecting equal typed interfaces preserves typing and scope.

3. **Identity elimination preservation**
   Removing a pure identity node and reconnecting equal unrestricted ports preserves `WellFormed` and language observations.

4. **Linear edge uniqueness**
   In a well-formed graph, a linear output cannot reach two distinct input occurrences.

5. **Erasure non-interference**
   Removing an erased proof-only subgraph does not change runtime projection.

6. **Branch protocol preservation**
   A branch whose arms expose equal normal interfaces has a well-typed joined output.

## Delayed proofs

Do not initially mechanize:

```text
full Typed Core elaboration
all QTT grade solving
LLVM semantics
FFI
multi-shot runtime implementation
general graph rewriting
full visual renderer
```

## Extraction goal

Eventually extract or cross-check a checker that consumes the same canonical graph schema used by the C compiler. The proof assistant should not own a separate hand-maintained IR definition.
