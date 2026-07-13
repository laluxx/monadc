# Monad

<div align="center">
  <img src="./etc/logo.png" alt="Monad logo" width="160">
</div>

Monad is an experimental functional systems language implemented in C and
LLVM. It combines a Lisp-like core, indentation-friendly Wisp syntax,
Hindley-Milner inference, dependent-type experiments, algebraic data types,
type classes, C FFI, inline assembly, a REPL, and an end-to-end compiler test
suite.

This repository is the compiler, runtime, standard-library seed, examples,
tests, and project context for the language.

## Current Status

Monad is not a polished general-purpose language yet. It is a serious compiler
workbench with many implemented language features and a large regression suite.

What is usable today:

- Compile `.mon` files to native executables through LLVM.
- Use either S-expression syntax or Wisp indentation syntax.
- Run the REPL, CLI helpers, core library tests, codegen fixtures, reader tests,
  and bytecode experiments.
- Parse C headers through libclang for FFI declarations.
- Build on Linux and MSYS2/Windows through the checked-in build paths.

What is still experimental:

- Some language areas are intentionally covered by future/known-failure tests.
- The core library is still being shaped.
- REPL/JIT and FFI-heavy workflows depend on platform linker behavior.
- The context corpus is extensive and useful, but it is not introductory
  documentation.

## Quick Start

Dependencies on Linux:

```sh
sudo apt install build-essential cmake ninja-build python3 llvm-dev libclang-dev libreadline-dev libgmp-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/monad --help
```

Run the main test suite:

```sh
make test
```

Explore the focused test suites:

```sh
./build/monad test list
./build/monad test runner
./build/monad test how-to
python tests/main.py list
python tests/main.py runner
python tests/main.py how-to
```

Run core library tests:

```sh
make test-core
```

Use the Python build wrapper for diagnostics:

```sh
./make doctor
./make test
```

## Windows / MSYS2

Windows is supported through MSYS2. Use the MSYS shell, not PowerShell or a
plain `cmd.exe` prompt.

Install dependencies:

```sh
pacman -S --needed base-devel git
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-llvm mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-readline mingw-w64-ucrt-x86_64-gmp
```

Build and smoke test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/monad.exe --help
MONAD_BINARY="$PWD/build/monad.exe" ./build/monad.exe test runner
```

The repository also includes a GitHub Actions workflow at
`.github/workflows/ci.yml` that performs CMake builds on Linux and
MSYS2/UCRT64 Windows.

## First Program

Create `hello.mon`:

```monad
show "Hello, Monad"
```

Compile and run it:

```sh
./build/monad hello.mon
./hello
```

On MSYS2/Windows the output binary is suffixed:

```sh
./build/monad.exe hello.mon
./hello.exe
```

## Package Builds

For a small package, place source under `src/` and add `package.yaml`:

```yaml
name: hello
executables:
  hello:
    main: Main.mon
    source-dirs: src
```

Create `src/Main.mon`:

```monad
(module Main)
show "Hello, package"
```

Build and run it from the package directory:

```sh
/path/to/monadc/build/monad build
./build/hello
```

`monad build` finds checkout-local `core/` and `libmonad.a` through the compiler
binary location, so a development checkout does not need `make install` before
building a package.

## Examples

The `how_to/` directory contains executable language examples:

```sh
./build/monad how_to/Syntax.mon
./build/monad how_to/AlgebraicDataTypes.mon
./build/monad how_to/Macros.mon
./build/monad how_to/Iter.mon
```

Some examples depend on external graphics libraries or platform-specific
headers. Start with the syntax, ADT, macro, and iterator examples before trying
the speedrun demos.

## Build Targets

The CMake build is the primary portable build path.

| Command                                      | Purpose                                                  |
|----------------------------------------------|----------------------------------------------------------|
| `cmake -S . -B build -G Ninja`               | Configure a debug build                                  |
| `cmake --build build --parallel`             | Build `monad` and `libmonad.a`                           |
| `ctest --test-dir build --output-on-failure` | Run CMake smoke and portability checks                   |
| `cmake --install build --prefix /path`       | Install compiler, runtime archive, header, and core libs |

The root `Makefile` remains available while the migration completes.

| Command                     | Purpose                                                     |
|-----------------------------|-------------------------------------------------------------|
| `make` or `make all`        | Debug compiler build                                        |
| `make release`              | Optimized compiler build                                    |
| `make asan`                 | AddressSanitizer build                                      |
| `make test`                 | Full runner: reader, Wisp, language, and codegen fixtures   |
| `make test-core`            | Core module test blocks                                     |
| `make test-runner`          | Compiler-facing unified runner suite                        |
| `make test-how-to`          | Compiler-facing README-listed example smoke suite           |
| `make clean`                | Remove compiler artifacts                                   |
| `make install PREFIX=/path` | Install compiler, runtime archive, header, and core modules |

The `./make` helper wraps common workflows and adds project diagnostics,
portable packaging, and vendor/runtime closure commands:

```sh
./make help
./make doctor
./make release
./make vendor
./make tar --with-binaries
```

## Repository Map

| Path         | What lives there                                                 |
|--------------|------------------------------------------------------------------|
| `*.c`, `*.h` | Compiler, runtime, REPL, FFI, LSP, bytecode, and support code    |
| `core/`      | Active core/prelude modules                                      |
| `how_to/`    | Small executable examples                                        |
| `tests/`     | Regression fixtures and Python test harnesses                    |
| `context/`   | Source-grounded project memory, design notes, and subsystem docs |
| `etc/`       | Logo, screenshots, fonts, and reference PDFs                     |
| `CMakeLists.txt` | Primary portable build/test/install path                     |
| `Makefile`   | Legacy build/test/install path during migration                  |
| `make`       | Python build helper                                              |

## Language Surface

Monad currently includes:

- S-expression and Wisp syntaxes for the same AST.
- Functions, lambdas, pattern matching, algebraic data types, and type classes.
- Hindley-Milner inference with monomorphized codegen.
- Dependent-type and refinement-type experiments.
- Arrays, lists, sets, maps, paths, chars, strings, numeric literals, and
  fixed-width numeric types.
- C FFI through libclang header parsing.
- Inline assembly and low-level layout support.
- Test blocks that can be compiled and executed by the test runner.

For deeper language docs, start in `context/info/index.org`.

## Verification

The important local checks are:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python tests/test_windows_portability.py
./build/monad test list
./build/monad test runner
python tests/main.py list
python tests/main.py runner
make test-runner
make test-core
make test
```

`make test` is the broadest suite and can take longer because it compiles and
runs many fixtures. Use the narrower commands while working on a specific
subsystem.

## Editor Support

The companion Emacs mode is
[monad-mode](https://github.com/laluxx/monad-mode). It provides syntax
highlighting, REPL integration, inline assembly support, and linting helpers.

## Project Notes

Monad is intentionally ambitious: it explores the boundary between high-level
functional programming and low-level systems control. The cost of that ambition
is complexity. The way to keep the project workable is to route changes through
small tests, source-grounded context records, and honest build verification.

Useful entry points:

- `context/info/index.org` for language documentation.
- `context/build.org` for build and verification policy.
- `context/tests.org` for test metadata and regression conventions.
- `context/source-map.org` for source-file orientation.
- `TODO.org` for open work.
