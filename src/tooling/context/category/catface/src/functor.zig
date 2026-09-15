const std = @import("std");
const model = @import("model.zig");

pub const ProjectionKind = enum {
    object_kind,
    source_file,
    record_type,
    concept_closure,
    test_surface,
};

pub fn parseProjection(name: []const u8) ?ProjectionKind {
    if (std.mem.eql(u8, name, "kind") or std.mem.eql(u8, name, "K")) return .object_kind;
    if (std.mem.eql(u8, name, "file") or std.mem.eql(u8, name, "src")) return .source_file;
    if (std.mem.eql(u8, name, "record") or std.mem.eql(u8, name, "R")) return .record_type;
    if (std.mem.eql(u8, name, "concept") or std.mem.eql(u8, name, "proj")) return .concept_closure;
    if (std.mem.eql(u8, name, "test") or std.mem.eql(u8, name, "T")) return .test_surface;
    return null;
}

pub fn projectionLabel(k: ProjectionKind) []const u8 {
    return switch (k) {
        .object_kind => "F_kind : Ctx -> Kind",
        .source_file => "F_src : Ctx -> File",
        .record_type => "F_record : Ctx -> RecordType",
        .concept_closure => "F_concept : Ctx -> ConceptPreorder",
        .test_surface => "F_test : Ctx -> TestSurface",
    };
}

pub fn projectObject(ctx: *const model.Context, object_index: usize, k: ProjectionKind) []const u8 {
    const obj = ctx.objects.items[object_index];
    return switch (k) {
        .object_kind => model.Context.kindName(obj.kind),
        .source_file => obj.path,
        .record_type => if (obj.kind == .record) obj.title else "-",
        .concept_closure => nearestConcept(ctx, object_index) orelse "⊥",
        .test_surface => if (obj.kind == .test_kind or std.mem.indexOf(u8, obj.path, "test") != null) obj.path else "-",
    };
}

pub fn nearestConcept(ctx: *const model.Context, object_index: usize) ?[]const u8 {
    const id = ctx.objects.items[object_index].id;
    for (ctx.edges.items) |e| {
        if (!std.mem.eql(u8, e.src, id)) continue;
        if (e.kind == .classifies_as or e.kind == .forgets_to or e.kind == .refines) return e.dst;
    }
    if (ctx.objects.items[object_index].kind == .concept) return id;
    return null;
}

pub fn conceptClosure(allocator: std.mem.Allocator, ctx: *const model.Context, start: usize, limit: usize) !std.array_list.Managed(usize) {
    var out = std.array_list.Managed(usize).init(allocator);
    var seen = try allocator.alloc(bool, ctx.objects.items.len);
    defer allocator.free(seen);
    @memset(seen, false);
    try out.append(start);
    seen[start] = true;
    var head: usize = 0;
    while (head < out.items.len and out.items.len < limit) : (head += 1) {
        const id = ctx.objects.items[out.items[head]].id;
        for (ctx.edges.items) |e| {
            if (!std.mem.eql(u8, e.src, id)) continue;
            if (e.kind != .refines and e.kind != .classifies_as and e.kind != .forgets_to) continue;
            if (ctx.findObject(e.dst)) |dst| {
                if (!seen[dst]) {
                    seen[dst] = true;
                    try out.append(dst);
                }
            }
        }
    }
    return out;
}

pub const NaturalSquare = struct {
    a: []const u8,
    b: []const u8,
    fa: []const u8,
    fb: []const u8,
    arrow: []const u8,
    commutes: bool,
};

pub fn sampleNaturality(ctx: *const model.Context, edge_index: usize, projection: ProjectionKind) NaturalSquare {
    const e = ctx.edges.items[edge_index];
    const si = ctx.findObject(e.src) orelse return .{ .a = e.src, .b = e.dst, .fa = "?", .fb = "?", .arrow = model.Context.edgeName(e.kind), .commutes = false };
    const di = ctx.findObject(e.dst) orelse return .{ .a = e.src, .b = e.dst, .fa = "?", .fb = "?", .arrow = model.Context.edgeName(e.kind), .commutes = false };
    const fa = projectObject(ctx, si, projection);
    const fb = projectObject(ctx, di, projection);
    return .{ .a = e.src, .b = e.dst, .fa = fa, .fb = fb, .arrow = model.Context.edgeName(e.kind), .commutes = fa.len != 0 and fb.len != 0 };
}

test "projection parser" {
    try std.testing.expect(parseProjection("kind") == .object_kind);
    try std.testing.expect(parseProjection("proj") == .concept_closure);
}
