const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");

pub const RelationClass = enum {
    structural,
    evidential,
    temporal,
    verification,
    taxonomy,
    projection,
    generated,
    lexical,
    unknown,
};

pub fn classify(k: model.EdgeKind) RelationClass {
    return switch (k) {
        .contains, .file_link, .id_link => .structural,
        .supports => .evidential,
        .supersedes => .temporal,
        .verifies, .blocks => .verification,
        .refines, .classifies_as => .taxonomy,
        .forgets_to => .projection,
        .generated_by => .generated,
        .mentions => .lexical,
        .unknown => .unknown,
    };
}

pub fn relationName(c: RelationClass) []const u8 {
    return switch (c) {
        .structural => "structural",
        .evidential => "evidential",
        .temporal => "temporal",
        .verification => "verification",
        .taxonomy => "taxonomy",
        .projection => "projection",
        .generated => "generated",
        .lexical => "lexical",
        .unknown => "unknown",
    };
}

pub fn isFunctorial(k: model.EdgeKind) bool {
    return k == .forgets_to or k == .classifies_as or k == .refines;
}

pub fn isEvidence(k: model.EdgeKind) bool {
    return k == .supports or k == .verifies or k == .blocks;
}

pub fn isNavigable(k: model.EdgeKind) bool {
    return k != .mentions and k != .unknown;
}

pub const Signature = struct {
    domain: []const model.ObjectKind,
    codomain: []const model.ObjectKind,
};

const any = [_]model.ObjectKind{ .file, .heading, .record, .script, .report, .concept, .test_kind, .source, .info, .todo, .done, .unknown };
const containers = [_]model.ObjectKind{ .file, .script, .report, .info };
const contained = [_]model.ObjectKind{ .heading, .record, .script, .report, .test_kind, .source, .info, .todo, .done };
const records = [_]model.ObjectKind{ .record };
const evidence_targets = [_]model.ObjectKind{ .record, .heading, .info, .todo, .done };
const concepts = [_]model.ObjectKind{ .concept };
const classify_domain = [_]model.ObjectKind{ .file, .heading, .record, .test_kind, .script, .report, .source, .info, .todo, .done };
const tests_or_records = [_]model.ObjectKind{ .test_kind, .record };

pub fn signature(k: model.EdgeKind) Signature {
    return switch (k) {
        .contains => .{ .domain = &containers, .codomain = &contained },
        .file_link => .{ .domain = &any, .codomain = &[_]model.ObjectKind{ .file, .script } },
        .id_link => .{ .domain = &any, .codomain = &[_]model.ObjectKind{ .heading, .concept } },
        .supports, .supersedes, .blocks => .{ .domain = &records, .codomain = &evidence_targets },
        .verifies => .{ .domain = &tests_or_records, .codomain = &evidence_targets },
        .refines => .{ .domain = &concepts, .codomain = &concepts },
        .classifies_as => .{ .domain = &classify_domain, .codomain = &concepts },
        .forgets_to => .{ .domain = &any, .codomain = &concepts },
        .generated_by, .mentions, .unknown => .{ .domain = &any, .codomain = &any },
    };
}

pub fn typechecks(src: model.ObjectKind, edge: model.EdgeKind, dst: model.ObjectKind) bool {
    const sig = signature(edge);
    return containsKind(sig.domain, src) and containsKind(sig.codomain, dst);
}

fn containsKind(xs: []const model.ObjectKind, k: model.ObjectKind) bool {
    for (xs) |x| {
        if (x == k) return true;
    }
    return false;
}

pub const RelationStats = struct {
    structural: usize = 0,
    evidential: usize = 0,
    temporal: usize = 0,
    verification: usize = 0,
    taxonomy: usize = 0,
    projection: usize = 0,
    generated: usize = 0,
    lexical: usize = 0,
    unknown: usize = 0,

    pub fn add(self: *RelationStats, c: RelationClass) void {
        switch (c) {
            .structural => self.structural += 1,
            .evidential => self.evidential += 1,
            .temporal => self.temporal += 1,
            .verification => self.verification += 1,
            .taxonomy => self.taxonomy += 1,
            .projection => self.projection += 1,
            .generated => self.generated += 1,
            .lexical => self.lexical += 1,
            .unknown => self.unknown += 1,
        }
    }
};

pub fn stats(ctx: *const model.Context) RelationStats {
    var s = RelationStats{};
    for (ctx.edges.items) |e| s.add(classify(e.kind));
    return s;
}

pub fn writeStats(buf: *std.array_list.Managed(u8), s: RelationStats) !void {
    try fmtbuf.print(buf, "structural\t{d}\n", .{s.structural});
    try fmtbuf.print(buf, "evidential\t{d}\n", .{s.evidential});
    try fmtbuf.print(buf, "temporal\t{d}\n", .{s.temporal});
    try fmtbuf.print(buf, "verification\t{d}\n", .{s.verification});
    try fmtbuf.print(buf, "taxonomy\t{d}\n", .{s.taxonomy});
    try fmtbuf.print(buf, "projection\t{d}\n", .{s.projection});
    try fmtbuf.print(buf, "generated\t{d}\n", .{s.generated});
    try fmtbuf.print(buf, "lexical\t{d}\n", .{s.lexical});
    try fmtbuf.print(buf, "unknown\t{d}\n", .{s.unknown});
}

test "relation classes" {
    try std.testing.expect(classify(.contains) == .structural);
    try std.testing.expect(isFunctorial(.refines));
    try std.testing.expect(typechecks(.concept, .refines, .concept));
}
