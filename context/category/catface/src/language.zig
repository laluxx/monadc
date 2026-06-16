const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");

/// Catface Query Language (CQL) is deliberately keyboard-small.
/// It is not SQL-shaped. It is a live stack/filter language for terminal search.
///
/// Grammar sketch:
///   expr     := relation | item*
///   relation := expr '->' expr | expr '<-' expr
///   item     := atom | filter | op | group
///   group    := '(' expr ')'
///   atom     := bare-text
///   filter   := ':' kind | '@' lane | '?' identity | '#' identity | '%' relation | field ':' term
///   op       := '>' | '<' | '~' | 'proj' | '!' | '⊔' | '∩' | '∘'
///   relation operators use spaces: lhs -> rhs, lhs <- rhs
///
/// Semantics are intentionally set-oriented:
///   words narrow by fuzzy predicate
///   filters narrow by typed predicate
///   arrows expand through generating morphisms
///   relation queries match morphisms between two object sets
///   proj projects through taxonomy/functor-like edges
pub const Op = enum {
    outgoing,
    incoming,
    neighborhood,
    projection,
    reset,
    union_next,
    intersect_next,
    compose,
};

pub const AtomKind = enum {
    word,
    kind,
    path,
    identity,
    relation,
    group_open,
    group_close,
    op,
};

pub const Atom = struct {
    kind: AtomKind,
    text: []const u8,
    op: ?Op = null,

    pub fn isFilter(self: Atom) bool {
        return self.kind == .kind or self.kind == .path or self.kind == .identity or self.kind == .relation;
    }
};

pub const Program = struct {
    allocator: std.mem.Allocator,
    atoms: []Atom,

    pub fn deinit(self: *Program) void {
        self.allocator.free(self.atoms);
    }
};

pub fn parse(allocator: std.mem.Allocator, source: []const u8) !Program {
    var atoms = std.array_list.Managed(Atom).init(allocator);
    var i: usize = 0;
    while (i < source.len) {
        i = skipSpace(source, i);
        if (i >= source.len) break;
        const start = i;
        if (std.mem.startsWith(u8, source[i..], "proj") and (i + 4 == source.len or isTerminator(source, i + 4))) {
            try atoms.append(.{ .kind = .op, .text = source[i..i+4], .op = .projection });
            i += 4;
            continue;
        }
        const cp_len = utf8Len(source[i]) catch 1;
        const slice = source[i..@min(source.len, i + cp_len)];
        if (source[i] == '(') { try atoms.append(.{ .kind = .group_open, .text = source[i..i+1] }); i += 1; continue; }
        if (source[i] == ')') { try atoms.append(.{ .kind = .group_close, .text = source[i..i+1] }); i += 1; continue; }
        if (opFromSlice(slice)) |op| { try atoms.append(.{ .kind = .op, .text = slice, .op = op }); i += cp_len; continue; }
        if (source[i] == ':' or source[i] == '@' or source[i] == '?' or source[i] == '#' or source[i] == '%') {
            const prefix = source[i];
            i += 1;
            const body_start = i;
            while (i < source.len and !isTerminator(source, i)) i += utf8Len(source[i]) catch 1;
            const k: AtomKind = switch (prefix) { ':' => .kind, '@' => .path, '?' => .identity, '#' => .identity, '%' => .relation, else => .word };
            try atoms.append(.{ .kind = k, .text = source[body_start..i] });
            continue;
        }
        while (i < source.len and !isTerminator(source, i)) i += utf8Len(source[i]) catch 1;
        try atoms.append(.{ .kind = .word, .text = source[start..i] });
    }
    return .{ .allocator = allocator, .atoms = try atoms.toOwnedSlice() };
}

pub fn explain(allocator: std.mem.Allocator, program: Program) ![]const u8 {
    var out = std.array_list.Managed(u8).init(allocator);
    for (program.atoms, 0..) |a, i| {
        if (i != 0) try out.appendSlice(" → ");
        switch (a.kind) {
            .word => try fmtbuf.print(&out, "fuzzy({s})", .{a.text}),
            .kind => try fmtbuf.print(&out, "kind={s}", .{a.text}),
            .path => try fmtbuf.print(&out, "path∋{s}", .{a.text}),
            .identity => try fmtbuf.print(&out, "id∋{s}", .{a.text}),
            .relation => try fmtbuf.print(&out, "edge={s}", .{a.text}),
            .group_open => try out.appendSlice("("),
            .group_close => try out.appendSlice(")"),
            .op => try fmtbuf.print(&out, "op.{s}", .{@tagName(a.op.?)}),
        }
    }
    return try out.toOwnedSlice();
}

