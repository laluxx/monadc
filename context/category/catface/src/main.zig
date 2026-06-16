const std = @import("std");
const app = @import("app.zig");
const glyphs = @import("glyphs.zig");
const math = @import("math.zig");
const model = @import("model.zig");
const org = @import("org.zig");
const query = @import("query.zig");
const render = @import("render.zig");

const Help =
    \\catface — universal category/search explorer for MonadC
    \\
    \\usage:
    \\  catface [project-or-context-root]
    \\  catface --root <project-or-context-root>
    \\  catface --query <expr> [project-or-context-root]
    \\  catface --card <object-id> [project-or-context-root]
    \\  catface --check [project-or-context-root]
    \\  catface --dump-objects [project-or-context-root]
    \\  catface --help
    \\
    \\examples:
    \\  catface ../../../
    \\  catface --query 'typed define :Record @wisp' ../../../
    \\  catface --query '@tests TODO' ../../../
    \\  catface --query '@info category proj' ../../../
    \\  catface --card 'monadc.context.category.index.purpose' ../../../
    \\
    \\query language:
    \\  word      fuzzy-searches id/title/path/tags/preview
    \\  :Kind     filters kind, e.g. :Test :Info :Source :Todo :Done
    \\  @tests    tests namespace
    \\  @info     info/docs/context namespace
    \\  @source   compiler source/scripts namespace
    \\  ?id/#id   identity filter
    \\  > < ~     outgoing, incoming, neighborhood
    \\  proj      concept/taxonomy projection
    \\
    \\interactive keys:
    \\  C-n/C-p move, C-a/C-e query bounds, C-d delete
    \\  C-k kill line, M-d kill word, C-y yank, ? tutorial
    \\  Tab switches panes, Enter follows/focuses, mouse selects rows
    \\
;
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var args = try std.process.argsWithAllocator(allocator);
    defer args.deinit();
    _ = args.next();

    var mode: enum { tui, query, card, check, dump, help } = .tui;
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

    var ctx = try org.loadContext(allocator, root_path);
    defer ctx.deinit();

    switch (mode) {
        .query => try runQuery(allocator, &ctx, query_text orelse ""),
        .card => try runCard(allocator, &ctx, card_id orelse ""),
        .check => try runCheck(&ctx),
        .dump => try runDump(&ctx),
        .tui => try app.run(allocator, &ctx),
        .help => unreachable,
    }
}

fn runQuery(allocator: std.mem.Allocator, ctx: *model.Context, text: []const u8) !void {
    var res = try query.evaluate(allocator, ctx, text, .{ .limit = 40 });
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

fn runDump(ctx: *model.Context) !void {
    var out_buf: [8192]u8 = undefined;
    var out_file = std.fs.File.stdout().writer(&out_buf);
    const out = &out_file.interface;
    for (ctx.objects.items) |obj| {
        try out.print("{s}\t{s}\t{s}\t{s}:{d}\n", .{ @tagName(obj.kind), obj.id, obj.title, obj.path, obj.line });
    }
    try out.flush();
}
