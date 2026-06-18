<div align="center">

<img src="./catface.svg" alt="Catface logo" width="180"/>

# Catface

### A fast category-theory terminal cockpit for the MonadC context garden

*Search TODOs. Read notes. Follow proofs. Jump through compiler evidence without leaving the terminal.*

</div>

> **Purpose:** Catface is not a generic curses-style file browser. It is a purpose-built interface for the `context/` category: objects are tests, notes, records, source files, reports, TODOs, and concepts; arrows are evidence links such as `verifies`, `supports`, `blocks`, `refines`, and `id-link`.

---

## 1. Repository Map

```text
catface/
├── build.zig             — build, run, test, perf, and report steps
├── build.zig.zon         — Zig package metadata
├── catface.svg           — GitHub-rendered logo used by this README
├── README.md             — this manual
├── examples/             — query catalogues to paste into the TUI or CLI
│   ├── queries.catq      │   daily query cookbook
│   ├── catalogue.catq    │   broad language/category examples
│   ├── symbols.catq      │   symbol/algebra examples
│   ├── cache.catq        │   cache and indexed-search probes
│   └── perf.catq         │   performance probe queries
└── src/
    ├── main.zig          — CLI entry point and report commands
    ├── app.zig           — TUI event loop and input routing
    ├── ui.zig            — mutable UI state, query cache, focus stack
    ├── render.zig        — terminal layout, object cards, relation tree
    ├── terminal.zig      — raw terminal, SGR mouse, dirty-cell renderer
    ├── model.zig         — category object/edge model
    ├── org.zig           — context loader
    ├── index.zig         — inverted text index + adjacency caches
    ├── query.zig         — query language evaluator
    ├── tree.zig          — collapsible relation-tree state
    ├── perf.zig          — monotonic timing helpers
    ├── perf_report.zig   — structured JSONL reports
    ├── file_cache.zig    — ignored-dir-aware file discovery cache
    ├── context_cache.zig — persistent serialized context snapshot cache
    ├── glyphs.zig        — object/edge/category symbols
    ├── palette.zig       — terminal color theme
    ├── manual.zig        — in-app help/manual text
    └── tests.zig         — module test harness
```

---

## 2. Philosophy

Catface is a terminal interface for a compiler memory system. The core workflow is:

1. **Ask a narrow question** with a compact query.
2. **Read the selected object** in the right pane.
3. **Follow arrows** in the bottom relation tree.
4. **Use tests and observations as evidence** before editing compiler code.

The interface is intentionally category-shaped:

```text
object ──arrow──▶ object
Test   ─verifies▶ Source
OBS    ─supports▶ Decision
TODO   ─blocks──▶ Feature
Note   ─id-link─▶ Record
```

The left pane gives you a ranked object stream. The right pane gives you object meaning plus its local Hom-neighborhood.

---

## 3. Version

**v0.7.6 — Function Cache**

This version focuses on first-class function/type search, faster index construction, persistent cache correctness, and richer right-pane navigation:

- First-class `Function` objects are extracted from Monad/Wisp/Lisp source definitions and searchable by name or type signature.
- Plain type-arrow searches such as `a -> a` and `Int -> Int` find functions; relation arrows like `@tests -> reader` still search category morphisms.
- Context snapshots are serialized under `~/.config/catface/cache/` by default.
- Cache invalidation uses the discovered text-artifact manifest: relative path, size, and mtime.
- `zig build cache-report` prints structured JSONL for cache hits/misses and scan time.
- `@info` now covers `context/info/*.org` pages with their stable Org `:ID:` values, such as `monadc.info.advanced-types`.
- Info-page previews are cleaned: no `#+` directives, property drawers, link-only rows, or source blocks in the right card.
- Broad lanes such as `@hot`, `@notes`, `@info`, `@tests`, `@roots`, `@leaves`, and `@orphans` are indexed lane buckets instead of repeated full scans.
- Relation queries use set-only evaluation for the left/right sides, avoiding expensive ranking work before walking arrows.
- Top-N ranking avoids sorting thousands of candidates when the UI only needs the first page.
- Index construction reuses token scratch buffers instead of allocating per token; this should reduce startup/index-build cost noticeably.

---

## 4. Build and Run

