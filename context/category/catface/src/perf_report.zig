const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");
const search_index = @import("index.zig");
const query = @import("query.zig");
const perf = @import("perf.zig");

const BenchQuery = struct {
    name: []const u8,
    expr: []const u8,
    rounds: usize = 32,
};

const default_queries = [_]BenchQuery{
    .{ .name = "todo_lane", .expr = "@todo" },
    .{ .name = "hot_lane", .expr = "@hot" },
    .{ .name = "info_lane", .expr = "@info" },
    .{ .name = "info_advanced_types", .expr = "@info advanced-types" },
    .{ .name = "functions_lane", .expr = "@functions" },
    .{ .name = "type_identity", .expr = "a -> a" },
    .{ .name = "type_int_to_int", .expr = "Int -> Int" },
    .{ .name = "exact_reader", .expr = "=reader" },
    .{ .name = "title_reader", .expr = "title:reader" },
    .{ .name = "verifies_reader", .expr = "@tests -> reader", .rounds = 16 },
    .{ .name = "reverse_reader", .expr = "reader <- @tests", .rounds = 16 },
    .{ .name = "verify_codegen", .expr = "%verifies @tests -> codegen", .rounds = 16 },
    .{ .name = "roots", .expr = "@roots" },
    .{ .name = "orphans", .expr = "@orphans" },
};

pub fn writeReport(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8), ctx: *const model.Context) !void {
    try fmtbuf.print(out, "{{\"catface_perf\":\"context\",\"objects\":{d},\"edges\":{d}}}\n", .{ ctx.objects.items.len, ctx.edges.items.len });
    const build_start = perf.nowNs();
    var idx = try search_index.SearchIndex.build(allocator, ctx);
    defer idx.deinit();
    const build_ns = perf.nanosSince(build_start);
    try fmtbuf.print(out, "{{\"catface_perf\":\"index_build\",\"objects\":{d},\"edges\":{d},\"terms\":{d},\"postings\":{d},\"ns\":{d}}}\n", .{ ctx.objects.items.len, ctx.edges.items.len, idx.text_index.terms.count(), idx.text_index.postingCount(), build_ns });
    for (default_queries) |b| try runQueryBench(allocator, out, ctx, &idx, b);
    try writeSyntheticReport(allocator, out);
}

fn runQueryBench(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8), ctx: *const model.Context, idx: *const search_index.SearchIndex, b: BenchQuery) !void {
    var matches: usize = 0;
    var rounds: usize = 0;
    const start = perf.nowNs();
    while (rounds < b.rounds) : (rounds += 1) {
        var res = try query.evaluateIndexed(allocator, ctx, idx, b.expr, .{ .limit = 64 });
        matches = res.items.len;
        res.deinit();
    }
    const total_ns = perf.nanosSince(start);
    const avg_ns = if (b.rounds == 0) 0 else @divTrunc(total_ns, @as(u64, @intCast(b.rounds)));
    try fmtbuf.print(out, "{{\"catface_perf\":\"query\",\"name\":\"{s}\",\"query\":\"{s}\",\"rounds\":{d},\"matches\":{d},\"total_ns\":{d},\"avg_ns\":{d}}}\n", .{ b.name, b.expr, b.rounds, matches, total_ns, avg_ns });
}

fn writeSyntheticReport(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8)) !void {
    var ctx = try syntheticContext(allocator, 960);
    defer ctx.deinit();
    const build_start = perf.nowNs();
    var idx = try search_index.SearchIndex.build(allocator, &ctx);
    defer idx.deinit();
    const build_ns = perf.nanosSince(build_start);
    try fmtbuf.print(out, "{{\"catface_perf\":\"synthetic_index_build\",\"objects\":{d},\"edges\":{d},\"terms\":{d},\"postings\":{d},\"ns\":{d}}}\n", .{ ctx.objects.items.len, ctx.edges.items.len, idx.text_index.terms.count(), idx.text_index.postingCount(), build_ns });
    const synthetic_queries = [_]BenchQuery{
        .{ .name = "synthetic_exact", .expr = "=needlefast", .rounds = 96 },
        .{ .name = "synthetic_word", .expr = "needlefast", .rounds = 96 },
        .{ .name = "synthetic_relation", .expr = "@tests -> needlefast", .rounds = 32 },
        .{ .name = "synthetic_blocked", .expr = "@blocked", .rounds = 96 },
        .{ .name = "synthetic_functions", .expr = "@functions", .rounds = 96 },
        .{ .name = "synthetic_type_arrow", .expr = "a -> a", .rounds = 96 },
        .{ .name = "synthetic_type_int", .expr = "Int -> Int", .rounds = 96 },
    };
    for (synthetic_queries) |b| try runQueryBench(allocator, out, &ctx, &idx, b);
}

