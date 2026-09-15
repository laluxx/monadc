const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");

pub const Surface = struct {
    tty: *terminal.Tty,
    theme: palette.Theme,

    pub fn clear(self: Surface) void {
        self.tty.clear(.{ .fg = self.theme.ink, .bg = self.theme.bg });
    }

    pub fn label(self: Surface, x: u16, y: u16, name: []const u8, value: []const u8, active: bool) void {
        self.tty.text(x, y, name, .{ .fg = self.theme.mute, .bg = self.theme.panel, .bold = active });
        const off: u16 = @intCast(@min(name.len + 2, 80));
        self.tty.textClipped(x + off, y, 80, value, .{ .fg = self.theme.ink, .bg = self.theme.panel, .bold = active });
    }

    pub fn pill(self: Surface, x: u16, y: u16, text: []const u8, style: palette.Style) void {
        self.tty.text(x, y, "", .{ .fg = style.bg, .bg = self.theme.bg });
        self.tty.textClipped(x + 1, y, @intCast(@min(text.len + 2, 60)), text, style);
        self.tty.text(x + @as(u16, @intCast(@min(text.len + 3, 61))), y, "", .{ .fg = style.bg, .bg = self.theme.bg });
    }

    pub fn meter(self: Surface, x: u16, y: u16, width: u16, value: usize, max: usize, label_text: []const u8) void {
        if (width == 0) return;
        const fill: u16 = if (max == 0) 0 else @min(width, @as(u16, @intCast(@divTrunc(value * @as(usize, width), max))));
        var i: u16 = 0;
        while (i < width) : (i += 1) {
            self.tty.set(x + i, y, if (i < fill) '━' else '─', .{ .fg = if (i < fill) self.theme.accent2 else self.theme.edge, .bg = self.theme.panel });
        }
        self.tty.textClipped(x, y, width, label_text, .{ .fg = self.theme.ink, .bg = self.theme.panel, .bold = true });
    }

    pub fn divider(self: Surface, r: terminal.Rect, y: u16, title: []const u8) void {
        if (y >= r.bottom()) return;
        var x = r.x + 1;
        while (x + 1 < r.right()) : (x += 1) self.tty.set(x, y, '─', .{ .fg = self.theme.edge, .bg = self.theme.panel });
        if (title.len != 0) self.tty.textClipped(r.x + 2, y, r.w - 4, title, .{ .fg = self.theme.accent2, .bg = self.theme.panel, .bold = true });
    }

    pub fn callout(self: Surface, r: terminal.Rect, title: []const u8, body: []const u8, good: bool) void {
        const color = if (good) self.theme.good else self.theme.warn;
        self.tty.fill(r, ' ', .{ .fg = self.theme.ink, .bg = self.theme.panel_alt });
        self.tty.textClipped(r.x + 2, r.y, r.w - 4, title, .{ .fg = color, .bg = self.theme.panel_alt, .bold = true });
        self.tty.textClipped(r.x + 2, r.y + 1, r.w - 4, body, .{ .fg = self.theme.ink, .bg = self.theme.panel_alt });
    }
};

pub const RectangleSpec = struct {
    title: []const u8,
    subtitle: []const u8 = "",
    active: bool = false,
    danger: bool = false,
};

pub fn drawRect(s: Surface, r: terminal.Rect, spec: RectangleSpec) void {
    const c = if (spec.danger) s.theme.bad else if (spec.active) s.theme.accent else s.theme.edge;
    s.tty.box(r, spec.title, spec.active, s.theme);
    if (spec.subtitle.len != 0 and r.h > 2) s.tty.textClipped(r.x + 2, r.y + 1, r.w - 4, spec.subtitle, .{ .fg = c, .bg = s.theme.panel });
}

pub fn writeAnsiReset(buf: *std.array_list.Managed(u8)) !void {
    try buf.appendSlice("\x1b[0m");
}

pub fn writeAnsiFg(buf: *std.array_list.Managed(u8), color: palette.Color) !void {
    try fmtbuf.print(buf, "\x1b[38;2;{d};{d};{d}m", .{ color.r, color.g, color.b });
}

test "ansi color" {
    var buf = std.array_list.Managed(u8).init(std.testing.allocator);
    defer buf.deinit();
    try writeAnsiFg(&buf, .{ .r = 1, .g = 2, .b = 3 });
    try std.testing.expect(std.mem.indexOf(u8, buf.items, "38;2") != null);
}
