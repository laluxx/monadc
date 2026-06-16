const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");

pub const Section = struct { title: []const u8, body: []const u8 };

pub const sections = [_]Section{
    .{ .title = "Why Catface exists", .body = "Catface makes the context category navigable as a living object. It is designed for fuzzy search, fast keyboard navigation, and direct category operations rather than menus." },
    .{ .title = "Category reading", .body = "The garden is Ctx = Free(G). G is the typed generating graph extracted from Org files, records, scripts, and category tables. Catface exposes representable Hom-style views for focused objects." },
    .{ .title = "Dual pane model", .body = "The left pane is a query result stack. The right pane is the focused object card. Tab toggles focus. Enter rewrites the query to the selected identity." },
    .{ .title = "Query language", .body = "Words fuzzy search. :Kind filters. @path filters. ?id and #id focus identity. > follows outgoing edges. < follows incoming edges. ~ expands neighborhoods. proj projects into conceptual/taxonomy space." },
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
    \\  Tab       switch active pane
    \\  Enter     follow focused object
    \\  j/k       move selection
    \\  o/i       append outgoing/incoming operation
    \\  n         append neighborhood operation
    \\  p         append projection operation
    \\  b         history back
    \\  ?         help
    \\  Ctrl-U    clear query
    \\  q/Esc     quit
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
