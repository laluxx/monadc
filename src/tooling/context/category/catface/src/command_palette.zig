const std = @import("std");
const model = @import("model.zig");
const fuzzy = @import("fuzzy.zig");
const query = @import("query.zig");
const search_index = @import("index.zig");

pub const Action = enum {
    quick_todo,
    quick_notes,
    quick_tests,
    quick_source,
    quick_info,
    quick_bugs,
    quick_failures,
    quick_regressions,
    quick_performance,
    quick_coverage,
    quick_examples,
    quick_tutorials,
    quick_api,
    quick_cli,
    quick_cache,
    quick_diagnostics,
    quick_design,
    quick_hot,
    quick_blocked,
    quick_roots,
    quick_leaves,
    quick_orphans,
    quick_wisp,
    quick_reader,
    quick_codegen,
    quick_functions,
    quick_contracts,
    quick_quality,
    quick_metadata,
    quick_links,
    quick_tables,
    quick_docs,
    change_directory,
    query_verifies,
    query_blocks,
    query_outgoing,
    query_incoming,
    query_neighborhood,
    query_projection,
    copy_org_link,
    copy_object_id,
    export_card,
    run_check,
    open_source,
    show_help,
};

pub const CommandItem = struct {
    action: Action,
    name: []const u8,
    description: []const u8,
    query_insert: []const u8 = "",
};

