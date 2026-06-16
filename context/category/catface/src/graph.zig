const std = @import("std");
const model = @import("model.zig");

pub const Direction = enum { incoming, outgoing, both };

pub const Step = struct {
    edge_index: usize,
    object_index: usize,
};

pub fn adjacent(allocator: std.mem.Allocator, ctx: *const model.Context, object_index: usize, dir: Direction) !std.array_list.Managed(Step) {
    var out = std.array_list.Managed(Step).init(allocator);
    const id = ctx.objects.items[object_index].id;
    for (ctx.edges.items, 0..) |e, i| {
        switch (dir) {
            .outgoing => {
                if (std.mem.eql(u8, e.src, id)) {
                    if (ctx.findObject(e.dst)) |di| {
                        try out.append(.{ .edge_index = i, .object_index = di });
                    }
                }
            },
            .incoming => {
                if (std.mem.eql(u8, e.dst, id)) {
                    if (ctx.findObject(e.src)) |si| {
                        try out.append(.{ .edge_index = i, .object_index = si });
                    }
                }
            },
            .both => {
                if (std.mem.eql(u8, e.src, id)) {
                    if (ctx.findObject(e.dst)) |di| {
                        try out.append(.{ .edge_index = i, .object_index = di });
                    }
                }
                if (std.mem.eql(u8, e.dst, id)) {
                    if (ctx.findObject(e.src)) |si| {
                        try out.append(.{ .edge_index = i, .object_index = si });
                    }
                }
            },
        }
    }
    return out;
}

pub const BfsPath = struct {
    objects: std.array_list.Managed(usize),
    edges: std.array_list.Managed(usize),

    pub fn init(allocator: std.mem.Allocator) BfsPath {
        return .{ .objects = std.array_list.Managed(usize).init(allocator), .edges = std.array_list.Managed(usize).init(allocator) };
    }

    pub fn deinit(self: *BfsPath) void {
        self.objects.deinit();
        self.edges.deinit();
    }
};

pub fn shortestPath(allocator: std.mem.Allocator, ctx: *const model.Context, start: usize, goal: usize, max_depth: usize) !?BfsPath {
    const n = ctx.objects.items.len;
    var seen = try allocator.alloc(bool, n);
    defer allocator.free(seen);
    @memset(seen, false);
    var prev_obj = try allocator.alloc(?usize, n);
    defer allocator.free(prev_obj);
    var prev_edge = try allocator.alloc(?usize, n);
    defer allocator.free(prev_edge);
    for (prev_obj) |*p| {
        p.* = null;
    }
    for (prev_edge) |*p| {
        p.* = null;
    }

    var q = std.array_list.Managed(usize).init(allocator);
    defer q.deinit();
    var depth = std.array_list.Managed(usize).init(allocator);
    defer depth.deinit();
    try q.append(start);
    try depth.append(0);
    seen[start] = true;
    var head: usize = 0;
    while (head < q.items.len) : (head += 1) {
        const cur = q.items[head];
        const d = depth.items[head];
        if (cur == goal) break;
        if (d >= max_depth) continue;
        var adj = try adjacent(allocator, ctx, cur, .both);
        defer adj.deinit();
        for (adj.items) |s| {
            if (seen[s.object_index]) continue;
            seen[s.object_index] = true;
            prev_obj[s.object_index] = cur;
            prev_edge[s.object_index] = s.edge_index;
            try q.append(s.object_index);
            try depth.append(d + 1);
        }
    }
    if (!seen[goal]) return null;
    var path = BfsPath.init(allocator);
    var cur = goal;
    while (cur != start) {
        try path.objects.append(cur);
        if (prev_edge[cur]) |e| {
            try path.edges.append(e);
        }
        cur = prev_obj[cur] orelse break;
    }
    try path.objects.append(start);
    std.mem.reverse(usize, path.objects.items);
    std.mem.reverse(usize, path.edges.items);
    return path;
}

pub fn degree(ctx: *const model.Context, object_index: usize) struct { incoming: usize, outgoing: usize } {
    const id = ctx.objects.items[object_index].id;
    var inc: usize = 0;
    var out: usize = 0;
    for (ctx.edges.items) |e| {
        if (std.mem.eql(u8, e.src, id)) {
            out += 1;
        }
        if (std.mem.eql(u8, e.dst, id)) {
            inc += 1;
        }
    }
    return .{ .incoming = inc, .outgoing = out };
}

pub fn centralityScore(ctx: *const model.Context, object_index: usize) i32 {
    const d = degree(ctx, object_index);
    return @intCast(d.incoming * 3 + d.outgoing * 2);
}

test "degree empty" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "A", .kind = .concept });
    const d = degree(&ctx, 0);
    try std.testing.expect(d.incoming == 0 and d.outgoing == 0);
}
