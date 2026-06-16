const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");
const graph = @import("graph.zig");
const functor = @import("functor.zig");

pub const ObjectInspection = struct {
    object_index: usize,
    incoming: usize,
    outgoing: usize,
    centrality: i32,
    concept: ?[]const u8,
    source_file: []const u8,
};

pub fn inspectObject(ctx: *const model.Context, idx: usize) ObjectInspection {
    const d = graph.degree(ctx, idx);
    return .{
        .object_index = idx,
        .incoming = d.incoming,
        .outgoing = d.outgoing,
        .centrality = graph.centralityScore(ctx, idx),
        .concept = functor.nearestConcept(ctx, idx),
        .source_file = ctx.objects.items[idx].path,
    };
}

pub fn writeInspection(buf: *std.array_list.Managed(u8), ctx: *const model.Context, idx: usize) !void {
    const obj = ctx.objects.items[idx];
    const i = inspectObject(ctx, idx);
    try fmtbuf.print(buf, "object: {s}\nkind: {s}\nsource: {s}\nin: {d}\nout: {d}\ncentrality: {d}\nconcept: {s}\n", .{
        obj.id,
        model.Context.kindName(obj.kind),
        i.source_file,
        i.incoming,
        i.outgoing,
        i.centrality,
        i.concept orelse "⊥",
    });
}

pub const LinkChoice = struct {
    edge_index: usize,
    target_index: usize,
    label: []const u8,
};

pub fn rankedLinks(allocator: std.mem.Allocator, ctx: *const model.Context, idx: usize) !std.array_list.Managed(LinkChoice) {
    var out = std.array_list.Managed(LinkChoice).init(allocator);
    var adj = try graph.adjacent(allocator, ctx, idx, .both);
    defer adj.deinit();
    for (adj.items) |s| {
        const edge = ctx.edges.items[s.edge_index];
        try out.append(.{ .edge_index = s.edge_index, .target_index = s.object_index, .label = model.Context.edgeName(edge.kind) });
    }
    std.mem.sort(LinkChoice, out.items, ctx, lessChoice);
    return out;
}

fn lessChoice(ctx: *const model.Context, a: LinkChoice, b: LinkChoice) bool {
    const ca = graph.centralityScore(ctx, a.target_index);
    const cb = graph.centralityScore(ctx, b.target_index);
    return ca > cb;
}

test "inspection empty" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "A", .kind = .concept, .path = "x" });
    const i = inspectObject(&ctx, 0);
    try std.testing.expect(i.incoming == 0);
}