pub const commands = [_]CommandItem{
    .{ .action = .quick_todo, .name = "todo", .description = "jump to TODO work queue", .query_insert = "@todo" },
    .{ .action = .quick_notes, .name = "notes", .description = "jump to notes/context records", .query_insert = "@notes" },
    .{ .action = .quick_tests, .name = "tests", .description = "jump to test corpus", .query_insert = "@tests" },
    .{ .action = .quick_source, .name = "source", .description = "jump to compiler source/scripts", .query_insert = "@source" },
    .{ .action = .quick_info, .name = "info", .description = "jump to info/docs", .query_insert = "@info" },
    .{ .action = .quick_bugs, .name = "bugs", .description = "jump to bug/failure/error surface", .query_insert = "@bugs" },
    .{ .action = .quick_failures, .name = "failures", .description = "jump to failed tests, FAIL records, and broken artifacts", .query_insert = "@failures" },
    .{ .action = .quick_regressions, .name = "regressions", .description = "jump to regressions, stale goldens, and behavior drift", .query_insert = "@regressions" },
    .{ .action = .quick_performance, .name = "performance", .description = "jump to performance, cache, index, and allocation notes", .query_insert = "@performance" },
    .{ .action = .quick_coverage, .name = "coverage", .description = "jump to coverage/test completeness metadata", .query_insert = "@coverage" },
    .{ .action = .quick_examples, .name = "examples", .description = "jump to examples, catalogues, and sample queries", .query_insert = "@examples" },
    .{ .action = .quick_tutorials, .name = "tutorials", .description = "jump to tutorials, manuals, and onboarding docs", .query_insert = "@tutorials" },
    .{ .action = .quick_api, .name = "api", .description = "jump to public APIs, signatures, and interface contracts", .query_insert = "@api" },
    .{ .action = .quick_cli, .name = "cli", .description = "jump to CLI, flags, commands, and scripts", .query_insert = "@cli" },
    .{ .action = .quick_cache, .name = "cache", .description = "jump to cache/index/persistence surfaces", .query_insert = "@cache" },
    .{ .action = .quick_diagnostics, .name = "diagnostics", .description = "jump to diagnostics, errors, warnings, and reporting", .query_insert = "@diagnostics" },
    .{ .action = .quick_design, .name = "design", .description = "jump to design decisions, architecture, and intent", .query_insert = "@design" },
    .{ .action = .quick_hot, .name = "hot", .description = "jump to urgent TODO/failure triage lane", .query_insert = "@hot" },
    .{ .action = .quick_blocked, .name = "blocked", .description = "show objects incident to blocking edges", .query_insert = "@blocked" },
    .{ .action = .quick_roots, .name = "roots", .description = "show objects with outgoing arrows and no incoming arrows", .query_insert = "@roots" },
    .{ .action = .quick_leaves, .name = "leaves", .description = "show objects with incoming arrows and no outgoing arrows", .query_insert = "@leaves" },
    .{ .action = .quick_orphans, .name = "orphans", .description = "show disconnected objects", .query_insert = "@orphans" },
    .{ .action = .quick_wisp, .name = "wisp", .description = "jump to Wisp language surface", .query_insert = "@wisp" },
    .{ .action = .quick_reader, .name = "reader", .description = "jump to reader/parser surface", .query_insert = "@reader" },
    .{ .action = .quick_codegen, .name = "codegen", .description = "jump to codegen/runtime surface", .query_insert = "@codegen" },
    .{ .action = .quick_functions, .name = "functions", .description = "jump to first-class core/prelude functions", .query_insert = "@functions" },
    .{ .action = .quick_contracts, .name = "contracts", .description = "jump to contracts and explicit invariants", .query_insert = "@contracts" },
    .{ .action = .quick_quality, .name = "quality", .description = "jump to trust/risk/anti-pattern metadata", .query_insert = "@quality" },
    .{ .action = .quick_metadata, .name = "metadata", .description = "jump to Org properties and CONTEXT_* metadata", .query_insert = "@metadata" },
    .{ .action = .quick_links, .name = "links", .description = "jump to clickable org/file/id links", .query_insert = "@links" },
    .{ .action = .quick_tables, .name = "tables", .description = "jump to rich org-table surfaces", .query_insert = "@tables" },
    .{ .action = .quick_docs, .name = "docs", .description = "jump to documents, info, reports, and manuals", .query_insert = "@docs" },
    .{ .action = .change_directory, .name = "change-directory", .description = "change the recursive category root inside Catface" },
    .{ .action = .query_verifies, .name = "verifies", .description = "insert %verifies edge filter", .query_insert = "%verifies" },
    .{ .action = .query_blocks, .name = "blocks", .description = "insert %blocks edge filter", .query_insert = "%blocks" },
    .{ .action = .query_outgoing, .name = "outgoing", .description = "append > and inspect Hom(A,-)", .query_insert = ">" },
    .{ .action = .query_incoming, .name = "incoming", .description = "append < and inspect Hom(-,A)", .query_insert = "<" },
    .{ .action = .query_neighborhood, .name = "neighborhood", .description = "append ~ and show adjacent objects", .query_insert = "~" },
    .{ .action = .query_projection, .name = "projection", .description = "append proj and project to concepts", .query_insert = "proj" },
    .{ .action = .copy_org_link, .name = "copy-org-link", .description = "format focused object as an org link" },
    .{ .action = .copy_object_id, .name = "copy-object-id", .description = "copy object id" },
    .{ .action = .export_card, .name = "export-card", .description = "print focused card" },
    .{ .action = .run_check, .name = "check-category", .description = "run free-category checks" },
    .{ .action = .open_source, .name = "open-source", .description = "open source path in $EDITOR" },
    .{ .action = .show_help, .name = "help", .description = "show query language help" },
};

pub const MatchKind = enum { command, object };

pub const Match = struct {
    kind: MatchKind = .command,
    index: usize,
    score: i32,
};

const MaxTerms = 16;

pub fn search(allocator: std.mem.Allocator, text: []const u8, limit: usize) !std.array_list.Managed(Match) {
    var out = std.array_list.Managed(Match).init(allocator);
    try searchInto(&out, text, limit);
    return out;
}

pub fn searchInto(out: *std.array_list.Managed(Match), text: []const u8, limit: usize) !void {
    out.clearRetainingCapacity();
    try out.ensureTotalCapacity(limit);
    var terms_buf: [MaxTerms][]const u8 = undefined;
    const terms = splitTerms(&terms_buf, text);
    for (commands, 0..) |c, i| {
        const score_value = orderlessScore(c, terms);
        if (score_value) |value| try insertBounded(out, .{ .kind = .command, .index = i, .score = value }, limit);
    }
    std.mem.sort(Match, out.items, {}, less);
    if (out.items.len > limit) out.shrinkRetainingCapacity(limit);
}

