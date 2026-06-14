# monadc extreme fuzz properties v4

Generated pack with 172 properties.

This pack is intentionally mostly legacy-compatible (`Int`/`Bool`, `expect:`) while stressing deeper compiler/codegen interactions:

- direct arithmetic/comparison dispatch against helper wrappers
- nested `if`, branchwise operation distribution, boolean reconstruction
- lexical `with` dependency order and shadowing
- `do` sequencing inside operands, comparisons, branches, and bindings
- min/max lattice, clamp, and guarded order laws
- mixed helper/direct dispatch feature interactions
- a small bitwise identity group without Bool coercion
- always-false negative properties for strict self-comparison and impossible lattice order

The properties live under:

```
tests/fuzzing/properties/monadc_extreme_fuzz_properties_v4/properties/
```

Your recursive fuzz loader should discover them automatically.

## Validation done here

- Required `.fuzz` fields present.
- Unique property names.
- Placeholder names match declared args.
- Laws are single-line.
- A local Python S-expression evaluator sampled 200 random assignments for each property.

I could not run the local `monad` compiler in this sandbox.

## Section counts

- arithmetic-normalization-deep: 20
- binding-scope-deep: 16
- bitwise-identities: 11
- comparison-dispatch: 4
- comparison-dispatch-binding: 4
- comparison-dispatch-do: 8
- control-do-deep: 12
- control-if-comparison: 15
- control-if-dispatch: 9
- control-if-structure: 8
- direct-dispatch: 3
- direct-dispatch-binding: 3
- direct-dispatch-do: 6
- feature-interactions-deep: 14
- lattice-clamp-deep: 16
- negative-always-false: 9
- order-logic-deep: 14
