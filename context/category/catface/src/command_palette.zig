const std = @import("std");
const model = @import("model.zig");
const fuzzy = @import("fuzzy.zig");

pub const Action = enum {
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
        else => "",
    };
}

test "command search" {
    var hits = try search(std.testing.allocator, "out", 3);
    defer hits.deinit();
    try std.testing.expect(hits.items.len > 0);
}
