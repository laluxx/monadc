# Agent build notes for monad

This archive was produced by `./make tar` from the canonical source layout.

## Included

- src/ — compiler, runtime, embedding, tooling, and host verification infrastructure
- core/ — shipped Monad core library
- tests/ — authored .mon verification corpus only
- how_to/ and examples/ — executable language examples
- Makefile, CMakeLists.txt, and ./make — canonical build frontends
- .githooks/ and .github/ — clean-tree enforcement and CI verification contract
- SOURCE_MANIFEST.json — package hashes and build metadata

## Build and verify

```sh
./make doctor
./make all
./make test
```

`./make all` refreshes `compile_commands.json` for clangd automatically.
Run `./make compdb` explicitly when you only want to refresh editor metadata.

## Clean-tree invariant

```sh
./make clean --check
```

Generated compiler/test/build state belongs under `build/` and is never source.
