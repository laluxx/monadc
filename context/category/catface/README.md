#+TITLE: Catface
#+FILETAGS: :monadc:category:catface:tui:zig:

Catface is the fast terminal cockpit for the MonadC category/context garden.  It is not a generic file browser and it is not a curses clone.  One search surface lets you move through the project as a category: objects are tests, info pages, source files, functions, TODO/DONE headings, records, observations, bugs, examples, reports, and design notes; arrows are the explicit links between them such as `contains`, `supports`, `verifies`, `blocks`, and `refines`.

**v0.11.6 — Subtree Org Editor Cockpit** focuses on three things: subtree-scoped cache loading, first-class Org body rendering, and editor-grade navigation from the right pane.

* Build

#+begin_src sh
zig build
zig build test
#+end_src

* Run

From =context/category/catface=:

#+begin_src sh
zig build run -- ../../
#+end_src

Catface treats the argument, or the current directory when no argument is passed, as the **recursive root**.  It indexes that directory downward only.  It never walks upward to find more context behind your back.

Useful command-line modes:

#+begin_src sh
catface --query '@todo' ../../
catface --query '@todo @tests' ../../
catface --card monadc.context.category.index.purpose ../../
catface --cache-report ../../
catface --query-report ../../
catface --test-report ../../
catface --perf ../../
catface --perf-self-test ../../
#+end_src

* Project structure

| path | purpose |
|------+---------|
| =src/main.zig= | CLI, cache report, query/card/perf entry points |
| =src/app.zig= | interactive application loop, key handling, mouse activation, editor launching |
| =src/ui.zig= | mutable UI state, minibuffer, palette, right-pane cursor/scroll state |
| =src/org.zig= | recursive context loading, Org parsing, record block extraction, source/test/function objects |
| =src/context_cache.zig= | subtree-scoped serialized context cache |
| =src/file_cache.zig= | recursive file manifest and skip rules |
| =src/index.zig= | search indexes and namespace lanes |
| =src/query.zig= | query language and indexed evaluation |
| =src/render.zig= | layout, result rows, right Org view, chips, relation tree, help/palette overlays |
| =src/keymap.zig= | Emacs-like key notation, chords, and descriptions |
| =src/which_key.zig= | delayed which-key-style prefix popup |
| =src/perf.zig= | timings, budgets, palette/query tracing |
| =examples/= | executable query examples and catalogues |

* Cache model

Catface now keys the context cache by the normalized recursive root.  A cache produced for =context/= is not loaded for =context/category/catface=, and a Catface subtree cache is not allowed to stand in for the whole project.  The cache header stores both the content signature and the exact root, so stale or wrong-branch cache files are rejected before allocating the object graph.

This matters because the right default is locality: when you launch Catface from a subtree, you should pay for and hold only that subtree in memory.

* Query language

Queries are whitespace-separated forms.  Multiple forms are a conjunction unless a relation operator changes the shape.

| form | meaning |
|------+---------|
| =word= | fuzzy search id, title, path, tags, preview |
| ==word= | exact normalized-token search |
| =:Kind= | object kind filter, for example =:Test=, =:Info=, =:Record= |
| =@lane= | first-class namespace/lane filter |
| =?id= or =#id= | identity/object id search |
| =path:term= | path search |
| =title:term= | title search |
| =%edge= | edge-kind filter, such as =%verifies= or =%blocks= |
| =lhs -> rhs= | relation search from left object query to right object query |
| =lhs <- rhs= | reverse relation search |
| =>=, =<=, =~= | outgoing, incoming, neighborhood projections |
| =proj= | taxonomy/category projection |
| =Int -> Int= | type-signature search when both sides are type-like |

Important lanes:

| lane | surface |
|------+---------|
| =@todo= | TODO headings and TODO objects |
| =@done= | DONE evidence |
| =@tests= | test contracts and fixtures |
| =@info= | first-class info pages, not notes |
| =@notes= | notes/context records, excluding Info pages |
| =@records= | OBS/DEC/BUG/FIX/INF style records |
| =@bugs= | BUG/FAIL/error/regression surface |
| =@failures= | failed tests and failure records |
| =@regressions= | behavioral drift and stale goldens |
| =@performance= | cache/index/allocation/latency notes |
| =@coverage= | test coverage and missing proof surfaces |
| =@examples=, =@tutorials= | examples and learning material |
| =@api=, =@cli= | interface and command surfaces |
| =@cache= | cache behavior and cache correctness |
| =@diagnostics= | errors, warnings, failure reporting |
| =@design= | architecture and design decisions |
| =@metadata= | Org headers, properties, context metadata |
| =@links=, =@tables=, =@docs= | Org links, tables, and document-like surfaces |
| =@functions= | first-class functions and type signatures |
| =@source= | compiler/runtime source and scripts |
| =@reader=, =@wisp=, =@codegen= | focused compiler subsystems |

Examples:

#+begin_src text
@todo reader
@todo @tests
@bugs @reader
@functions Int -> Int
@tests -> reader
reader <- @tests
%verifies @tests -> codegen
path:src title:reader
#+end_src

* Completion palette

=TAB= opens the flat bottom completion palette.  It is orderless-inspired and context-sensitive.

