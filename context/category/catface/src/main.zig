const std = @import("std");
const app = @import("app.zig");
const glyphs = @import("glyphs.zig");
const math = @import("math.zig");
const model = @import("model.zig");
const org = @import("org.zig");
const query = @import("query.zig");
const render = @import("render.zig");
const version = @import("version.zig");
const search_index = @import("index.zig");
const perf_report = @import("perf_report.zig");
const file_cache = @import("file_cache.zig");
const context_cache = @import("context_cache.zig");
const perf = @import("perf.zig");

const Help = "catface v" ++ version.version ++ " " ++ version.codename ++ " — universal category/search explorer for MonadC\n" ++
    \\
    \\usage:
    \\  catface [project-or-context-root]
    \\  catface --root <project-or-context-root>
    \\  catface --query <expr> [project-or-context-root]
    \\  catface --card <object-id> [project-or-context-root]
    \\  catface --check [project-or-context-root]
    \\  catface --dump-objects [project-or-context-root]
    \\  catface --perf [project-or-context-root]
    \\  catface --query-report [project-or-context-root]
    \\  catface --test-report [project-or-context-root]
    \\  catface --cache-report [project-or-context-root]
    \\  catface --help
    \\
    \\examples:
    \\  catface ../../../
    \\  catface --query '@todo' ../../../
    \\  catface --query '@bugs' ../../../
    \\  catface --query '@notes reader' ../../../
    \\  catface --query 'title:reader path:src' ../../../
    \\  catface --query '=reader @source' ../../../
    \\  catface --query '@functions id' ../../../
    \\  catface --query 'a -> a' ../../../
    \\  catface --query '@tests -> reader' ../../../
    \\  catface --query 'reader <- @tests' ../../../
    \\  catface --query '%verifies @tests -> codegen' ../../../
    \\  catface --card 'monadc.context.category.index.purpose' ../../../
    \\  catface --perf ../../../
    \\  catface --query-report ../../../
    \\  catface --test-report ../../../
    \\  catface --cache-report ../../../
    \\
    \\query language:
    \\  word      fuzzy-searches id/title/path/tags/preview
    \\  =term     exact normalized-token search
    \\  :Kind     filters kind, e.g. :Test :Info :Source :Todo :Done :Record
    \\  @todo     TODO queue
    \\  @bugs     BUG/FAIL/error surface
    \\  @notes    notes/context/records namespace
    \\  @tests    tests namespace
    \\  @info     info/docs context namespace
    \\  @source   compiler source/scripts namespace
    \\  @functions first-class functions from core/prelude/source
    \\  a -> a    type-signature search when both sides are type-like
    \\  @reader/@wisp/@codegen focused compiler surfaces; @reports/@fix for generated reports and repair notes
    \\  ?id/#id   identity filter
    \\  %verifies edge-kind filter; also %supports %blocks %refines
    \\  lhs -> rhs relation search through generating morphisms when sides are object queries
    \\  lhs <- rhs reverse relation search
    \\  > < ~     outgoing, incoming, neighborhood
    \\  proj      concept/taxonomy projection
    \\
    \\interactive keys:
    \\  Type in left pane; right pane captures tree navigation
    \\  C-n/C-p always move results; arrows move active pane
    \\  C-k kill line, M-d kill word, C-y yank, C-l clear, ? tutorial
    \\  Alt-t TODO, Alt-n notes, Alt-e tests, Alt-s source, Alt-i info
    \\  Alt-u bugs, Alt-w wisp, Alt-m reader, Alt-c codegen, Alt-f functions
    \\  Alt-v %verifies, Alt-x %blocks, Alt-o >, Alt-< <, Alt-g ~, Alt-p proj, Alt-b back
    \\  Tab switches panes; in right pane n/p/j/k move relation tree, RET/l opens, h backs up
    \\
;
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var args = try std.process.argsWithAllocator(allocator);
    defer args.deinit();
    _ = args.next();

    var mode: enum { tui, query, card, check, dump, perf, query_report, test_report, cache_report, help } = .tui;
    var query_text: ?[]const u8 = null;
    var card_id: ?[]const u8 = null;
    var root_arg: ?[]const u8 = null;

    while (args.next()) |arg| {
        if (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h")) {
            mode = .help;
        } else if (std.mem.eql(u8, arg, "--root")) {
            root_arg = args.next() orelse return error.MissingRoot;
        } else if (std.mem.eql(u8, arg, "--query") or std.mem.eql(u8, arg, "-q")) {
            mode = .query;
            query_text = args.next() orelse return error.MissingQuery;
        } else if (std.mem.eql(u8, arg, "--card")) {
            mode = .card;
            card_id = args.next() orelse return error.MissingObjectId;
        } else if (std.mem.eql(u8, arg, "--check")) {
            mode = .check;
        } else if (std.mem.eql(u8, arg, "--dump-objects")) {
            mode = .dump;
        } else if (std.mem.eql(u8, arg, "--perf")) {
            mode = .perf;
        } else if (std.mem.eql(u8, arg, "--query-report")) {
            mode = .query_report;
        } else if (std.mem.eql(u8, arg, "--test-report")) {
            mode = .test_report;
        } else if (std.mem.eql(u8, arg, "--cache-report")) {
            mode = .cache_report;
        } else {
            root_arg = arg;
        }
    }

    if (mode == .help) {
        var out_buf: [4096]u8 = undefined;
        var out_file = std.fs.File.stdout().writer(&out_buf);
        const out = &out_file.interface;
        try out.writeAll(Help);
        try out.flush();
        return;
    }

    const cwd = try std.process.getCwdAlloc(allocator);
    defer allocator.free(cwd);
    const root_path = root_arg orelse cwd;

    if (mode == .cache_report) {
        try runCacheReport(allocator, root_path);
        return;
    }

    var ctx = try org.loadContext(allocator, root_path);
    defer ctx.deinit();

    switch (mode) {
        .query => try runQuery(allocator, &ctx, query_text orelse ""),
        .card => try runCard(allocator, &ctx, card_id orelse ""),
        .check => try runCheck(&ctx),
        .dump => try runDump(&ctx),
        .perf => try runPerf(allocator, &ctx),
        .query_report => try runQueryReport(allocator, &ctx),
        .test_report => try runTestReport(allocator, &ctx),
        .cache_report => unreachable,
        .tui => try app.run(allocator, &ctx),
        .help => unreachable,
    }
}

