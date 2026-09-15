const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");

pub const Section = struct { title: []const u8, body: []const u8 };

pub const sections = [_]Section{
    .{ .title = "Why Catface exists", .body = "Catface makes the context category navigable as a living object. It is designed for fuzzy search, fast keyboard navigation, and direct category operations rather than menus." },
    .{ .title = "Category reading", .body = "The garden is Ctx = Free(G). G is the typed generating graph extracted from Org files, records, scripts, and category tables. Catface exposes representable Hom-style views for focused objects." },
    .{ .title = "Cockpit model", .body = "The top rail is a command/search prompt with quick lanes. The left pane is a ranked result stream. The right pane is a living object inspector: specialized cards, clickable path/id/org links, semantic Org headings/lists/tables/blocks, and a v-opened mini document viewer layered over the relation tree. Press/drag only arms/updates hover; release activates, so mouse actions are cancelable. Typing edits search in the left pane. RET enters the right-pane content mode for the selected object instead of rewriting the query. TAB opens the flat Vertico/orderless completion palette. In the right pane, n/p/j/k, arrows, mouse, and l navigate the relation tree, v opens the viewer, q closes it, h jumps back through relation targets, and the prompt is not mutated. C-n/C-p always move the main result stream." },
    .{ .title = "Performance model", .body = "Catface is event-driven: it redraws on input, resize, or cursor blink rather than repainting every tick. Query results are cached until the query changes. Palette completion searches into a retained candidate buffer rather than allocating a new list per keypress. The indexed runtime stores text postings, kind buckets, lane buckets, edge-kind buckets, and adjacency maps. Startup uses a persistent parsed-context snapshot under $CATFACE_CACHE_DIR, $XDG_CACHE_HOME/catface, or ~/.config/catface/cache. The terminal backend tracks dirty cells and flushes only the changed bounds. The footer exposes frame/query/flush nanosecond timings." },
    .{ .title = "Query language", .body = "Words fuzzy search through the index. :Kind filters. @namespace filters; @info jumps to context/info Org pages and documentation hubs. title:/path:/id:/preview:/tag:/function:/sig: are field filters. =term is exact-token indexed search. @hot/@blocked/@roots/@leaves/@orphans are structural lanes. @failures/@regressions/@performance/@coverage/@examples/@tutorials/@api/@cli/@cache/@diagnostics/@design are first-class context lanes. %edge-kind narrows by relation type. ?id and #id focus identity. plain type arrows like Int -> Int or a -> a search Function signatures; lhs -> rhs and lhs <- rhs search for morphisms when the sides are object queries. > follows outgoing edges. < follows incoming edges. ~ expands neighborhoods. proj projects into conceptual/taxonomy space." },
    .{ .title = "Information theory", .body = "Search is an information narrowing operation. The interface should prefer queries that reduce entropy while preserving explainable paths to source records." },
};

pub fn writeManual(buf: *std.array_list.Managed(u8)) !void {
    for (sections) |s| {
        try fmtbuf.print(buf, "# {s}\n\n{s}\n\n", .{ s.title, s.body });
    }
}

pub fn findSection(name: []const u8) ?Section {
    for (sections) |s| {
        if (std.mem.indexOf(u8, s.title, name) != null) return s;
    }
    return null;
}

pub const keyReference =
    \\Keys
    \\  Type        edit the live query
    \\  TAB         open command/completion palette
    \\  RET         enter right-pane content mode for the selected object
    \\  C-i/Alt-i   @info (context/info pages)
    \\  C-o/S-TAB   switch panes
    \\  C-n/C-p     move result selection, or palette candidates when palette is open
    \\  C-h c       describe-key-briefly
    \\  C-h k       help
    \\  C-c c       command palette
    \\  C-c d       change recursive root directory
    \\  C-c p/n     first/last palette candidate
    \\  Alt-t/n/e/s @todo @notes @tests @source
    \\  Alt-f/k/y/a @functions @contracts @quality @metadata
    \\  Alt-l/u     @links @bugs
    \\  Alt-w/m/c   @wisp @reader @codegen
    \\  Alt-v/x     append %verifies / %blocks
    \\  Alt-o/<     append outgoing > / incoming <
    \\  Alt-g/p     append neighborhood ~ / projection proj
    \\  Alt-b       history back
    \\  ?           help when query is empty
    \\  Ctrl-L/U    clear query
    \\  Esc/C-g     cancel overlay or quit from the base UI
    \\
;
pub const designPrinciples =
    \\Design principles
    \\  1. No generic-looking UI: hand-render the terminal cells.
    \\  2. No SQL grammar: use compact glyphs and filters.
    \\  3. Every visible object must link back to source.
    \\  4. Category operations must be visible as operations, not hidden filters.
    \\  5. Batch scripts can be Python; the live interface is Zig-first.
    \\
;

test "manual section" {
    try std.testing.expect(findSection("Category") != null);
}
