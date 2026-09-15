# Testing architecture

The test system has one deliberate authoring surface: **Monad source**.

## Authored tests

`tests/` contains only `.mon` files. Expectations that used to live beside a
fixture as `.stdout`, `.json`, or `.desugar` sidecars are stored in that
fixture's `TEST-*` metadata, so a test is one source file and one source of
truth.

Reader corpus rows that used to live in TSV files are normal `.mon` fixtures
under `tests/reader/`. Core law programs are normal `.mon` fixtures under
`tests/laws/`.

Run them with:

```sh
./make test
./make test --list-targets
./make test codegen errors reader
./make test --only-failed
```

Generated results and preserved failures live under `build/test/`; they never
belong in `tests/`.

## Test implementation

- `runner.py` — canonical `.mon` suite discovery/execution and terminal UI.
- `monad_binary.py` — compiler binary resolution.
- `core_runner.py` — isolated execution for test blocks embedded in shipped core modules.
- `contracts/` — Python host/toolchain/API contracts that cannot honestly be
  expressed as Monad programs (PTY behavior, C embedding ABI, build-system
  policy, platform checks, and similar boundaries).
- `contracts/fixtures/` — C/C++/shader inputs owned by those host contracts.
- `data/` — non-test input corpora such as property/fuzz descriptions and
  generated documentation data.

Python is infrastructure here, not the language-level test format.
