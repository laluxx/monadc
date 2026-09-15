const std = @import("std");
const terminal = @import("terminal.zig");

pub const Command = enum {
    none,
    quit,
    move_up,
    move_down,
    page_up,
    page_down,
    switch_pane,
    follow,
    clear_query,
    backspace,
    delete_forward,
    op_outgoing,
    op_incoming,
    op_neighborhood,
    op_projection,
    history_back,
    quick_todo,
    quick_notes,
    quick_tests,
    quick_source,
    quick_info,
    quick_functions,
    quick_records,
    quick_bugs,
    quick_wisp,
    quick_reader,
    quick_codegen,
    op_verifies,
    op_blocks,
    help,
    insert_char,
};

pub const Decoded = struct {
    command: Command,
    char: ?u21 = null,
};

pub fn decode(key: terminal.Key, query_empty: bool) Decoded {
    return switch (key) {
        .none, .resize => .{ .command = .none },
        .esc => .{ .command = .quit },
        .tab => .{ .command = .switch_pane },
        .up => .{ .command = .move_up },
        .down => .{ .command = .move_down },
        .page_up => .{ .command = .page_up },
        .page_down => .{ .command = .page_down },
        .enter => .{ .command = .follow },
        .backspace => .{ .command = .backspace },
        .delete => .{ .command = .delete_forward },
        .ctrl => |c| switch (c) {
            3 => .{ .command = .quit },
            4 => .{ .command = .delete_forward },
            12, 21 => .{ .command = .clear_query },
            14 => .{ .command = .move_down },
            16 => .{ .command = .move_up },
            20 => .{ .command = .quick_todo },
            8 => .{ .command = .backspace },
            else => .{ .command = .none },
        },
        .alt => |c| switch (c) {
            't', 'T' => .{ .command = .quick_todo },
            'n', 'N' => .{ .command = .quick_notes },
            'e', 'E' => .{ .command = .quick_tests },
            's', 'S' => .{ .command = .quick_source },
            'i', 'I' => .{ .command = .quick_info },
            'f', 'F' => .{ .command = .quick_functions },
            'r', 'R' => .{ .command = .quick_records },
            'u', 'U' => .{ .command = .quick_bugs },
            'w', 'W' => .{ .command = .quick_wisp },
            'm', 'M' => .{ .command = .quick_reader },
            'c', 'C' => .{ .command = .quick_codegen },
            'v', 'V' => .{ .command = .op_verifies },
            'x', 'X' => .{ .command = .op_blocks },
            'o', 'O' => .{ .command = .op_outgoing },
            '<' => .{ .command = .op_incoming },
            'g', 'G' => .{ .command = .op_neighborhood },
            'p', 'P' => .{ .command = .op_projection },
            'b', 'B' => .{ .command = .history_back },
            else => .{ .command = .none },
        },
        .char => |cp| switch (cp) {
            '?' => if (query_empty) .{ .command = .help } else .{ .command = .insert_char, .char = cp },
            else => .{ .command = .insert_char, .char = cp },
        },
        else => .{ .command = .none },
    };
}

pub fn commandName(c: Command) []const u8 {
    return switch (c) {
        .none => "none",
        .quit => "quit",
        .move_up => "move-up",
        .move_down => "move-down",
        .page_up => "page-up",
        .page_down => "page-down",
        .switch_pane => "switch-pane",
        .follow => "follow",
        .clear_query => "clear-query",
        .backspace => "backspace",
        .delete_forward => "delete-forward",
        .op_outgoing => "op-outgoing",
        .op_incoming => "op-incoming",
        .op_neighborhood => "op-neighborhood",
        .op_projection => "op-projection",
        .history_back => "history-back",
        .quick_todo => "quick-todo",
        .quick_notes => "quick-notes",
        .quick_tests => "quick-tests",
        .quick_source => "quick-source",
        .quick_info => "quick-info",
        .quick_functions => "quick-functions",
        .quick_records => "quick-records",
        .quick_bugs => "quick-bugs",
        .quick_wisp => "quick-wisp",
        .quick_reader => "quick-reader",
        .quick_codegen => "quick-codegen",
        .op_verifies => "op-verifies",
        .op_blocks => "op-blocks",
        .help => "help",
        .insert_char => "insert-char",
    };
}

test "printable letters insert" {
    const q = decode(.{ .char = 'q' }, false);
    try std.testing.expect(q.command == .insert_char);
    try std.testing.expect(q.char.? == 'q');
}

test "empty question mark opens help" {
    try std.testing.expect(decode(.{ .char = '?' }, true).command == .help);
    try std.testing.expect(decode(.{ .char = '?' }, false).command == .insert_char);
}