fn runQuery(allocator: std.mem.Allocator, ctx: *model.Context, text: []const u8) !void {
    var search = try search_index.SearchIndex.build(allocator, ctx);
    defer search.deinit();
    var res = try query.evaluateIndexed(allocator, ctx, &search, text, .{ .limit = 40 });
    defer res.deinit();
    var out_buf: [8192]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    for (res.items, 0..) |item, i| {
        const obj = ctx.objects.items[item.object_index];
        try out.print("{d: >2}. {s} {s}  score={d}\n", .{ i + 1, glyphs.kind(obj.kind), obj.id, item.score });
        if (obj.title.len != 0) try out.print("    {s}\n", .{obj.title});
        if (obj.path.len != 0) try out.print("    @{s}:{d}\n", .{ obj.path, obj.line });
    }
    try out.flush();
}

fn runCard(allocator: std.mem.Allocator, ctx: *model.Context, id: []const u8) !void {
    const idx = ctx.findObject(id) orelse return error.ObjectNotFound;
    var out_buf: [16384]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    var scratch = std.array_list.Managed(u8).init(allocator);
    defer scratch.deinit();
    try render.writeObjectCard(&scratch, ctx, idx, 96);
    try out.writeAll(scratch.items);
    try out.flush();
}

fn runCheck(ctx: *model.Context) !void {
    var out_buf: [4096]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    const report = math.checkCategory(ctx);
    try out.print("objects: {d}\nedges: {d}\nidentities: {d}\nunresolved: {d}\ncomposable samples: {d}\nstatus: {s}\n", .{
        ctx.objects.items.len,
        ctx.edges.items.len,
        ctx.objects.items.len,
        report.unresolved_edges,
        report.composable_samples,
        if (report.ok()) "OK" else "BROKEN",
    });
    try out.flush();
}

fn runCacheReport(allocator: std.mem.Allocator, root_path: []const u8) !void {
    var out_buf: [8192]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    const scan_start = perf.nowNs();
    var files = try file_cache.Cache.build(allocator, root_path);
    defer files.deinit();
    const scan_ns = perf.nanosSince(scan_start);
    const sig = context_cache.signature(&files);
    const path = try context_cache.cachePath(allocator, root_path);
    defer allocator.free(path);
    try out.print("{{\"catface_cache\":\"manifest\",\"files\":{d},\"dirs_scanned\":{d},\"dirs_skipped\":{d},\"signature\":\"{x}\",\"scan_ns\":{d},\"path\":\"{s}\"}}\n", .{ files.files.items.len, files.directories_scanned, files.directories_skipped, sig, scan_ns, path });
    const load_start = perf.nowNs();
    if (try context_cache.load(allocator, root_path, sig)) |ctx| {
        var cached = ctx;
        defer cached.deinit();
        try out.print("{{\"catface_cache\":\"load\",\"hit\":true,\"objects\":{d},\"edges\":{d},\"ns\":{d}}}\n", .{ cached.objects.items.len, cached.edges.items.len, perf.nanosSince(load_start) });
    } else {
        var ctx = try org.loadContext(allocator, root_path);
        defer ctx.deinit();
        try out.print("{{\"catface_cache\":\"load\",\"hit\":false,\"objects\":{d},\"edges\":{d},\"ns\":{d},\"saved\":true}}\n", .{ ctx.objects.items.len, ctx.edges.items.len, perf.nanosSince(load_start) });
    }
    try out.flush();
}

fn runPerf(allocator: std.mem.Allocator, ctx: *model.Context) !void {
    var out_buf_list = std.array_list.Managed(u8).init(allocator);
    defer out_buf_list.deinit();
    try perf_report.writeReport(allocator, &out_buf_list, ctx);
    try writeStdout(out_buf_list.items);
}

fn runQueryReport(allocator: std.mem.Allocator, ctx: *model.Context) !void {
    var out_buf_list = std.array_list.Managed(u8).init(allocator);
    defer out_buf_list.deinit();
    try perf_report.writeQueryReport(allocator, &out_buf_list, ctx);
    try writeStdout(out_buf_list.items);
}

fn runTestReport(allocator: std.mem.Allocator, ctx: *model.Context) !void {
    var out_buf_list = std.array_list.Managed(u8).init(allocator);
    defer out_buf_list.deinit();
    try perf_report.writeTestReport(allocator, &out_buf_list, ctx);
    try writeStdout(out_buf_list.items);
}

fn writeStdout(bytes: []const u8) !void {
    var out_buf: [32768]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    try out.writeAll(bytes);
    try out.flush();
}

fn runDump(ctx: *model.Context) !void {
    var out_buf: [8192]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    for (ctx.objects.items) |obj| {
        try out.print("{s}\t{s}\t{s}\t{s}:{d}\n", .{ @tagName(obj.kind), obj.id, obj.title, obj.path, obj.line });
    }
    try out.flush();
}
