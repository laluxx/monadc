# Glyph IR v0.5
## Graph Reality and the Trusted Kernel

**Status:** research specification with executable explicit-graph nucleus
**Date:** 2026-08-05
**Role:** proposed authoritative semantic IR for Monad
**Supersedes:** Glyph IR v0.4
**Cycle theme:** stop reconstructing the graph; make the graph the thing being trusted

---

## 0. Executive decision

Glyph v0.4 successfully exercised authority, but its executable nucleus represented a function as a nested tree of nodes and recovered dependencies from textual names. That was a valid cutover experiment; it is not an acceptable permanent semantic object.

Glyph v0.5 replaces that approximation with an explicit hierarchical open graph:

```text
G = (R, N, P, E, Σ, Π)
```

where:

```text
R   regions and typed multi-exit interfaces
N   operations
P   typed directional ports
E   typed edges and hyperedges
Σ   operation schemas and protocol declarations
Π   independently replayable proof objects
```

The governing statement is:

> **No semantic dependency may exist only because two strings happen to name the same value. Every dependency, authority transfer, effect route, and region crossing is an explicit typed connection checked by a small fail-closed kernel.**

v0.5 makes ten decisions.

1. **The graph is real.** Ports and edges are first-class semantic objects.
2. **Region boundaries are open interfaces.** Cross-region flow occurs only through declared imports and typed exits.
3. **Multi-exit semantics is restored to the executable nucleus.** Normal return, effect suspension, abort, loop transfer, and unsupported exits are typed boundary protocols.
4. **Identity is split.** Exact serialization identity, semantic alpha identity, source identity, and layout identity are distinct.
5. **The trusted computing base is deliberately small.** It checks ownership, typing, scope, linearity, erasure, protocols, and proof replay; it does not optimize.
6. **Transforms are graph patches.** A transform submits a target graph and a local certificate; the checker reconstructs the expected rewrite.
7. **Incrementality follows region Merkle boundaries.** Unchanged sibling regions keep their semantic hashes.
8. **Visual text is a projection of topology.** The renderer consumes explicit edges rather than inventing apparent wires from traversal order.
9. **The existing QTT subsystem receives an exact importer destination.** Its evidence becomes graph structure, facets, or temporary differential oracles—not a parallel authority.
10. **No new semantic feature family enters the cycle.** v0.5 strengthens representation and trust rather than broadening the language.

This version still does not integrate into the production Monad compiler, emit LLVM, or provide a machine-checked soundness proof. It does establish the object and checker that those later claims must use.

---

## 1. The correction to v0.4

v0.4's reference object was:

```text
Node {
    kind
    attributes
    children
}
```

The reference verifier inferred relations from names such as `κrequest`, `%valid`, and `ℓwrite`. That had four weaknesses.

### 1.1 Accidental identity

A typo or duplicate label could change or alias meaning before the verifier saw an explicit relation.

### 1.2 Hidden region capture

A nested region could appear to use an outer value without an explicit free-variable or authority interface.

### 1.3 Weak support for cycles and non-tree dependence

Loops, recursive dataflow, shared unrestricted values, state rails, and effect portals are graph relations. A tree can contain their syntax, but not represent their connectivity without side conventions.

### 1.4 Unclear proof invalidation

If an operation changes, a tree hash can show that an ancestor changed, but cannot precisely identify which boundary, edge, or port obligation was invalidated.

v0.5 corrects all four by making connection identity explicit.

---

## 2. Formal object

A Glyph function is a typed hierarchical open port-hypergraph:

```text
F = ⟨name, root, Aresult, Ω, R, N, P, E, Σ, Π⟩
```

where `Ω` is an observation lens.

### 2.1 Ports

A port is:

```text
p = ⟨owner, polarity, sort, payload, multiplicity, phase⟩
```

The port sorts are:

```text
val A       runtime or compile-time value of type A
state D     version of state domain D
cap C@s     authority for capability C in protocol state s
loan L      lexical loan witness
fx ε        effect signal or effect row fragment
resume q A  resumption with quantitative grade q
proof π     erased certificate or equality witness
```

