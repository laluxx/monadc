# Codegen/type-system corpus quality report

- total `.mon` fixtures: 1756
- pass/run fixtures: 978
- fail/compile fixtures: 760
- type-system menu fixtures: 200
- HM fixtures: 150
- dependent-type fixtures: 30
- dep/HM bridge fixtures: 20
- type-system negative fixtures: 133
- atom nodes: 1756
- graph edges: 13029
- duplicate TEST-ID values: 0
- missing required metadata rows: 0

The type-system menu is intended for recursive CLI targets such as:

```sh
monad test types
monad test types hm
monad test types dep
monad test types bridge
monad test types hm unification
monad test types dep refinement
```

Each atom is a leaf directory and carries TEST-INFER-PATH / TEST-DEP-PATH metadata when it targets infer.c or dep.c directly.
