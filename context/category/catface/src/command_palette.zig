const std = @import("std");
const model = @import("model.zig");
const fuzzy = @import("fuzzy.zig");

pub const Action = enum {
    quick_todo,
    quick_notes,
    quick_tests,
    quick_source,
    quick_info,
    quick_bugs,
    quick_hot,
    quick_blocked,
    quick_roots,
    quick_leaves,
    quick_orphans,
    quick_wisp,
    quick_reader,
    quick_codegen,
    quick_functions,
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
    .{ .action = .quick_hot, .name = "hot", .description = "jump to urgent TODO/failure triage lane", .query_insert = "@hot" },
    .{ .action = .quick_blocked, .name = "blocked", .description = "show objects incident to blocking edges", .query_insert = "@blocked" },
    .{ .action = .quick_roots, .name = "roots", .description = "show objects with outgoing arrows and no incoming arrows", .query_insert = "@roots" },
    .{ .action = .quick_leaves, .name = "leaves", .description = "show objects with incoming arrows and no outgoing arrows", .query_insert = "@leaves" },
    .{ .action = .quick_orphans, .name = "orphans", .description = "show disconnected objects", .query_insert = "@orphans" },
    .{ .action = .quick_wisp, .name = "wisp", .description = "jump to Wisp language surface", .query_insert = "@wisp" },
    .{ .action = .quick_reader, .name = "reader", .description = "jump to reader/parser surface", .query_insert = "@reader" },
    .{ .action = .quick_codegen, .name = "codegen", .description = "jump to codegen/runtime surface", .query_insert = "@codegen" },
    .{ .action = .quick_functions, .name = "functions", .description = "jump to first-class core/prelude functions", .query_insert = "@functions" },
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

pub const Match = struct {
    index: usize,
    score: i32,
};

pub fn search(allocator: std.mem.Allocator, text: []const u8, limit: usize) !std.array_list.Managed(Match) {
    var out = std.array_list.Managed(Match).init(allocator);
    for (commands, 0..) |c, i| {
        var best = fuzzy.score(c.name, text);
        const sd = fuzzy.score(c.description, text);
        if (sd.value > best.value) best = sd;
        if (best.matched) try out.append(.{ .index = i, .score = best.value });
    }
    std.mem.sort(Match, out.items, {}, less);
    if (out.items.len > limit) out.shrinkRetainingCapacity(limit);
    return out;
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
        .quick_hot => "@hot",
        .quick_blocked => "@blocked",
        .quick_roots => "@roots",
        .quick_leaves => "@leaves",
        .quick_orphans => "@orphans",
        .quick_wisp => "@wisp",
        .quick_reader => "@reader",
        .quick_codegen => "@codegen",
        .quick_functions => "@functions",
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