pub fn formatCheatsheet(buf: *std.array_list.Managed(u8)) !void {
    try buf.appendSlice(
        \\Catface query mini-language
        \\  word        fuzzy search
        \\  :Record     kind filter
        \\  @wisp       namespace/lane filter
        \\  @info       context/info Org reference pages
        \\  @functions  first-class core/prelude functions
        \\  title:x     field filter: title/path/id/preview/tag/function/sig
        \\  path:context/info  info-page path filter
        \\  @hot        structural/triage lane
        \\  ?id #id     identity filter
        \\  %supports   edge-kind filter
        \\  a -> a      type-signature search when both sides are type-like
        \\  a -> b      objects connected by morphisms a→b
        \\  a <- b      objects connected by morphisms b→a
        \\  > < ~       outgoing / incoming / neighborhood
        \\  proj        taxonomy/functor projection
        \\  !           reset to all objects
        \\  ⊔ ∩ ∘       planned union/intersection/composition glyphs
        \\
    );
}

fn skipSpace(s: []const u8, start_index: usize) usize {
    var i = start_index;
    while (i < s.len and std.ascii.isWhitespace(s[i])) i += 1;
    return i;
}

fn isTerminator(s: []const u8, i: usize) bool {
    if (i >= s.len) return true;
    if (std.ascii.isWhitespace(s[i]) or s[i] == '(' or s[i] == ')') return true;
    const l = utf8Len(s[i]) catch 1;
    return opFromSlice(s[i..@min(s.len, i + l)]) != null;
}

fn opFromSlice(slice: []const u8) ?Op {
    if (std.mem.eql(u8, slice, ">")) return .outgoing;
    if (std.mem.eql(u8, slice, "<")) return .incoming;
    if (std.mem.eql(u8, slice, "~")) return .neighborhood;
    if (std.mem.eql(u8, slice, "!")) return .reset;
    if (std.mem.eql(u8, slice, "⊔")) return .union_next;
    if (std.mem.eql(u8, slice, "∩")) return .intersect_next;
    if (std.mem.eql(u8, slice, "∘")) return .compose;
    return null;
}

fn utf8Len(first: u8) !usize {
    if (first < 0x80) return 1;
    if ((first & 0xe0) == 0xc0) return 2;
    if ((first & 0xf0) == 0xe0) return 3;
    if ((first & 0xf8) == 0xf0) return 4;
    return error.InvalidUtf8;
}

pub const Example = struct { query: []const u8, meaning: []const u8 };

pub const examples = [_]Example{
    .{ .query = "typed define :Record @wisp", .meaning = "records about typed define in Wisp files" },
    .{ .query = "reader :Concept >", .meaning = "objects generated out of reader concept" },
    .{ .query = "?monadc.context.category.index.purpose ~", .meaning = "neighborhood of a known heading" },
    .{ .query = "superset proj", .meaning = "fuzzy superset term then concept projection" },
    .{ .query = "script :Script @catface", .meaning = "Catface source scripts" },
    .{ .query = "@todo -> @source", .meaning = "open work items connected to implementation surfaces" },
    .{ .query = "title:reader @hot", .meaning = "indexed title field search inside urgent work" },
    .{ .query = "=reader @source", .meaning = "exact token search, then source lane filter" },
    .{ .query = "@roots -> @leaves", .meaning = "shape diagnostic over category roots and leaves" },
    .{ .query = "@tests -> reader", .meaning = "objects in arrows from tests to reader matches" },
    .{ .query = "reader <- @tests", .meaning = "reverse question: tests pointing at reader objects" },
    .{ .query = "%verifies @tests -> codegen", .meaning = "verified codegen targets and participating tests" },
};

test "language parse" {
    var p = try parse(std.testing.allocator, "typed :Record @wisp %supports > proj");
    defer p.deinit();
    try std.testing.expect(p.atoms.len == 6);
    try std.testing.expect(p.atoms[1].kind == .kind);
    try std.testing.expect(p.atoms[3].kind == .relation);
    try std.testing.expect(p.atoms[4].op.? == .outgoing);
}
