# Monad

Monad is an experimental functional systems language implemented in C and LLVM.
It combines S-expression and indentation-friendly Wisp syntax, Hindley-Milner
inference, dependent-type experiments, algebraic data types, type classes, C
FFI, inline assembly, a REPL, language-server tooling, and a large executable
language specification.

The repository is deliberately split between **implementation**, **language
source**, and **generated state**. Generated state does not belong beside source
files.

## Quick Start

The canonical developer front door is `./make`:

```sh
./make deps
./make all
./make test
```

The compiler produced by the Python build frontend lives at:

```text
build/bin/monad
```

Inspect the available workflows with:

```sh
./make help
./make doctor
./make targets
```

On supported Linux distributions and macOS, `./make deps --install` can install
common host build tools. Project libraries may still require the platform's LLVM,
libclang, readline, GMP, and pthread development packages.

## Repository Layout

```text
src/                         host implementation source
├── *.c, *.h                 compiler/runtime/frontend/backend
├── qtt/                     quantitative/dependent compiler pipeline
├── effects/                 effect-system implementation
├── concurrency/             concurrency runtime/compiler support
├── embed/                   embedding and compiler APIs
├── tooling/                 developer-facing tools
│   ├── lsp.c/.h             language server
│   ├── repl.c/.h            REPL
│   ├── lsp_repl.c/.h        LSP/REPL bridge
│   ├── context/             context tooling
│   └── console.py           shared terminal presentation
└── testing/                 host-side test infrastructure/contracts

core/                        shipped Monad core/prelude library
tests/                       authored Monad tests — .mon files only
how_to/                      focused executable examples
examples/                    larger language demonstrations
context/                     design/project knowledge, optional for building
glyph/                       Glyph design material, optional for building
etc/                         research/assets, optional for building
build/                       all generated build/test/editor state
```

Developer tooling is rooted at `src/tooling/`; host verification infrastructure is rooted at `src/testing/`.

The important boundary is simple:

- implementation code belongs under `src/`;
- authored language verification belongs under `tests/` and is `.mon`;
- host contracts that genuinely require Python/C/C++ belong under
  `src/testing/`;
- generated state belongs under `build/` and is disposable.

## Building

### Canonical Python build

```sh
./make all
./make debug
./make release
```

The Python frontend keeps objects, libraries, and executables under `build/`:

```text
build/obj/
build/lib/
build/bin/
```

The Python script is the only build implementation. There is intentionally no
root `Makefile`; invoke it directly for all workflows:

```sh
./make all
./make release
./make asan
./make ubsan
./make test
./make core
./make test-embedding
```

### CMake build

CMake remains a supported portable/CI build path:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake exports a compilation database automatically.

## Editor / clangd Support

`./make` maintains `compile_commands.json` for the Python build so clangd can
understand files at any depth under `src/` without changing source includes to
editor-only paths such as `../../...`.

```sh
./make compdb
```

The canonical database is written to:

```text
build/compile_commands.json
```

and a root `compile_commands.json` link is published for normal clangd ancestor
discovery.

The compilation database is generated directly from the Python build plan; GNU
Make and Bear are not involved in its generation.
Successful `./make all`, `./make debug`, and `./make release` refresh the
database automatically. Set `MAKE_COMPDB=0` only when you explicitly do not want
that behavior.

## Tests

The public test front doors are:

```sh
./make test
build/bin/monad test list
build/bin/monad test runner
build/bin/monad test core
```

`tests/` is intentionally data, not test infrastructure. Every authored file in
that tree is a `.mon` fixture. Expectations live in fixture metadata rather than
parallel `.stdout`, `.json`, or ad-hoc Python sidecars.

The canonical language runner lives under `src/testing/runner.py`. The core
module runner lives under `src/testing/core_runner.py`. Host-level contracts for
embedding ABIs, portability, CMake, PTYs, packaging, and similar concerns live
under `src/testing/contracts/` because those concerns cannot honestly be tested
as Monad programs alone.