fn syntheticContext(allocator: std.mem.Allocator, n: usize) !model.Context {
    var ctx = try model.Context.init(allocator, "synthetic");
    errdefer ctx.deinit();
    var i: usize = 0;
    while (i < n) : (i += 1) {
        var id_buf: [64]u8 = undefined;
        var title_buf: [96]u8 = undefined;
        const id = try std.fmt.bufPrint(&id_buf, "synthetic.{d}", .{i});
        const title = try std.fmt.bufPrint(&title_buf, "synthetic object {d}", .{i});
        const kind: model.ObjectKind = if (i % 17 == 0) .function_kind else if (i % 11 == 0) .test_kind else if (i % 13 == 0) .todo else if (i % 7 == 0) .source else .record;
        const preview = if (kind == .function_kind) if (i % 34 == 0) "function inc :: Int -> Int" else "function id :: a -> a" else if (i % 60 == 0) "needlefast structured performance target" else if (i % 37 == 0) "BLOCKED perf blocker surface" else "ordinary compiler context object";
        _ = try ctx.addObject(.{ .id = id, .kind = kind, .title = title, .path = "context/synthetic.org", .line = i + 1, .preview = preview });
    }
    i = 0;
    while (i + 1 < n) : (i += 3) {
        var edge_id_buf: [64]u8 = undefined;
        var src_buf: [64]u8 = undefined;
        var dst_buf: [64]u8 = undefined;
        const edge_id = try std.fmt.bufPrint(&edge_id_buf, "edge.{d}", .{i});
        const src = try std.fmt.bufPrint(&src_buf, "synthetic.{d}", .{i});
        const dst = try std.fmt.bufPrint(&dst_buf, "synthetic.{d}", .{(i + 1) % n});
        const kind: model.EdgeKind = if (i % 5 == 0) .verifies else if (i % 7 == 0) .blocks else .supports;
        _ = try ctx.addEdge(.{ .id = edge_id, .kind = kind, .src = src, .dst = dst, .path = "context/synthetic.org", .line = i + 1 });
    }
    return ctx;
}

test "structured performance report is JSON-lines shaped" {
    var ctx = try syntheticContext(std.testing.allocator, 120);
    defer ctx.deinit();
    var out = std.array_list.Managed(u8).init(std.testing.allocator);
    defer out.deinit();
    try writeReport(std.testing.allocator, &out, &ctx);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"catface_perf\":\"context\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"catface_perf\":\"query\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"avg_ns\"") != null);
}

const report_queries = [_]BenchQuery{
    .{ .name = "lane_todo", .expr = "@todo", .rounds = 8 },
    .{ .name = "lane_hot", .expr = "@hot", .rounds = 8 },
    .{ .name = "lane_notes", .expr = "@notes", .rounds = 8 },
    .{ .name = "lane_tests", .expr = "@tests", .rounds = 8 },
    .{ .name = "lane_info", .expr = "@info", .rounds = 8 },
    .{ .name = "lane_functions", .expr = "@functions", .rounds = 8 },
    .{ .name = "type_identity", .expr = "a -> a", .rounds = 8 },
    .{ .name = "info_advanced_types", .expr = "@info advanced-types", .rounds = 8 },
    .{ .name = "field_title_reader", .expr = "title:reader", .rounds = 8 },
    .{ .name = "exact_reader", .expr = "=reader", .rounds = 16 },
    .{ .name = "relation_tests_reader", .expr = "@tests -> reader", .rounds = 8 },
    .{ .name = "relation_reader_tests", .expr = "reader <- @tests", .rounds = 8 },
    .{ .name = "edge_verify_codegen", .expr = "%verifies @tests -> codegen", .rounds = 8 },
    .{ .name = "roots", .expr = "@roots", .rounds = 8 },
    .{ .name = "leaves", .expr = "@leaves", .rounds = 8 },
    .{ .name = "orphans", .expr = "@orphans", .rounds = 8 },
};

