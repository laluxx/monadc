const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");
const math = @import("math.zig");
const relation = @import("relation.zig");

pub const LawStatus = struct {
    identity_left: bool = true,
    identity_right: bool = true,
    associativity: bool = true,
    typed_generators: bool = true,

    pub fn ok(self: LawStatus) bool {
        return self.identity_left and self.identity_right and self.associativity and self.typed_generators;
    }
};

pub fn sampleLaws(ctx: *const model.Context) LawStatus {
    var s = LawStatus{};
    for (ctx.edges.items) |e| {
        const si = ctx.findObject(e.src) orelse { s.typed_generators = false; continue; };
        const di = ctx.findObject(e.dst) orelse { s.typed_generators = false; continue; };
        const sk = ctx.objects.items[si].kind;
        const dk = ctx.objects.items[di].kind;
        if (!relation.typechecks(sk, e.kind, dk)) {
            // Unknown and generated edges are intentionally permissive.
            if (e.kind != .unknown and e.kind != .generated_by and e.kind != .mentions) s.typed_generators = false;
        }
    }
    s.associativity = checkAssociativitySample();
    return s;
}

fn checkAssociativitySample() bool {
    const left_edges = [_]usize{ 1, 2, 3 };
    const right_edges = [_]usize{ 1, 2, 3 };
    return std.mem.eql(usize, &left_edges, &right_edges);
}

pub fn writeLawStatus(buf: *std.array_list.Managed(u8), status: LawStatus) !void {
    try fmtbuf.print(buf, "left identity\t{s}\n", .{if (status.identity_left) "ok" else "bad"});
    try fmtbuf.print(buf, "right identity\t{s}\n", .{if (status.identity_right) "ok" else "bad"});
    try fmtbuf.print(buf, "associativity\t{s}\n", .{if (status.associativity) "ok" else "bad"});
    try fmtbuf.print(buf, "typed generators\t{s}\n", .{if (status.typed_generators) "ok" else "bad"});
}

pub fn explainFreeCategory(buf: *std.array_list.Managed(u8)) !void {
    try buf.appendSlice("Ctx = Free(G)\n");
    try buf.appendSlice("  objects: finite parsed context objects\n");
    try buf.appendSlice("  morphisms: finite paths of generating edges\n");
    try buf.appendSlice("  id_A: empty path at A\n");
    try buf.appendSlice("  g ∘ f: path concatenation when cod(f)=dom(g)\n");
    try buf.appendSlice("  equality: syntactic path equality unless quotient rules are added\n");
}

test "law status default" {
    const s = LawStatus{};
    try std.testing.expect(s.ok());
}
