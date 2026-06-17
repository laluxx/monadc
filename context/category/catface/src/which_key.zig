const std = @import("std");
const palette = @import("palette.zig");
const terminal = @import("terminal.zig");
pub const IdleDelayMs: i64 = 700;
pub const SecondaryDelayMs: i64 = 120;

pub const Binding = struct {
    key: []const u8,
    command: []const u8,
    group: bool = false,
};

pub fn prefixName(prefix: anytype) []const u8 {
    const tag = @tagName(prefix);
    if (std.mem.eql(u8, tag, "ctrl_h")) return "C-h";
    if (std.mem.eql(u8, tag, "ctrl_c")) return "C-c";
    if (std.mem.eql(u8, tag, "ctrl_x")) return "C-x";
    if (std.mem.eql(u8, tag, "describe_key")) return "C-h c";
    return "";
}

pub fn bindings(prefix: anytype) []const Binding {
    const tag = @tagName(prefix);
    if (std.mem.eql(u8, tag, "ctrl_h")) return &.{
            .{ .key = "c", .command = "describe-key-briefly" },
            .{ .key = "k", .command = "help / query manual" },
        };
    if (std.mem.eql(u8, tag, "ctrl_c")) return &.{
            .{ .key = "c", .command = "command palette" },
            .{ .key = "d", .command = "edit recursive root in minibuffer" },
            .{ .key = "p", .command = "first completion candidate" },
            .{ .key = "n", .command = "last completion candidate" },
            .{ .key = "g", .command = "keyboard quit" },
        };
    if (std.mem.eql(u8, tag, "ctrl_x")) return &.{
            .{ .key = "C-c", .command = "save session and quit Catface" },
        };
    if (std.mem.eql(u8, tag, "describe_key")) return &.{
            .{ .key = "any key", .command = "echo command bound to that key" },
        };
    return &.{};
}

pub fn shouldDisplay(prefix: anytype, frame_ms: i64, started_ms: i64, forced: bool) bool {
    if (std.mem.eql(u8, @tagName(prefix), "none")) return false;
    if (forced) return true;
    return frame_ms - started_ms >= IdleDelayMs;
}

pub fn draw(t: *terminal.Tty, area: terminal.Rect, prefix: anytype, frame_ms: i64, started_ms: i64, forced: bool, theme: palette.Theme) void {
    if (!shouldDisplay(prefix, frame_ms, started_ms, forced)) return;
    const bs = bindings(prefix);
    if (bs.len == 0 or area.h < 4 or area.w < 16) return;
    const max_h: u16 = @intCast(@min(bs.len + 2, @as(usize, 8)));
    const y: u16 = if (area.bottom() > max_h + 2) area.bottom() - max_h - 2 else area.y;
    const r = terminal.Rect{ .x = area.x, .y = y, .w = area.w, .h = max_h };
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    var title_buf: [128]u8 = undefined;
    const title = std.fmt.bufPrint(&title_buf, " {s}-  available keys", .{prefixName(prefix)}) catch " available keys";
    t.textClipped(r.x + 2, r.y, if (r.w > 4) r.w - 4 else 0, title, .{ .fg = theme.mute, .bg = theme.panel_alt, .bold = true });
    var yy = r.y + 1;
    var i: usize = 0;
    while (i < bs.len and yy < r.bottom()) : (i += 1) {
        const b = bs[i];
        t.textClipped(r.x + 2, yy, @min(@as(u16, 10), r.w), b.key, .{ .fg = theme.accent2, .bg = theme.panel_alt, .bold = true });
        if (r.w > 16) t.textClipped(r.x + 13, yy, r.w - 15, b.command, .{ .fg = theme.ink, .bg = theme.panel_alt });
        yy += 1;
    }
}

pub fn describeKey(key: []const u8, mode_name: []const u8) []const u8 {
    _ = mode_name;
    if (std.mem.eql(u8, key, "C-x C-c")) return "C-x C-c runs quit, like Emacs.";
    if (std.mem.eql(u8, key, "C-c d")) return "C-c d edits the recursive category root in the minibuffer.";
    if (std.mem.eql(u8, key, "TAB")) return "TAB opens Catface's Vertico/orderless completion palette.";
    if (std.mem.eql(u8, key, "RET")) return "RET enters the right-pane major mode for the focused object.";
    return "key is not described in the active Catface keymap";
}

test "which-key prefix bindings exist" {
    const P = enum { none, ctrl_h, ctrl_c, ctrl_x, describe_key };
    try std.testing.expect(bindings(P.ctrl_c).len >= 4);
    try std.testing.expect(std.mem.eql(u8, prefixName(P.ctrl_x), "C-x"));
}