From `context/category/catface`:

```sh
zig build
zig build run -- ../../../
```

Passing `../../../` points Catface at the compiler repository root, so it can search `context/`, tests, source, scripts, and docs. Passing `../../` limits it mostly to the `context/` subtree.

Run deterministic tests:

```sh
zig build test
```

`zig build test` is intentionally quiet on success. Use the report steps when you want structured output to paste into a discussion.

---

## 5. Structured Reports

### Performance report

```sh
zig build perf
# equivalent after install/build:
zig build run -- --perf ../../../
```

This prints JSONL records like:

```jsonl
{"catface_perf":"context","objects":3590,"edges":4421}
{"catface_perf":"index_build","objects":3590,"edges":4421,"terms":12000,"ns":1234567}
{"catface_perf":"query","name":"todo_lane","query":"@todo","rounds":32,"matches":44,"total_ns":222222,"avg_ns":6944}
```


### Persistent cache report

```sh
zig build cache-report
# or
zig build run -- --cache-report ../../../
```

This prints JSONL records for the file-manifest scan and snapshot load:

```jsonl
{"catface_cache":"manifest","files":812,"dirs_scanned":41,"dirs_skipped":7,"signature":"...","scan_ns":123456,"path":"/home/me/.config/catface/cache/context-....tsv"}
{"catface_cache":"load","hit":true,"objects":5748,"edges":4238,"ns":9876543}
```

The first run for a changed tree is expected to be a miss and will save the snapshot. The next run should be a hit. The cache is intentionally outside the repository so normal source trees stay clean; set `CATFACE_CACHE_DIR` to override it.

### Query correctness/performance report

```sh
zig build query-report
# or
zig build run -- --query-report ../../../
```

This evaluates the built-in query catalogue and prints one JSONL record per query. It is useful when a search lane feels slow or wrong.

### Test-style report

```sh
zig build test-report
# or
zig build run -- --test-report ../../../
```

This prints structured `catface_test` records for query-language invariants, relation syntax, exact-token search, and performance smoke checks. Normal `zig build test` remains the source of pass/fail truth; this command exists so you can paste evidence back for tuning.

---

## 6. CLI Reference

```sh
catface [project-or-context-root]
catface --root <project-or-context-root>
catface --query <expr> [project-or-context-root]
catface --card <object-id> [project-or-context-root]
catface --check [project-or-context-root]
catface --dump-objects [project-or-context-root]
catface --perf [project-or-context-root]
catface --query-report [project-or-context-root]
catface --test-report [project-or-context-root]
catface --cache-report [project-or-context-root]
catface --help
```

Useful examples:

```sh
catface --query '@todo' ../../../
catface --query '@bugs' ../../../
catface --query '@notes reader' ../../../
catface --query '@functions' ../../../
catface --query 'a -> a' ../../../
catface --query '=reader' ../../../
catface --query 'title:reader path:src' ../../../
catface --query '@tests -> reader' ../../../
catface --query 'reader <- @tests' ../../../
catface --query '%verifies @tests -> codegen' ../../../
catface --card 'monadc.context.category.index.purpose' ../../../
catface --perf ../../../
catface --query-report ../../../
catface --cache-report ../../../
```

---

## 7. Interface Model

Catface is a two-pane terminal cockpit.

### Top rail

The top rail shows:

- Catface name and version,
- corpus size,
- live query line,
- quick search lanes,
- footer telemetry for frame/query/flush time.

### Left pane: ranked object stream

The left pane is compact and symbolic. It intentionally uses short category marks instead of wide colored bubbles because this pane is for fast scanning.

Example row shape:

```text
▌ T TEST  reader layout gap regression
   tests/reader-layout.mon:42  tests.reader.layout-gap
   verifies reader layout method gap behavior
```

Use:

- `C-n` / `C-p` globally to move results,
- arrows or `PageUp` / `PageDown`,
- mouse wheel over the left pane,
- `Enter` to pin the selected object as `?id`.

`C-n` and `C-p` keep moving the left pane even when the right pane is focused.

### Right pane: selected object + relation tree

The right pane has two conceptual regions:

1. **Top object card** — rounded colored tag, kind, title, id, source path, trust signals, and clean text.
2. **Bottom relation tree** — collapsible Hom-tree of incoming/outgoing arrows.

