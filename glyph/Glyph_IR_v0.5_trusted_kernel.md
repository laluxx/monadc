# Glyph IR v0.5 — Trusted Kernel Boundary

## Purpose

The kernel exists so every producer of Glyph may be treated as untrusted.

## Inputs

```text
sealed graph arenas
schema registry digest
observation lens
optional patch certificate
optional source correspondence table
```

## Mandatory checks

1. Every handle is in range.
2. Every object has one owner.
3. The root region has no parent.
4. Every non-root region has one structured owner.
5. Node ports match a registered schema.
6. Every edge has one output source and one or more input targets.
7. Edge endpoints agree exactly on type, sort, multiplicity, and phase.
8. Every input has one producer.
9. Linear outputs have one consumer; affine outputs have at most one.
10. Edges do not cross region boundaries.
11. Boundary imports and exports match by type and protocol.
12. Branch arm interfaces agree extensionally.
13. Effect suspension exits are covered or propagated.
14. Multi-shot resumptions have duplicable/snapshot-safe frames.
15. Erased evidence cannot enter runtime ports.
16. Every root exit closes required authority.
17. Every certificate hash and local witness is recomputed.

## Non-responsibilities

The kernel does not:

```text
choose optimizations
schedule passes
infer source types
solve QTT constraints
select a destructor
layout closures
emit LLVM
render diagrams
trust cache entries
```

Those systems produce claims. The kernel accepts or rejects the claims.

## Implementation discipline

- No callbacks into untrusted compiler phases while checking.
- No mutable global schema state after compiler initialization.
- No raw pointer identity in serialized proof objects.
- No diagnostic formatting in the semantic decision path.
- Stable error codes with separate rich diagnostic reconstruction.
- Allocation failure is a rejection, never partial acceptance.
- Optional expensive checks may be cached only under graph and schema hashes.

## Mechanization boundary

The first proof-assistant model should cover only:

```text
ports
edges
regions
multiplicity
boundary composition
erasure
one capability transition algebra
one effect suspension/handler rule
```

It should not begin by formalizing the complete Monad frontend or LLVM.