pub fn searchContextInto(allocator: std.mem.Allocator, out: *std.array_list.Managed(Match), ctx: *const model.Context, idx: *const search_index.SearchIndex, text: []const u8, limit: usize) !void {
    if (objectCompletionHead(text)) |head| {
        // A namespace/kind head followed by a space is an explicit request for
        // object candidates, not command names. Returning an empty bounded list
        // is more stable than falling back to command completion and repainting
        // a different candidate shape after every keystroke.
        try searchObjectsInto(allocator, out, ctx, idx, head.query, head.filter, limit);
        return;
    }
    try searchInto(out, text, limit);
}

const ObjectHead = struct { query: []const u8, filter: []const u8 };

fn objectCompletionHead(text: []const u8) ?ObjectHead {
    const trimmed_right = std.mem.trimRight(u8, text, " \t\r\n");
    if (trimmed_right.len == 0) return null;
    var query_part = trimmed_right;
    var filter: []const u8 = "";
    const ended_with_space = trimmed_right.len != text.len;
    if (!ended_with_space) {
        if (lastCompletionSpace(trimmed_right)) |sp| {
            query_part = std.mem.trimRight(u8, trimmed_right[0..sp], " \t");
            filter = trimmed_right[sp + 1 ..];
        } else return null;
    }
    if (query_part.len == 0) return null;
    if (std.mem.indexOfScalar(u8, query_part, '@') == null and std.mem.indexOfScalar(u8, query_part, ':') == null and std.mem.indexOfScalar(u8, query_part, '%') == null) return null;
    return .{ .query = query_part, .filter = filter };
}

fn lastCompletionSpace(text: []const u8) ?usize {
    if (text.len == 0) return null;
    var i = text.len;
    while (i > 0) {
        i -= 1;
        if (text[i] == ' ' or text[i] == '\t') return i;
    }
    return null;
}

fn searchObjectsInto(allocator: std.mem.Allocator, out: *std.array_list.Managed(Match), ctx: *const model.Context, idx: *const search_index.SearchIndex, query_text: []const u8, filter: []const u8, limit: usize) !void {
    out.clearRetainingCapacity();
    try out.ensureTotalCapacity(limit);
    const eval_limit = @max(limit * 8, @as(usize, 64));
    var results = try query.evaluateIndexed(allocator, ctx, idx, query_text, .{ .limit = eval_limit });
    defer results.deinit();
    for (results.items) |result| {
        const object_index = result.object_index;
        const obj = ctx.objects.items[object_index];
        const score_value = objectCandidateScore(obj, filter) orelse continue;
        try insertBounded(out, .{ .kind = .object, .index = object_index, .score = score_value }, limit);
    }
    std.mem.sort(Match, out.items, {}, less);
    if (out.items.len > limit) out.shrinkRetainingCapacity(limit);
}

fn objectCandidateScore(obj: model.Object, filter: []const u8) ?i32 {
    if (filter.len == 0) return 1;
    var terms_buf: [MaxTerms][]const u8 = undefined;
    const terms = splitTerms(&terms_buf, filter);
    if (terms.len == 0) return 1;
    var total: i32 = 0;
    for (terms) |term| {
        var best = fuzzy.score(obj.id, term);
        const st = fuzzy.score(obj.title, term);
        if (st.value > best.value) best = st;
        const sp = fuzzy.score(obj.path, term);
        if (sp.value > best.value) best = sp;
        const sv = fuzzy.score(obj.preview, term);
        if (sv.value > best.value) best = sv;
        if (!best.matched) return null;
        total += best.value;
    }
    return total;
}

pub fn matchName(ctx: *const model.Context, m: Match) []const u8 {
    return oneLine(switch (m.kind) {
        .command => commands[m.index].name,
        .object => blk: {
            const obj = ctx.objects.items[m.index];
            break :blk if (obj.title.len != 0) obj.title else obj.id;
        },
    });
}