Right-pane keys:

```text
Tab      switch panes
n / j    move down in relation tree
p / k    move up in relation tree
RET / l  open selected heading/object row
h        jump back to the previous relation target
```

Mouse:

- click `▸` / `▾` direction headings to collapse or expand,
- click edge-kind headings to collapse or expand edge groups,
- click object rows to select relation targets,
- wheel scrolls the active pane.

---

## 8. Symbol Algebra

Catface uses compact terminal symbols so category rows fit inside a narrow pane. The symbols are not decoration; they encode the object or arrow kind.

### Object symbols

| Symbol | Object kind | Meaning |
|---|---|---|
| `T` | Test | A compiler/test-suite contract. Usually expected to verify source behavior. |
| `S` | Source | Compiler source, scripts, or implementation surface. |
| `λ` | Function | Parsed function/type object from Monad/Wisp/Lisp source. |
| `▣` | Record | Context fact/claim such as OBS, DEC, INF, or FIX. |
| `I` | Info / Note | Documentation or explanatory note. |
| `!` | TODO | Open work item. |
| `✓` | Done | Completed work/evidence. |
| `◯` | Concept | Abstract category/language concept. |
| `◇` | File | File-level object. |
| `▤` | Report | Generated report or audit artifact. |
| `⌁` | Script | Script/tooling surface. |
| `◆` | Heading | Org/Markdown heading object. |
| `·` | Unknown | Object exists but has weak type information. |

### Edge symbols

| Symbol | Edge kind | Category reading |
|---|---|---|
| `✓` | verifies | `test ✓ source`: test verifies behavior of the target object. |
| `⊢` | supports | `record ⊢ decision`: source object supports the target. |
| `⊣` | blocks | `todo ⊣ feature`: source object blocks target work. |
| `≤` | refines | source object is a refinement/specialization of target. |
| `⇢` | id-link | explicit identity/cross-reference link. |
| `⊃` | contains | file/heading contains another object. |
| `↦` | file-link | textual link to a file object. |
| `≻` | supersedes | newer object replaces older object. |
| `∈` | classifies-as | object belongs to a class/concept. |
| `ƒ` | forgets-to | missing/forgotten relation or documentation obligation. |
| `↺` | generated-by | generated artifact points back to generator. |
| `∋` | mentions | loose textual mention. |
| `→` | unknown | edge is present but not typed strongly. |

### Relation tree equations

```text
OUT  Hom(object, -)  = all arrows leaving the selected object
IN   Hom(-, object)  = all arrows entering the selected object
```

Examples:

```text
@tests -> reader
```

means: “Find objects reachable by arrows from test-like objects to reader-like objects.”

```text
reader <- @tests
```

means: “Find reader-like objects that have incoming arrows from test-like objects.”

```text
%verifies @tests -> codegen
```

means: “Restrict the relation search to `verifies` arrows from test objects into codegen objects.”

---

## 9. Query Language

Catface’s query language is deliberately small and fast.

### Words

```text
reader layout
wisp define
path literal
```

Words search object id, title, path, tags, kind, and preview text. Exact hits use the index first; approximate expansion is only used when necessary.

### Exact token search

```text
=reader
=TODO
=needlefast
```

`=term` means “only exact normalized token hits.” Use it for precise probes and performance checks.

### Kind filters

```text
:Test
:Record
:Source
:Concept
:Todo
:Done
:Info
```

### Namespace lanes

```text
@todo
@bugs
@notes
@tests
@info
@functions
@source
@reader
@wisp
@codegen
@reports
@fix
@hot
@triage
@blocked
@blockers
@roots
@leaves
@orphans
@obs
@dec
@inf
```

Useful daily queries:

```text
@todo reader
@bugs codegen
@notes wisp define
@obs path literal
@dec syntax
@inf type inference
@blocked
@hot
@info advanced-types
@functions id
a -> a
Int -> Int
=monadc.info.advanced-types
```

### Field filters

```text
title:reader
path:reader.c
id:monadc.context
preview:TODO
tag:tests
```

Combine field filters with lanes:

```text
@todo title:reader
@tests path:reader
@notes preview:OBS
```

### Edge filters

