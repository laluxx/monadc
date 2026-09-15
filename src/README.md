# Source layout

`src/` is the home for implementation code and developer infrastructure.

The repository is moving toward this shape:

- `src/compiler/` — compiler implementation and native frontend/backend code.
- `src/runtime/` — native runtime implementation.
- `src/embed/` — public embedding implementation and host ABI.
- `src/testing/` — test runner, host contracts, fixtures, and property data.
- `src/tooling/` — reusable developer-facing tooling such as terminal UI code.

`core/` remains Monad source: it is the language's shipped core library rather
than host implementation code. `tests/` is intentionally different as well: it
is the authored Monad verification surface and contains only `.mon` files.

The current source package predates the native-source move and may still have
native files outside `src/`. `./make doctor` reports build-referenced files that
are missing from a package, and `./make tar` now fails closed instead of silently
creating an incomplete source archive.
