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
    op_outgoing,
    op_incoming,
    op_neighborhood,
    op_projection,
    history_back,
    help,
    insert_char,
};

pub const Decoded = struct {
    command: Command,
    char: ?u21 = null,
};

pub fn decode(key: terminal.Key) Decoded {
    return switch (key) {
        .none => .{ .command = .none },
        .esc => .{ .command = .quit },
        .tab => .{ .command = .switch_pane },
        .up => .{ .command = .move_up },
        .down => .{ .command = .move_down },
        .page_up => .{ .command = .page_up },
        .page_down => .{ .command = .page_down },
        .enter => .{ .command = .follow },
        .backspace => .{ .command = .backspace },
        .ctrl => |c| switch (c) {
            3 => .{ .command = .quit },
            21 => .{ .command = .clear_query },
            8 => .{ .command = .backspace },
            else => .{ .command = .none },
        },
        .char => |cp| switch (cp) {
            'q' => .{ .command = .quit },
            'j' => .{ .command = .move_down },
            'k' => .{ .command = .move_up },
            'o' => .{ .command = .op_outgoing },
            'i' => .{ .command = .op_incoming },
            'n' => .{ .command = .op_neighborhood },
            'p' => .{ .command = .op_projection },
            'b' => .{ .command = .history_back },
            '?' => .{ .command = .help },
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
        .op_outgoing => "op-outgoing",
        .op_incoming => "op-incoming",
        .op_neighborhood => "op-neighborhood",
        .op_projection => "op-projection",
        .history_back => "history-back",
        .help => "help",
        .insert_char => "insert-char",
    };
}

test "decode q" {
    const d = decode(.{ .char = 'q' });
    try std.testing.expect(d.command == .quit);
}
