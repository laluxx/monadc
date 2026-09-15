const std = @import("std");
const model = @import("model.zig");

pub const PathMorphism = struct {
    src: []const u8,
    dst: []const u8,
    edges: []const usize,

    pub fn identity(id: []const u8) PathMorphism {
        return .{ .src = id, .dst = id, .edges = &.{} };
    }

    pub fn compose(allocator: std.mem.Allocator, a: PathMorphism, b: PathMorphism) !PathMorphism {
        if (!std.mem.eql(u8, a.dst, b.src)) return error.NotComposable;
        var edges = try allocator.alloc(usize, a.edges.len + b.edges.len);
        @memcpy(edges[0..a.edges.len], a.edges);
        @memcpy(edges[a.edges.len..], b.edges);
        return .{ .src = a.src, .dst = b.dst, .edges = edges };
    }
};

pub const CategoryReport = struct {
    unresolved_edges: usize = 0,
    bad_identity_samples: usize = 0,
    bad_composition_samples: usize = 0,
    composable_samples: usize = 0,

    pub fn ok(self: CategoryReport) bool {
        return self.unresolved_edges == 0 and self.bad_identity_samples == 0 and self.bad_composition_samples == 0;
    }
};

pub fn checkCategory(ctx: *const model.Context) CategoryReport {
    var r = CategoryReport{};
    for (ctx.edges.items) |e| {
        if (ctx.findObject(e.src) == null or ctx.findObject(e.dst) == null) {
            r.unresolved_edges += 1;
        }
    }
    var samples: usize = 0;
    for (ctx.edges.items) |a| {
        if (samples > 400) break;
        for (ctx.edges.items) |b| {
            if (std.mem.eql(u8, a.dst, b.src)) {
                samples += 1;
                r.composable_samples += 1;
                if (ctx.findObject(a.src) == null or ctx.findObject(b.dst) == null) {
                    r.bad_composition_samples += 1;
                }
                break;
            }
        }
    }
    return r;
}

pub const Distribution = struct {
    file: usize = 0,
    heading: usize = 0,
    record: usize = 0,
    script: usize = 0,
    report: usize = 0,
    concept: usize = 0,
    test_count: usize = 0,
    source: usize = 0,
    function_count: usize = 0,
    info: usize = 0,
    todo: usize = 0,
    done: usize = 0,
    unknown: usize = 0,

    pub fn add(self: *Distribution, k: model.ObjectKind) void {
        switch (k) {
            .file => { self.file += 1; },
            .heading => { self.heading += 1; },
            .record => { self.record += 1; },
            .script => { self.script += 1; },
            .report => { self.report += 1; },
            .concept => { self.concept += 1; },
            .test_kind => { self.test_count += 1; },
            .source => { self.source += 1; },
            .function_kind => { self.function_count += 1; },
            .info => { self.info += 1; },
            .todo => { self.todo += 1; },
            .done => { self.done += 1; },
            .unknown => { self.unknown += 1; },
        }
    }

    pub fn total(self: Distribution) usize {
        return self.file + self.heading + self.record + self.script + self.report + self.concept + self.test_count + self.source + self.function_count + self.info + self.todo + self.done + self.unknown;
    }
};

pub fn distribution(ctx: *const model.Context) Distribution {
    var d = Distribution{};
    for (ctx.objects.items) |o| d.add(o.kind);
    return d;
}

pub fn entropyBits(counts: []const usize) f64 {
    var sum: usize = 0;
    for (counts) |c| {
        sum += c;
    }
    if (sum == 0) return 0;
    var h: f64 = 0;
    for (counts) |c| {
        if (c == 0) continue;
        const p = @as(f64, @floatFromInt(c)) / @as(f64, @floatFromInt(sum));
        h -= p * std.math.log2(p);
    }
    return h;
}

pub fn objectEntropy(ctx: *const model.Context) f64 {
    const d = distribution(ctx);
    const counts = [_]usize{ d.file, d.heading, d.record, d.script, d.report, d.concept, d.test_count, d.source, d.info, d.todo, d.done, d.unknown };
    return entropyBits(&counts);
}

test "entropy" {
    const counts = [_]usize{ 1, 1 };
    try std.testing.expectApproxEqAbs(@as(f64, 1.0), entropyBits(&counts), 0.001);
}

test "identity construction" {
    const id = PathMorphism.identity("A");
    try std.testing.expect(std.mem.eql(u8, id.src, "A"));
    try std.testing.expect(id.edges.len == 0);
}
