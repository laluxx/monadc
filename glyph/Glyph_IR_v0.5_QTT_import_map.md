# Glyph IR v0.5 — QTT Import Map

This map is based on direct inspection of the uploaded QTT subsystem.

| Existing object                   | Glyph destination                                          | Migration status     |
|-----------------------------------|------------------------------------------------------------|----------------------|
| `QttSemanticFunction`             | function graph root, facet container, correspondence table | importer source      |
| `QttSemanticNode`                 | operation plus value/capability/effect ports               | importer source      |
| `QttSemanticGradeEvidence`        | erased grade proof facet/port                              | importer source      |
| `QttSemanticDestructorEvidence`   | cleanup schema instance and drop-proof facet               | importer source      |
| `QttSemanticClosureFieldEvidence` | closure region import and environment capability           | importer source      |
| `QttSemanticCapabilityTransition` | explicit capability ports and transition edge              | importer source      |
| `QttSemanticCapabilityEdge`       | replaced by arm boundary protocols and hashes              | delete after parity  |
| `QttSemanticCallEvidence`         | call schema instance and transfer ports                    | importer source      |
| `QttSemanticEffectJudgment`       | root effect protocol certificate                           | importer source      |
| `QttResourceBlock`                | independent rejection oracle                               | temporary oracle     |
| `QttAnfProgram`                   | Execution Glyph differential oracle                        | temporary oracle     |
| `QttDropMaskCertificate`          | cleanup proof object                                       | retained proof facet |
| `QttEffectRuntime`                | backend/runtime mechanism                                  | remains below Glyph  |

## Import sequence

```text
Typed Core / QttSemanticFunction
    |
    |  allocate stable source identities
    v
create Glyph regions from lexical/control structure
    |
    |  create operation ports from type/call/effect evidence
    v
create value, capability, loan, effect, and proof edges
    |
    |  create child boundary bindings
    v
seal graph
    |
    |  trusted kernel verification
    v
compare legacy resource/ANF/drop projections
```

## Fail-closed rule

If an existing evidence object cannot be represented without inventing an implicit connection, the importer returns `UNSUPPORTED_IMPORT`. It may not attach an opaque annotation and continue.

## First production specimen

Use the same hostile function shape as v0.4/v0.5:

```text
owned aggregate
borrow or projection
branch
partial move
masked drop
moved closure capture
handled effect
cleanup
join
return
```

## Deletion gate

A legacy authority path is deleted after:

1. imported Glyph verifies;
2. legacy and Glyph agree across the selected corpus;
3. the backend consumes Glyph only;
4. mutation tests prove the removed fact is load-bearing;
5. source search confirms no fallback read remains.