Polarity is relative to the owner:

```text
in    the owner consumes the connection
out   the owner produces the connection
```

Multiplicity is:

```text
unrestricted   zero or more consumers
affine         zero or one consumer
linear         exactly one consumer
```

### 2.2 Edges

An edge is:

```text
e = ⟨region, source, targets, role⟩
```

The source must be an `out` port. Every target must be an `in` port. Source and targets must agree exactly on sort, payload, multiplicity, and erased/runtime phase.

A value edge may be a hyperedge when its multiplicity allows fanout. Capability, loan, effect, and one-shot resumption edges are normally linear.

### 2.3 Operations

An operation is:

```text
n = ⟨schema, region, inputs, outputs, attributes, child-bindings⟩
```

An operation schema declares:

```text
port names and types
permitted child-region roles
purity and totality class
protocol transition law
observation events
proof obligations
```

An operation cannot create an undeclared port, hidden region, implicit authority transition, or unregistered observation.

### 2.4 Regions

A region is:

```text
r = ⟨kind, parent, imports, exits, nodes, edges⟩
```

A region input port is an internal producer made available by its boundary. A region exit port is an internal consumer carrying a result to the owner.

A region has one or more named exits:

```text
normal
return
abort
suspend:ε
resume:q
break:L
continue:L
unsupported:reason
```

Each exit contains typed ports and a protocol state summary.

### 2.5 Child bindings

A structured operation owns child regions through an explicit boundary binding:

```text
bind child r as role
  import parent.input  -> child.input
  export child.exit.p  -> parent.output
```

There is no cross-region edge. A connection crossing a region boundary must be represented by a binding or a declared dynamic portal such as an effect clause ingress.

---

## 3. Kernel judgment

The trusted checker validates:

```text
Σ ; Ω ⊢ G ✓
```

by replaying smaller judgments.

### 3.1 Structural ownership

```text
Σ ⊢ owners(G)
```

Every node belongs to exactly one region. Every non-root region belongs to exactly one structured node. Every port belongs to exactly one node or region. Every edge belongs to exactly one region.

### 3.2 Port typing

```text
Σ ⊢ source(e) : T
Σ ⊢ target_i(e) : T
────────────────────────
Σ ⊢ e : T
```

There are no coercions hidden in edges. Representation or protocol conversions are operations with schemas.

### 3.3 Multiplicity

For an output port `p`:

```text
mult(p) = linear        ⇒ consumers(p) = 1
mult(p) = affine        ⇒ consumers(p) ≤ 1
mult(p) = unrestricted  ⇒ consumers(p) ≥ 0
```

Every input port has exactly one producer.

### 3.4 Region scope

```text
edge e ∈ region r
────────────────────────────────────
owner-region(source(e)) = r
owner-region(target(e)) = r
```

Outer values enter through region inputs. Results and authority leave through named exits. Higher-order free variables are explicit boundary imports, not dominance accidents.

### 3.5 Branch reconciliation

For branch arms `r1 … rn`:

```text
interface(r1) ≡ ... ≡ interface(rn)
protocol-normal(r1) ≡ ... ≡ protocol-normal(rn)
```

Arm-local authority must be closed or exported explicitly. The branch may implement different cleanup paths, but the surviving boundary protocol must agree extensionally.

### 3.6 Effect routing

A closure or computation region may expose:

```text
suspend:Console.write (!signal : Console.write)
```

A handling node must declare coverage and own a clause region whose ingress and exit protocol matches that effect.

For multi-shot grade `ω`, the frame policy must prove duplicability through `clone` or `snapshot`. `move`, `borrow`, and `forbid` are insufficient.

### 3.7 Erasure

An erased proof output may connect only to erased proof inputs. Any attempt to feed erased evidence into a runtime port is rejected.

---

## 4. Closed trusted kernel, extensible schemas

The v0.5 trusted kernel understands only universal graph laws:

