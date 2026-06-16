const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");

pub const Section = struct { title: []const u8, body: []const u8 };

pub const sections = [_]Section{
    .{ .title = "Why Catface exists", .body = "Catface makes the context category navigable as a living object. It is designed for fuzzy search, fast keyboard navigation, and direct category operations rather than menus." },
    .{ .title = "Category reading", .body = "The garden is Ctx = Free(G). G is the typed generating graph extracted from Org files, records, scripts, and category tables. Catface exposes representable Hom-style views for focused objects." },
    .{ .title = "Cockpit model", .body = "The top rail is a command/search prompt with quick lanes. The left pane is a ranked result stream. The right pane keeps selected object text on top and a bottom-anchored collapsible relation tree below it. Typing edits search in the left pane. In the right pane, n/p/j/k, arrows, mouse, and RET/l navigate the relation tree, h jumps back through relation targets, and the prompt is not mutated. C-n/C-p always move the main result stream." },
    .{ .title = "Performance model", .body = "Catface is event-driven: it redraws on input, resize, or cursor blink rather than repainting every tick. Query results are cached until the query changes. The indexed runtime stores text postings, kind buckets, lane buckets, edge-kind buckets, and adjacency maps. Startup uses a persistent parsed-context snapshot under $CATFACE_CACHE_DIR, $XDG_CACHE_HOME/catface, or ~/.config/catface/cache. The terminal backend tracks dirty cells and flushes only the changed bounds. The footer exposes frame/query/flush nanosecond timings." },
    .{ .title = "Query language", .body = "Words fuzzy search through the index. :Kind filters. @namespace filters; @info jumps to context/info Org pages and documentation hubs. title:/path:/id:/preview:/tag:/function:/sig: are field filters. =term is exact-token indexed search. @hot/@blocked/@roots/@leaves/@orphans are structural lanes. %edge-kind narrows by relation type. ?id and #id focus identity. plain type arrows like a -> a search Function signatures; lhs -> rhs and lhs <- rhs search for morphisms when the sides are object queries. > follows outgoing edges. < follows incoming edges. ~ expands neighborhoods. proj projects into conceptual/taxonomy space." },
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
    \\  Type      edit the live query
    \\  Up/Down   move selection
    \\  C-n/C-p   move selection
    \\  Enter     follow focused object
    \\  Alt-t     @todo
    \\  Alt-n     @notes
    \\  Alt-e     @tests
    \\  Alt-s     @source
    \\  Alt-i     @info (context/info pages)
    \\  Alt-u     @bugs
    \\  Alt-w     @wisp
    \\  Alt-m     @reader
    \\  Alt-c     @codegen
    \\  Alt-f     @functions
    \\  Alt-v     append %verifies
    \\  Alt-x     append %blocks
    \\  Alt-o     append outgoing >
    \\  Alt-<     append incoming <
    \\  Alt-g     append neighborhood ~
    \\  Alt-p     append projection proj
    \\  Alt-b     history back
    \\  ?         help when query is empty
    \\  Ctrl-L/U  clear query
    \\  Esc/C-c   quit
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
