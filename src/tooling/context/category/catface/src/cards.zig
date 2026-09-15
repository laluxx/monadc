const std = @import("std");
const model = @import("model.zig");

pub const CardKind = enum { object, edge, law, help };

pub const Card = struct {
    kind: CardKind,
    title: []const u8,
    subtitle: []const u8 = "",
    body: []const u8 = "",
};

pub fn writeHelpCard(buf: *std.array_list.Managed(u8), width: usize) !void {
    try border(buf, width, '╭', '╮');
    try row(buf, width, "Catface query language");
    try sep(buf, width);
    try row(buf, width, "word       fuzzy object/title/path/preview search");
    try row(buf, width, ":Record    filter object kind");
    try row(buf, width, "@wisp/@reader/@codegen focused namespaces");
    try row(buf, width, "@todo/@bugs/@notes/@info/@tests/@functions lanes");
    try row(buf, width, "@contracts/@quality/@metadata indexed metadata lanes");
    try row(buf, width, "?id #id    filter object identity");
    try row(buf, width, "%verifies  filter by edge kind");
    try row(buf, width, "Int -> Int search function signatures by type");
    try row(buf, width, "a -> b     relation search through arrows");
    try row(buf, width, "a <- b     reverse relation search");
    try row(buf, width, "> < ~      outgoing / incoming / neighborhood");
    try row(buf, width, "proj         project through taxonomy/functor edges");
    try row(buf, width, "(term) proj   human-friendly superset/concept projection");
    try border(buf, width, '╰', '╯');
}

pub fn writeLawCard(buf: *std.array_list.Managed(u8), width: usize) !void {
    try border(buf, width, '╭', '╮');
    try row(buf, width, "Ctx = Free(G)");
    try sep(buf, width);
    try row(buf, width, "objects: files, headings, records, scripts, concepts");
    try row(buf, width, "generators: contains, links, supports, verifies, refines");
    try row(buf, width, "identity: empty path id_A for every object A");
    try row(buf, width, "composition: path concatenation when codomain/domain match");
    try row(buf, width, "query > and < inspect representable Hom-views");
    try row(buf, width, "query a -> b searches generator arrows between sets");
    try border(buf, width, '╰', '╯');
}

fn border(buf: *std.array_list.Managed(u8), width: usize, left: u21, right: u21) !void {
    try cp(buf, left);
    var i: usize = 0;
    while (i + 2 < width) : (i += 1) try cp(buf, '─');
    try cp(buf, right);
    try buf.append('\n');
}

fn sep(buf: *std.array_list.Managed(u8), width: usize) !void {
    try cp(buf, '├');
    var i: usize = 0;
    while (i + 2 < width) : (i += 1) try cp(buf, '─');
    try cp(buf, '┤');
    try buf.append('\n');
}

fn row(buf: *std.array_list.Managed(u8), width: usize, text: []const u8) !void {
    try cp(buf, '│');
    try buf.append(' ');
    const inner = if (width > 4) width - 4 else 0;
    const n = @min(inner, text.len);
    try buf.appendSlice(text[0..n]);
    var i = n;
    while (i < inner) : (i += 1) try buf.append(' ');
    try buf.append(' ');
    try cp(buf, '│');
    try buf.append('\n');
}

fn cp(buf: *std.array_list.Managed(u8), codepoint: u21) !void {
    var tmp: [4]u8 = undefined;
    const n = try std.unicode.utf8Encode(codepoint, &tmp);
    try buf.appendSlice(tmp[0..n]);
}

test "help card renders" {
    var buf = std.array_list.Managed(u8).init(std.testing.allocator);
    defer buf.deinit();
    try writeHelpCard(&buf, 50);
    try std.testing.expect(std.mem.indexOf(u8, buf.items, "Catface") != null);
}