```text
%verifies
%supports
%blocks
%refines
%mentions
%generated-by
```

### Category relation search

```text
lhs -> rhs
lhs <- rhs
```

Both sides are normal subqueries when they look like object expressions (`@tests -> reader`). When the expression looks like a type signature (`a -> a`, `Int -> Int`, `List a -> a`), Catface treats it as function type search instead of relation search. This makes relation search compositional:

```text
@tests -> reader
reader <- @tests
%verifies @tests -> codegen
@todo -> @source
@obs -> @dec
@blocked <- @todo
```

### Graph operators

```text
>       outgoing expansion
<       incoming expansion
~       one-step neighborhood
proj    concept/taxonomy projection
```

---


## 10. Info Pages

The `context/info/*.org` pages are first-class `Info` objects. A page like:

```org
#+TITLE: Advanced Types
#+FILETAGS: :monad:language:advanced-types:info:
:PROPERTIES:
:ID:       monadc.info.advanced-types
:END:
```

becomes searchable by title, path, tags, and stable id:

```text
@info
@info advanced-types
@info type-inference
=monadc.info.advanced-types
path:context/info
```

The right pane renders the page as clean explanation text. Catface deliberately strips Org machine directives such as `#+TITLE`, property drawers, link-only navigation rows, and source blocks from the visible summary, while still indexing the meaningful prose.

## 11. Examples Directory

The `examples/` directory is a query catalogue, not test fixture noise. Open these files and paste lines into Catface, or run similar expressions through `--query`.

| File | Use |
|---|---|
| `examples/queries.catq` | Main cookbook for day-to-day exploration. |
| `examples/catalogue.catq` | Broader coverage of lanes, fields, relations, and graph ops. |
| `examples/symbols.catq` | Queries that make the object/edge symbols meaningful. |
| `examples/cache.catq` | Queries intended to exercise cached/indexed paths. |
| `examples/perf.catq` | Performance probes for `--perf` and manual timing. |
| `examples/functions.catq` | Function-name and type-signature queries. |

Try:

```sh
sed -n '1,120p' examples/queries.catq
zig build run -- --query '@hot' ../../../
zig build run -- --query '@functions' ../../../
zig build run -- --query 'a -> a' ../../../
zig build run -- --query '@tests -> reader' ../../../
zig build run -- --query '%verifies @tests -> codegen' ../../../
```

---

## 12. Performance Model

Catface tries to respect the CPU:

- context files are discovered with an ignored-directory cache,
- parsed context snapshots are persisted in `~/.config/catface/cache/`,
- `.git`, `.zig-cache`, `node_modules`, `__pycache__`, `build`, and `dist` are skipped,
- a startup search index stores normalized text postings,
- object-kind buckets are precomputed,
- edge-kind buckets are precomputed,
- incoming/outgoing adjacency maps are precomputed,
- per-object/per-edge-kind degree counters are precomputed,
- query results are cached in UI state,
- the terminal is redrawn only when state changes,
- flush writes dirty cells/runs instead of clearing the whole screen.

The footer shows live telemetry:

```text
frame 120000ns  query 8000ns  flush 42000ns  redraws 128  cached 77
```

For reproducible output, use:

```sh
zig build perf
zig build query-report
zig build test-report
zig build cache-report
```

---

## 13. Recommended Workflow

1. Start broad:

   ```text
   @hot
   @todo
   @bugs
   ```

2. Narrow by subsystem:

   ```text
   @todo reader
   @bugs wisp
   @tests codegen
   ```

3. Ask category questions:

   ```text
   @tests -> reader
   reader <- @tests
   %verifies @tests -> codegen
   ```

4. Press `Tab` and use the relation tree to follow evidence.
5. Use `h` when you jumped too far.
6. Use `zig build query-report` or `zig build perf` when performance or ranking feels off.

---

## 14. Notes for Future Work

Good next improvements:

- persist the full search index snapshot, not only the parsed context snapshot,
- add command history search,
- add dedicated object preview scrolling,
- add per-query allocation counters,
- add a report diff command to compare two performance runs,
- add a generated symbol legend inside `--help` and `?` output for very narrow terminals.

Catface should stay small, fast, and category-specific. When in doubt, prefer better evidence navigation over generic file-browser features.