```text
ID ownership
port polarity
type equality
multiplicity
region containment
boundary binding
exit protocol equality
erasure separation
schema lookup
certificate replay
```

It does not know the implementation of `String.length`, a layout constructor, an FFI call, or an optimization heuristic.

Operation families remain extensible through declarative schemas, but a schema cannot redefine:

```text
what an edge means
what linearity means
how regions are owned
how effects cross boundaries
how a capability is consumed
how proof identity is checked
```

This is the core safety boundary: extensible vocabulary over a closed connective calculus.

---

## 5. Four identities

v0.5 makes identity explicit because one hash cannot serve all compiler purposes.

### 5.1 Exact identity

```text
H_exact(G)
```

Includes serialized IDs, display labels, source metadata, and order. Used for exact artifact integrity.

### 5.2 Semantic alpha identity

```text
H_sem(G)
```

Excludes transient IDs and display metadata. Region, node, port, and edge IDs are alpha-normalized by deterministic hierarchical traversal.

The reference nucleus proves:

```text
H_sem(rename(G)) = H_sem(G)
H_exact(rename(G)) ≠ H_exact(G)
```

This is alpha stability, not a claim of solving unrestricted graph isomorphism.

### 5.3 Source identity

```text
H_source(node)
```

Binds a graph object to a Typed Core node, module identity, and source range. It supports diagnostics and importer correspondence.

### 5.4 Layout identity

```text
H_layout(view)
```

Captures terminal width, portal placement, and visual expansion. It is never semantic authority.

---

## 6. Region Merkle identity and incremental compilation

Every region receives a hash over:

```text
boundary signature
operation schemas and semantic attributes
local port topology
local edges
ordered child-region hashes
```

A local patch invalidates:

```text
the changed region
its ancestor chain
consumers of changed published interfaces
```

Unchanged siblings retain their hashes.

The executable v0.5 specimen removes a pure identity node in the root region. The root hash changes while the `yes`, `no`, closure body, and handler clause hashes remain stable.

This provides a principled cache key for:

```text
verification results
lowering projections
rendered views
analysis facets
backend plans
oracle comparison
```

---

## 7. Proof-carrying graph patches

An optimizer is not trusted to preserve Glyph.

A transform proposes:

```text
Patch {
    rule
    source-semantic-hash
    target-semantic-hash
    observation-lens
    changed-regions
    source-boundary-hashes
    target-boundary-hashes
    witness
}
```

The checker:

1. verifies source and target graphs independently;
2. checks source and target hashes;
3. verifies that the rule is known;
4. reconstructs the expected local target from the witness;
5. checks boundary protocols;
6. checks that the requested observation lens is permitted;
7. accepts only if the reconstructed target has the target semantic hash.

v0.5 implements one complete rule:

```text
pure.identity-elimination
```

The important result is not the optimization. It is the architecture: the optimizer supplies an untrusted proposal; the kernel replays a small rule.

---

## 8. Visual language

The visual language remains fundamental, but it is now downstream of explicit connectivity.

A port is rendered using a stable alphabet:

```text
%   value
κ   capability
ℓ   loan
!   effect
↻   resumption
σ   state-domain version
π   proof
```

Connection geometry remains semantic:

```text
──▶   directed dependency or transfer
◀──   input supplied by a producer
╭─    region or operation opens
╰─    region, operation, or exit closes
╳     consumed, evacuated, or revoked
┊     erased compile-time path
```

The v0.5 reference view is intentionally verbose. It shows each input's explicit producer and each output's explicit destinations. Future compact renderers may fold trivial ports or align long wires, but they must preserve the same graph.

A fragment looks like:

```text
╭─ cap.move-field  evacuate payload
│  ├─ κsource : Request{payload:owned,meta:owned} [linear]
│  │     ◀── @yes.resource
│  ├─ κfield : String{root:owned} [linear]
│  │     ──▶ @closure.make.capture
│  ╰─ κremainder : Request{payload:gone,meta:owned} [linear]
│        ──▶ @cap.drop.resource
╰─ end cap.move-field
```