- With an empty query it completes commands and lanes.
- After a lane plus a space, such as =@todo =, it switches to objects in that lane.
- Conjunctions work: =@todo @tests = completes objects that satisfy both surfaces.
- Candidate rows are capped and single-line sanitized so completion cannot grow until it corrupts the screen.

Palette keys:

| key | action |
|-----+--------|
| =C-n= / =C-p= | next/previous candidate |
| =C-c p= / =C-c n= | first/last candidate |
| =RET= | accept candidate |
| =ESC= / =C-g= | cancel |

* Right pane as an Org/category viewer

The right pane is the main reading surface.  The top rows show metadata as clickable chips, and the body renders the actual context text.  Org metadata is not repeated in the body once it has become a chip.

Rendered features:

- =#+TITLE:=, =#+FILETAGS:=, and property drawer fields become top chips.
- Headings render with colored bullets instead of raw stars.
- =TODO=, =DONE=, =WIP=, and priorities like =[#A]= / =[#B]= / =[#C]= are colored.
- Org trailing tags like =:reader:wisp:= become clickable buttons.
- Links and buttons are clickable but are not underlined until hovered.
- Observation/decision/bug records render their prose, not only the =[OBS id:...]= metadata header.
- =#+BEGIN_EXAMPLE= / =#+END_EXAMPLE= delimiters are hidden; the example body gets a darker full-line background.
- Org tables render as structured table rows.
- The bottom relation tree shows the categorical neighborhood of the selected object.

Right-pane keys:

| key | action |
|-----+--------|
| =ESC= | focus the right pane from search mode |
| =i= | focus the left search pane |
| =n= / =p= | next/previous Org target, tag, heading, record, or link |
| =TAB= | next Org link/button |
| =g= / =G= | top/bottom of Org body |
| =h= | go back through followed relation/link history |
| =j= / =k= | move relation-tree cursor |
| =l= / =RET= | open relation-tree row |
| =v= | open full document viewer |
| mouse release | activate hovered link/chip/tree row; drag away cancels |

Clicking the =[at: line:N]= chip launches =$EDITOR +N <file>=.  If =$EDITOR= is unset, Catface falls back to =$VISUAL= and then =vi=.

* Global keybindings

| key | action |
|-----+--------|
| =C-n= / =C-p= | next/previous result or candidate |
| =RET= | from left pane, enter right content mode |
| =TAB= | command/completion palette; in right pane, next Org link/button |
| =C-i= / =Alt-i= | Info lane, when terminal distinguishes =C-i= from =TAB= |
| =C-o= / =Shift-TAB= | switch panes |
| =C-c c= | command palette |
| =C-c d= | edit recursive root in minibuffer |
| =C-h c= | describe next key briefly |
| =C-h ?= | force which-key-style prefix help |
| =C-x C-c= | quit Catface |
| =Alt-t= | =@todo= |
| =Alt-n= | =@notes= |
| =Alt-e= | =@tests= |
| =Alt-s= | =@source= |
| =Alt-f= | =@functions= |
| =Alt-u= | =@bugs= |
| =Alt-k= | =@contracts= |
| =Alt-y= | =@quality= |
| =Alt-a= | =@metadata= |
| =Alt-l= | =@links= |

* Minibuffer and editing

Catface keeps directory changes and prompts in the minibuffer, not a separate pane.  Editing supports the expected Emacs-style movement and deletion commands: =C-a=, =C-e=, =C-u=, arrows, Home/End, Delete, and Backspace.  Editing spots use a block cursor so search, completion, and directory prompts feel like one integrated interface.

* Performance and proof

Catface is written to make performance observable rather than guessed.

- Subtree-scoped cache keys prevent loading useless parent-project graphs.
- The search index keeps lane membership and token data precomputed.
- The palette reuses candidate buffers and caps visible rows.
- Right-pane navigation counts visible Org body lines while skipping metadata and block delimiters.
- Mouse wheel escape packets are coalesced so partial terminal input cannot leak into the prompt.
- The footer reports frame/query/flush/palette timings.
- =--perf-self-test= emits JSON lines for index construction and representative indexed queries.
- =--cache-report= proves cache hit/miss/stale behavior for the active subtree.
- =--query-report= and =--test-report= give stable surfaces for regression testing.

Example:

#+begin_src sh
catface --perf-self-test ../../
catface --cache-report ../../
#+end_src

The goal is to keep hot interaction paths allocation-light and bounded: visible palette rows are capped, right-pane actions are resolved from the selected object plus line index, and cache loading is rejected early when the root branch or content signature does not match.

* Context conventions

Catface understands the MonadC context as an Org superset.  The most useful conventions are:

#+begin_example
#+TITLE: Reader contracts
#+FILETAGS: :reader:wisp:tests:
:PROPERTIES:
:ID: monadc.context.reader.contracts
:END:

* TODO [#A] Fix reader layout gap :reader:bug:
[OBS id:obs.reader-gap supports:test.reader-layout]
The actual observation prose should be readable in the right pane.

#+BEGIN_EXAMPLE
for x -> [1 2 3]
  show x
#+END_EXAMPLE
#+end_example

The metadata becomes chips, the TODO/priority/tag state becomes colored structure, the observation becomes a first-class record, and the example body renders as a dark block without showing the delimiters.
