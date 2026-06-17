# Catface query examples

These files are pasteable query catalogues. Lines beginning with `#` are comments.

- `queries.catq` — daily workflow queries, including `@info` documentation pages.
- `catalogue.catq` — broad language coverage.
- `symbols.catq` — object/edge symbol examples.
- `functions.catq` — first-class core/function and type-signature probes, including `Int -> Int`.
- `cache.catq` — persistent-cache, index, and info-page probes.
- `perf.catq` — performance probes.
- `metadata.catq` — metadata lanes, context contracts, confidence/risk, structured record probes, and living-right-pane link/table probes.

Run one expression through the CLI with:

```sh
zig build run -- --query '@tests -> reader' ../../../
zig build run -- --query '@info advanced-types' ../../../
zig build run -- --query 'Int -> Int' ../../../
zig build run -- --query 'org link table right pane' ../../../
```

Print structured reports with:

```sh
zig build cache-report
zig build query-report
zig build perf
zig build test-report
```