The shape is no longer guessed from sequence. It is rendered from ports and edges.

---

## 9. Hostile specimen

The v0.5 specimen preserves the v0.4 cutover workload and expresses it as a real graph:

```text
Request value
Request capability
validation
pure identity patch target
branch
partial payload evacuation
remainder destruction
moved closure capture
closure body with explicit latent effect exit
abortive handler clause
closure-environment destruction
branch result join
root normal exit
```

The specimen contains five regions:

```text
function root
yes arm
no arm
closure body
handler clause
```

and seven port sorts are available to the kernel, although this specimen exercises values, capabilities, and effects.

---

## 10. Actual QTT convergence map

The current `qtt.tar(3).gz` subsystem was inspected directly for this cycle.

### 10.1 `QttSemanticFunction`

Current fields include:

```text
nodes
grade_evidence
types
effects
usage
resources
destructors
transitions
edges
calls
closure_fields
effect_judgment
```

The v0.5 destination is:

```text
QttSemanticFunction
    -> GlyphFunction root
       + imported operation nodes
       + imported typed ports
       + imported capability/effect edges
       + proof facets
       + source correspondence table
```

### 10.2 `QttSemanticNode`

Fields such as kind, source, type identity, effects, representation, capability, closure identity, and callable domains become:

```text
operation schema
source identity
value ports
capability ports
effect exits
closure child-region ownership
certified facets
```

The flat `subtree_size` becomes importer traversal evidence and is not retained as semantic authority.

### 10.3 `QttSemanticGradeEvidence`

Becomes a proof facet or erased proof port attached to the binder/call/closure boundary it justifies. It does not become an executable edge unless a graded runtime object is actually present.

### 10.4 `QttSemanticCapabilityTransition`

Becomes explicit capability input/output ports and edges on move, drop, borrow, write, replace, and rehome operations.

The fields `before`, `after`, `target_after`, place, and control path become protocol payloads and region position, not a detached event log.

### 10.5 `QttSemanticCapabilityEdge`

This historical branch projection is deleted after migration. Branch interface equality and per-region Merkle identity reconstruct its purpose from real arm boundaries.

### 10.6 `QttSemanticCallEvidence`

Becomes the call operation schema instance:

```text
argument value ports
argument capability/loan ports
result ports
callable contract hash
source correspondence
```

### 10.7 `QttSemanticClosureFieldEvidence`

Becomes closure-region imports and closure-environment capability ports. Storage, exit policy, representation, and destructor descriptor remain certified physical facets.

### 10.8 `QttSemanticDestructorEvidence` and drop masks

Become cleanup operation schemas and path-sensitive capability-tree transitions. A drop mask is retained as a replayable proof facet, not a backend hint.

### 10.9 `QttSemanticEffectJudgment`

Becomes the root effect protocol certificate and published interface hash. Its authority, row, and constraint fingerprints remain portable proof identity.

### 10.10 `QttResourceBlock`

Remains an independent rejection oracle during migration. It is eventually generated as a diagnostic projection from Glyph or deleted from the production authority path.

### 10.11 `QttAnfProgram`

Becomes the differential oracle for Execution Glyph until all backend consumers use certified Glyph lowering.

---

## 11. Production representation

The production C representation should use dense arenas:

```text
GlyphGraph
  regions[]
  nodes[]
  ports[]
  edges[]
  edge_targets[]
  boundary_bindings[]
  exits[]
  attributes[]
  proof_objects[]
```

Required properties:

```text
stable 32- or 64-bit handles
no semantic raw pointers across serialization boundaries
immutable sealed graphs
builder phase separated from verified phase
interned type and schema identities
region-local adjacency indexes
source identity side table
content hashes cached after sealing
```

The bundle contains a strict C11 kernel sketch that checks the universal graph layer independently of the Python reference.

---

## 12. Trusted computing base

The intended trusted base is:

```text
canonical decoder
schema registry authenticity checker
graph kernel verifier
boundary protocol verifier
certificate replay engine
cryptographic hash implementation
```

Not trusted:

```text
frontend importer
optimizer
visual renderer
Execution Glyph lowering
LLVM emitter
legacy QTT oracle
cache
incremental scheduler
```

These components may be buggy; they must fail by producing an artifact the kernel rejects or a certificate it cannot replay.

---

## 13. Research positioning

Glyph v0.5 draws a strict boundary around ideas seen in current IR systems and research:

- HUGR demonstrates a practical hierarchical graph IR with explicit ports, strict typing, linear resources, and staged lowering.
- RVSDG demonstrates that regionalized dependence graphs can serve as competitive whole-program compiler representations.
- recent higher-order SSA work replaces CFG dominance with nesting and free-variable dependency structure.
- MLIR graph regions explicitly model values as multi-edges and admit cycles, while its operation/region framework demonstrates scalable schema-driven extensibility.
- Lean's compiler includes an explicit IR checker, reinforcing the practical value of checking IR invariants rather than trusting every producer.
- recent translation-validation and certificate-carrying work supports small independent checkers for untrusted transformations.
- recent IR fuzzing work supports schema-derived generation and mutation as a serious validation method.

Glyph's distinctive commitment is to make QTT grades, capabilities, loans, effects, resumptions, and proof erasure edge sorts and boundary protocols in one visual semantic graph.

---

## 14. Explicit non-goals

v0.5 does not add:

```text
new ownership modes
concurrency semantics
memory consistency models
general equality saturation
new handler varieties
GPU/vector dialects
arbitrary graph-isomorphism canonicalization
full binary wire format
production parser recovery
mechanized soundness proof
LLVM translation validation
```

The version is successful only if the graph and checker become more trustworthy without the semantic surface growing.

---

## 15. Executable validation

The Python reference validates:

```text
explicit graph ownership
declarative operation schemas
typed edges
linearity and affinity
region boundary protocols
branch interface equality
effect and continuation contracts
root result protocol
canonical graph serialization
alpha-stable semantic hashing
deterministic visual projection
local proof-carrying patch replay
region-local incremental invalidation
forged certificate rejection
10 semantic graph mutations
```

The strict C11 kernel validates:

```text
range safety
root ownership
port ownership
edge direction
type equality
region containment
input producer uniqueness
linear/affine fanout
erasure separation
```

---

## 16. What v0.5 honestly proves

v0.5 demonstrates that:

1. the executable Glyph object can be an actual hierarchical port graph rather than a named tree;
2. alpha-renaming can be separated from exact artifact identity;
3. a small checker can reject hidden crossing, missing producers, linear duplication, protocol mismatch, effect mismatch, and forged patches;
4. local transformations can be independently replayed;
5. incremental invalidation can follow region semantic hashes;
6. the visual renderer can derive connections from explicit topology;
7. the existing QTT structures have a concrete migration destination.

It does **not** prove language-wide type soundness, ownership soundness, effect soundness, compiler correctness, or native-code equivalence.

---

## 17. Correct next implementation order

After v0.5, implementation should proceed as:

```text
1. add Glyph arenas and handles to the C compiler
2. implement QttSemanticFunction -> Glyph importer
3. seal and verify imported functions
4. render imported real functions through the visual view
5. compare Glyph facts against existing QTT oracles
6. generate Execution Glyph from explicit ports and regions
7. switch one backend cleanup/call path to Glyph authority
8. delete the corresponding legacy authority projection
9. add generated graph mutation/fuzz testing
10. begin a mechanized model of the closed kernel
```

The next version should be driven by actual imported Monad functions, not another speculative semantic category.

---

## 18. Final statement

v0.4 showed that a shaped semantic language could exercise authority.

v0.5 removes the last conceptual shortcut in that experiment:

> **the wires are no longer implied; the wires are the program.**

That is the point at which Glyph becomes a credible research IR kernel rather than a beautiful tree notation with graph aspirations.