Useful selection commands include:

```sh
./make test --list
./make test codegen
./make test codegen errors reader
./make test --name 'pattern'
./make test --only-failed
./make test --rerun-first-failure
./make test --fail-fast
./make test --validate-metadata
```

All test frontends use the same restrained terminal presentation as `./make`.

## Clean-Tree Policy

`./make clean` means a repository-level scrub, not merely `make clean`:

```sh
./make clean
./make clean --check
```

It removes generated build/test/editor/compiler state such as build trees,
objects, archives, compiler intermediates (`.mqti`, `.ll`, `.bc`), Python
caches, generated core JSON, compilation databases, coverage/profiling files,
and editor debris.

`--check` is non-destructive and exits unsuccessfully if generated state is
present. Managed Git hooks enforce the policy:

```sh
./make hooks
```

The hooks use:

```text
pre-commit  ./make clean --check
pre-push    ./make clean --check && ./make check
```

This makes a dirty source tree visible before garbage reaches Git history.

## Quality Gate

```sh
./make check
```

The quality gate is fail-closed: source hygiene, build, and canonical tests.
Additional checks belong in the Python frontend, which is the single source of
truth for build, test, packaging, installation, and cleanup.

Sanitizer workflows are first-class:

```sh
./make asan
./make ubsan
```

## Source Archives

The default archive is intentionally small and source-complete:

```sh
./make tar
```

It contains the material needed to build and verify the language:

- `src/`
- `core/`
- `.mon` tests
- `how_to/` and `examples/`
- `CMakeLists.txt` and `./make`
- managed Git hooks and package metadata

It does **not** copy `.git`, build products, caches, vendored toolchains,
prebuilt binaries, screenshots, fonts, or research PDFs.

Additional payloads are explicit:

```sh
./make tar --with-binaries
./make tar --with-vendor
./make tar --with-context
```

`--with-context` adds the design/research payload (`context/`, `glyph/`, and
`etc/`). `--with-vendor` adds the relocatable dependency/toolchain bundle.
`--with-binaries` adds the portable runtime folder.

Every archive contains `SOURCE_MANIFEST.json` and `AGENT_BUILD.md`, and packaging fails
closed if the canonical source surface is incomplete or if `tests/` contains
anything other than authored `.mon` files.

## Running Programs

Create `hello.mon`:

```monad
show "Hello, Monad"
```

Compile it:

```sh
build/bin/monad hello.mon -o hello
./hello
```

On MSYS2/Windows the compiler and generated executable use the `.exe` suffix.

## Package Builds

A Monad package can keep its own language source under its own `src/` directory:

```yaml
name: hello
executables:
  hello:
    main: Main.mon
    source-dirs: src
```

```monad
(module Main)
show "Hello, package"
```

Then:

```sh
/path/to/monadc/build/bin/monad build
./build/hello
```

The compiler resolves checkout-local `core/` and the runtime archive during
development, so installing Monad globally is not required first.

## Windows / MSYS2

Use an MSYS2 UCRT64 shell. Install the UCRT64 toolchain, CMake/Ninja, Python,
pkg-config, LLVM/Clang, readline, and GMP packages, then use either `./make` or
the CMake workflow above.

The checked-in GitHub Actions workflow builds and verifies Linux and MSYS2
Windows configurations.

## Language Surface

Monad currently explores:

- S-expression and Wisp syntaxes for the same AST;
- functions, lambdas, pattern matching, ADTs, and type classes;
- Hindley-Milner inference and monomorphized code generation;
- dependent, refinement, and quantitative type-system work;
- effects and concurrency;
- arrays, lists, sets, maps, paths, strings, characters, and numeric families;
- C FFI through libclang;
- inline assembly and low-level layout support;
- REPL/JIT workflows;
- compiler-checked test blocks and first-class verification metadata.

For the full design corpus, start at `context/info/index.org` in the repository
or build an archive with `./make tar --with-context`.