pub fn writeQueryReport(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8), ctx: *const model.Context) !void {
    const build_start = perf.nowNs();
    var idx = try search_index.SearchIndex.build(allocator, ctx);
    defer idx.deinit();
    const build_ns = perf.nanosSince(build_start);
    try fmtbuf.print(out, "{{\"catface_query_report\":\"index\",\"objects\":{d},\"edges\":{d},\"terms\":{d},\"postings\":{d},\"build_ns\":{d}}}\n", .{ ctx.objects.items.len, ctx.edges.items.len, idx.text_index.terms.count(), idx.text_index.postingCount(), build_ns });
    for (report_queries) |b| {
        try writeQueryRecord(allocator, out, ctx, &idx, b, "catalogue");
    }
}

pub fn writeTestReport(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8), ctx: *const model.Context) !void {
    const build_start = perf.nowNs();
    var idx = try search_index.SearchIndex.build(allocator, ctx);
    defer idx.deinit();
    const build_ns = perf.nanosSince(build_start);
    try fmtbuf.print(out, "{{\"catface_test\":\"index_build\",\"ok\":true,\"objects\":{d},\"edges\":{d},\"terms\":{d},\"postings\":{d},\"ns\":{d}}}\n", .{ ctx.objects.items.len, ctx.edges.items.len, idx.text_index.terms.count(), idx.text_index.postingCount(), build_ns });
    try writeInvariantRecord(out, "relation_arrow_token", query.findRelation("@tests -> reader") != null);
    try writeInvariantRecord(out, "reverse_arrow_token", query.findRelation("reader <- @tests") != null);
    try writeInvariantRecord(out, "unspaced_arrow_is_word", query.findRelation("Int->Int") == null);
    try writeInvariantRecord(out, "type_arrow_is_not_relation", query.findRelation("a -> a") == null);
    for (report_queries) |b| {
        try writeQueryRecord(allocator, out, ctx, &idx, b, "test_query");
    }
}

fn writeInvariantRecord(out: *std.array_list.Managed(u8), name: []const u8, ok: bool) !void {
    try fmtbuf.print(out, "{{\"catface_test\":\"invariant\",\"name\":\"{s}\",\"ok\":{s}}}\n", .{ name, if (ok) "true" else "false" });
}

fn writeQueryRecord(allocator: std.mem.Allocator, out: *std.array_list.Managed(u8), ctx: *const model.Context, idx: *const search_index.SearchIndex, b: BenchQuery, family: []const u8) !void {
    var matches: usize = 0;
    const start = perf.nowNs();
    var round: usize = 0;
    while (round < b.rounds) : (round += 1) {
        var res = try query.evaluateIndexed(allocator, ctx, idx, b.expr, .{ .limit = 128 });
        matches = res.items.len;
        res.deinit();
    }
    const total_ns = perf.nanosSince(start);
    const avg_ns = if (b.rounds == 0) 0 else @divTrunc(total_ns, @as(u64, @intCast(b.rounds)));
    try fmtbuf.print(out, "{{\"catface_test\":\"{s}\",\"name\":\"{s}\",\"query\":\"{s}\",\"rounds\":{d},\"matches\":{d},\"total_ns\":{d},\"avg_ns\":{d},\"ok\":true}}\n", .{ family, b.name, b.expr, b.rounds, matches, total_ns, avg_ns });
}

test "query and test reports are structured JSON-lines shaped" {
    var ctx = try syntheticContext(std.testing.allocator, 160);
    defer ctx.deinit();
    var out = std.array_list.Managed(u8).init(std.testing.allocator);
    defer out.deinit();
    try writeQueryReport(std.testing.allocator, &out, &ctx);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"catface_query_report\":\"index\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"catface_test\":\"catalogue\"") != null);
    out.clearRetainingCapacity();
    try writeTestReport(std.testing.allocator, &out, &ctx);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"catface_test\":\"invariant\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "\"avg_ns\"") != null);
}