pub fn matchDescription(ctx: *const model.Context, m: Match) []const u8 {
    return oneLine(switch (m.kind) {
        .command => commands[m.index].description,
        .object => ctx.objects.items[m.index].preview,
    });
}

pub fn matchInsert(ctx: *const model.Context, m: Match) []const u8 {
    return switch (m.kind) {
        .command => if (commands[m.index].query_insert.len != 0) commands[m.index].query_insert else actionLabel(commands[m.index].action),
        .object => ctx.objects.items[m.index].id,
    };
}

pub fn promptForMatch(ctx: *const model.Context, m: ?Match) []const u8 {
    if (m) |mm| switch (mm.kind) {
        .command => return promptFor(commands[mm.index]),
        .object => return "object:",
    };
    _ = ctx;
    return "complete:";
}

fn oneLine(text: []const u8) []const u8 {
    var end: usize = 0;
    while (end < text.len and text[end] != '\n' and text[end] != '\r') : (end += 1) {}
    return std.mem.trim(u8, text[0..end], " \t");
}

fn splitTerms(buf: *[MaxTerms][]const u8, text: []const u8) []const []const u8 {
    var n: usize = 0;
    var it = std.mem.tokenizeAny(u8, text, " \t\r\n");
    while (it.next()) |term| {
        if (n >= buf.len) break;
        buf[n] = term;
        n += 1;
    }
    return buf[0..n];
}

fn insertBounded(out: *std.array_list.Managed(Match), item: Match, limit: usize) !void {
    if (limit == 0) return;
    if (out.items.len < limit) {
        try out.append(item);
        return;
    }
    var worst_i: usize = 0;
    var worst = out.items[0].score;
    for (out.items, 0..) |m, i| {
        if (m.score < worst) {
            worst = m.score;
            worst_i = i;
        }
    }
    if (item.score > worst) out.items[worst_i] = item;
}

fn orderlessScore(c: CommandItem, terms: []const []const u8) ?i32 {
    if (terms.len == 0) return 0;
    var total: i32 = 0;
    for (terms) |term| {
        var best = fuzzy.score(c.name, term);
        const sd = fuzzy.score(c.description, term);
        if (sd.value > best.value) best = sd;
        if (c.query_insert.len != 0) {
            const sq = fuzzy.score(c.query_insert, term);
            if (sq.value > best.value) best = sq;
        }
        if (!best.matched) return null;
        total += best.value;
    }
    return total;
}

fn less(_: void, a: Match, b: Match) bool {
    return a.score > b.score;
}

pub fn actionLabel(a: Action) []const u8 {
    return switch (a) {
        .quick_todo => "@todo",
        .quick_notes => "@notes",
        .quick_tests => "@tests",
        .quick_source => "@source",
        .quick_info => "@info",
        .quick_bugs => "@bugs",
        .quick_failures => "@failures",
        .quick_regressions => "@regressions",
        .quick_performance => "@performance",
        .quick_coverage => "@coverage",
        .quick_examples => "@examples",
        .quick_tutorials => "@tutorials",
        .quick_api => "@api",
        .quick_cli => "@cli",
        .quick_cache => "@cache",
        .quick_diagnostics => "@diagnostics",
        .quick_design => "@design",
        .quick_hot => "@hot",
        .quick_blocked => "@blocked",
        .quick_roots => "@roots",
        .quick_leaves => "@leaves",
        .quick_orphans => "@orphans",
        .quick_wisp => "@wisp",
        .quick_reader => "@reader",
        .quick_codegen => "@codegen",
        .quick_functions => "@functions",
        .quick_contracts => "@contracts",
        .quick_quality => "@quality",
        .quick_metadata => "@metadata",
        .quick_links => "@links",
        .quick_tables => "@tables",
        .quick_docs => "@docs",
        .change_directory => "change directory",
        .query_verifies => "%verifies",
        .query_blocks => "%blocks",
        .query_outgoing => "append >",
        .query_incoming => "append <",
        .query_neighborhood => "append ~",
        .query_projection => "append proj",
        .copy_org_link => "copy org link",
        .copy_object_id => "copy id",
        .export_card => "export card",
        .run_check => "check category",
        .open_source => "open source",
        .show_help => "help",
    };
}

