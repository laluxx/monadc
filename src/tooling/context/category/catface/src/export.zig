const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");
const math = @import("math.zig");

pub fn writeJsonSummary(buf: *std.array_list.Managed(u8), ctx: *const model.Context) !void {
    try fmtbuf.print(buf, "{{\n  \"objects\": {d},\n  \"edges\": {d},\n  \"entropy_bits\": {d:.4},\n  \"kinds\": {{\n", .{ ctx.objects.items.len, ctx.edges.items.len, math.objectEntropy(ctx) });
    const d = math.distribution(ctx);
    try fmtbuf.print(buf, "    \"file\": {d},\n    \"heading\": {d},\n    \"record\": {d},\n    \"script\": {d},\n    \"report\": {d},\n    \"concept\": {d},\n    \"test\": {d},\n    \"source\": {d},\n    \"unknown\": {d}\n", .{ d.file, d.heading, d.record, d.script, d.report, d.concept, d.test_count, d.source, d.unknown });
    try buf.appendSlice("  }\n}\n");
}

pub fn writeOrgLink(buf: *std.array_list.Managed(u8), ctx: *const model.Context, object_index: usize) !void {
    const obj = ctx.objects.items[object_index];
    if (obj.kind == .heading or obj.kind == .concept) {
        try fmtbuf.print(buf, "[[id:{s}][{s}]]", .{ obj.id, if (obj.title.len != 0) obj.title else obj.id });
    } else if (obj.path.len != 0) {
        try fmtbuf.print(buf, "[[file:{s}::{d}][{s}]]", .{ obj.path, obj.line, if (obj.title.len != 0) obj.title else obj.id });
    } else {
        try buf.appendSlice(obj.id);
    }
}

pub fn writeMarkdownObject(buf: *std.array_list.Managed(u8), ctx: *const model.Context, object_index: usize) !void {
    const obj = ctx.objects.items[object_index];
    try fmtbuf.print(buf, "### `{s}`\n\n", .{obj.id});
    try fmtbuf.print(buf, "- kind: `{s}`\n", .{model.Context.kindName(obj.kind)});
    try fmtbuf.print(buf, "- source: `{s}:{d}`\n", .{ obj.path, obj.line });
    if (obj.title.len != 0) try fmtbuf.print(buf, "- title: {s}\n", .{obj.title});
    if (obj.preview.len != 0) try fmtbuf.print(buf, "\n{s}\n", .{obj.preview});
}

test "json summary text" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var buf = std.array_list.Managed(u8).init(std.testing.allocator);
    defer buf.deinit();
    try writeJsonSummary(&buf, &ctx);
    try std.testing.expect(std.mem.indexOf(u8, buf.items, "objects") != null);
}