pub fn promptFor(item: ?CommandItem) []const u8 {
    if (item) |c| {
        return switch (c.action) {
            .change_directory => "directory root:",
            .query_verifies, .query_blocks, .query_outgoing, .query_incoming, .query_neighborhood, .query_projection => "query operator:",
            .copy_org_link, .copy_object_id, .export_card => "object action:",
            .run_check => "category action:",
            .open_source => "source action:",
            .show_help => "help action:",
            else => "category lane:",
        };
    }
    return "complete:";
}

pub fn defaultQueryForObject(obj: model.Object) []const u8 {
    return switch (obj.kind) {
        .record => ":Record",
        .heading => ":Heading",
        .concept => ":Concept",
        .script => ":Script",
        .test_kind => ":Test",
        .function_kind => ":Function",
        else => "",
    };
}

test "command search" {
    var hits = try search(std.testing.allocator, "out", 3);
    defer hits.deinit();
    try std.testing.expect(hits.items.len > 0);
}

test "orderless command palette matches separated terms" {
    var hits = try search(std.testing.allocator, "dir change", 5);
    defer hits.deinit();
    try std.testing.expect(hits.items.len > 0);
    const item = commands[hits.items[0].index];
    try std.testing.expect(item.action == .change_directory);
}

test "in-place command search reuses output list" {
    var hits = std.array_list.Managed(Match).init(std.testing.allocator);
    defer hits.deinit();
    try searchInto(&hits, "perf", 10);
    try std.testing.expect(hits.items.len > 0);
    try std.testing.expect(commands[hits.items[0].index].action == .quick_performance);
}

test "contextual palette turns namespace plus space into object candidates" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.reader-gap", .kind = .todo, .title = "TODO reader gap", .path = "context/todo.org", .preview = "TODO reader gap" });
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .path = "tests/reader.mon", .preview = "TEST reader" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var hits = std.array_list.Managed(Match).init(std.testing.allocator);
    defer hits.deinit();
    try searchContextInto(std.testing.allocator, &hits, &ctx, &idx, "@todo ", 10);
    try std.testing.expect(hits.items.len == 1);
    try std.testing.expect(hits.items[0].kind == .object);
    try std.testing.expect(std.mem.eql(u8, ctx.objects.items[hits.items[0].index].id, "todo.reader-gap"));
}


test "contextual palette never falls back to command candidates after namespace space" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var hits = std.array_list.Managed(Match).init(std.testing.allocator);
    defer hits.deinit();
    try searchContextInto(std.testing.allocator, &hits, &ctx, &idx, "@todo ", 10);
    try std.testing.expect(hits.items.len == 0);
}

test "object palette descriptions are single line and bounded" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.multiline", .kind = .todo, .title = "TODO multiline", .path = "todo.org", .preview = "TODO one\nsecond line must not escape the palette" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var hits = std.array_list.Managed(Match).init(std.testing.allocator);
    defer hits.deinit();
    try searchContextInto(std.testing.allocator, &hits, &ctx, &idx, "@todo ", 10);
    try std.testing.expect(hits.items.len <= 10);
    try std.testing.expect(hits.capacity <= 26);
    const desc = matchDescription(&ctx, hits.items[0]);
    try std.testing.expect(std.mem.indexOfScalar(u8, desc, '\n') == null);
}

test "conjunction object completion remains bounded" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.test", .kind = .test_kind, .title = "TODO TEST object", .path = "tests/a.mon", .preview = "TODO TEST" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var hits = std.array_list.Managed(Match).init(std.testing.allocator);
    defer hits.deinit();
    try searchContextInto(std.testing.allocator, &hits, &ctx, &idx, "@todo @tests ", 10);
    try std.testing.expect(hits.items.len <= 10);
}
