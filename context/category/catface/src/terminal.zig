const std = @import("std");
const palette = @import("palette.zig");

pub const Key = union(enum) {
    none,
    char: u21,
    ctrl: u8,
    alt: u21,
    mouse: MouseEvent,
    up,
    down,
    left,
    right,
    home,
    end,
    page_up,
    page_down,
    backspace,
    delete,
    enter,
    tab,
    esc,
    resize,
};

pub const MouseEvent = struct {
    x: u16,
    y: u16,
    button: u8,
};

pub const Cell = struct {
    cp: u21 = ' ',
    style: palette.Style = .{},

    pub fn eql(self: Cell, other: Cell) bool {
        return self.cp == other.cp and styleEql(self.style, other.style);
    }
};

fn optColorEql(a: ?palette.Color, b: ?palette.Color) bool {
    if (a == null and b == null) return true;
    if (a == null or b == null) return false;
    return a.?.r == b.?.r and a.?.g == b.?.g and a.?.b == b.?.b;
}

fn styleEql(a: palette.Style, b: palette.Style) bool {
    return optColorEql(a.fg, b.fg) and optColorEql(a.bg, b.bg) and
        a.bold == b.bold and a.dim == b.dim and a.italic == b.italic and
        a.underline == b.underline and a.reverse == b.reverse;
}

pub const Rect = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,

    pub fn right(self: Rect) u16 { return self.x + self.w; }
    pub fn bottom(self: Rect) u16 { return self.y + self.h; }
    pub fn inset(self: Rect, n: u16) Rect {
        if (self.w <= n * 2 or self.h <= n * 2) return .{ .x = self.x, .y = self.y, .w = 0, .h = 0 };
        return .{ .x = self.x + n, .y = self.y + n, .w = self.w - n * 2, .h = self.h - n * 2 };
    }
};

pub const Tty = struct {
    allocator: std.mem.Allocator,
    stdout: std.fs.File,
    stdin: std.fs.File,
    original: std.posix.termios,
    width: u16 = 80,
    height: u16 = 24,
    front: []Cell = &.{},
    back: []Cell = &.{},
    mouse: bool = false,

    pub fn init(allocator: std.mem.Allocator) !*Tty {
        const tty = try allocator.create(Tty);
        tty.* = .{
            .allocator = allocator,
            .stdout = .{ .handle = std.posix.STDOUT_FILENO },
            .stdin = .{ .handle = std.posix.STDIN_FILENO },
            .original = try std.posix.tcgetattr(std.posix.STDIN_FILENO),
        };
        try tty.enableRaw();
        try tty.stdout.writeAll("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[0m");
        try tty.resize();
        return tty;
    }

    pub fn deinit(self: *Tty) void {
        self.stdout.writeAll("\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l") catch {};
        std.posix.tcsetattr(std.posix.STDIN_FILENO, .FLUSH, self.original) catch {};
        if (self.front.len != 0) self.allocator.free(self.front);
        if (self.back.len != 0) self.allocator.free(self.back);
        self.allocator.destroy(self);
    }

    fn enableRaw(self: *Tty) !void {
        var raw = self.original;
        raw.lflag.ECHO = false;
        raw.lflag.ICANON = false;
        raw.lflag.ISIG = false;
        raw.lflag.IEXTEN = false;
        raw.iflag.IXON = false;
        raw.iflag.ICRNL = false;
        raw.oflag.OPOST = false;
        raw.cc[@intFromEnum(std.posix.V.TIME)] = 0;
        raw.cc[@intFromEnum(std.posix.V.MIN)] = 0;
        try std.posix.tcsetattr(std.posix.STDIN_FILENO, .FLUSH, raw);
    }

    pub fn resize(self: *Tty) !void {
        var ws: std.posix.winsize = undefined;
        if (std.posix.system.ioctl(self.stdout.handle, std.posix.T.IOCGWINSZ, @intFromPtr(&ws)) != 0) {
            ws = .{ .row = 24, .col = 80, .xpixel = 0, .ypixel = 0 };
        }
        if (ws.col == 0 or ws.row == 0) {
            ws.col = 80;
            ws.row = 24;
        }
        self.width = ws.col;
        self.height = ws.row;
        const len = @as(usize, self.width) * @as(usize, self.height);
        self.front = try self.allocator.realloc(self.front, len);
        self.back = try self.allocator.realloc(self.back, len);
        @memset(self.front, .{ .cp = 0 });
        @memset(self.back, .{});
    }

    pub fn clear(self: *Tty, style: palette.Style) void {
        for (self.back) |*c| c.* = .{ .cp = ' ', .style = style };
    }

    pub fn set(self: *Tty, x: u16, y: u16, cp: u21, style: palette.Style) void {
        if (x >= self.width or y >= self.height) return;
        self.back[@as(usize, y) * self.width + x] = .{ .cp = cp, .style = style };
    }

    pub fn fill(self: *Tty, r: Rect, cp: u21, style: palette.Style) void {
        var yy = r.y;
        while (yy < @min(self.height, r.bottom())) : (yy += 1) {
            var xx = r.x;
            while (xx < @min(self.width, r.right())) : (xx += 1) self.set(xx, yy, cp, style);
        }
    }

    pub fn text(self: *Tty, x: u16, y: u16, bytes: []const u8, style: palette.Style) void {
        if (y >= self.height or x >= self.width) return;
        var xx = x;
        var view = std.unicode.Utf8View.init(bytes) catch return;
        var it = view.iterator();
        while (it.nextCodepoint()) |cp| {
            if (xx >= self.width) break;
            self.set(xx, y, cp, style);
            xx += 1;
        }
    }

    pub fn textClipped(self: *Tty, x: u16, y: u16, width: u16, bytes: []const u8, style: palette.Style) void {
        if (width == 0) return;
        var xx = x;
        var used: u16 = 0;
        var view = std.unicode.Utf8View.init(bytes) catch return;
        var it = view.iterator();
        while (it.nextCodepoint()) |cp| {
            if (used >= width) break;
            self.set(xx, y, cp, style);
            xx += 1;
            used += 1;
        }
        if (used == width and width > 1) {
            self.set(x + width - 1, y, '…', style);
        }
    }

    pub fn box(self: *Tty, r: Rect, title: []const u8, active: bool, theme: palette.Theme) void {
        if (r.w < 2 or r.h < 2) return;
        const line = palette.Style{ .fg = if (active) theme.accent else theme.edge, .bg = theme.panel, .bold = active };
        const fill_style = palette.Style{ .fg = theme.ink, .bg = theme.panel };
        self.fill(r, ' ', fill_style);
        var x = r.x + 1;
        while (x + 1 < r.right()) : (x += 1) {
            self.set(x, r.y, '─', line);
            self.set(x, r.bottom() - 1, '─', line);
        }
        var y = r.y + 1;
        while (y + 1 < r.bottom()) : (y += 1) {
            self.set(r.x, y, '│', line);
            self.set(r.right() - 1, y, '│', line);
        }
        self.set(r.x, r.y, '╭', line);
        self.set(r.right() - 1, r.y, '╮', line);
        self.set(r.x, r.bottom() - 1, '╰', line);
        self.set(r.right() - 1, r.bottom() - 1, '╯', line);
        if (title.len != 0 and r.w > 4) {
            self.textClipped(r.x + 2, r.y, r.w - 4, title, line);
        }
    }

    fn ansiStyle(w: anytype, from: palette.Style, to: palette.Style) !void {
        if (styleEql(from, to)) return;
        try w.interface.writeAll("\x1b[0m");
        if (to.bold) try w.interface.writeAll("\x1b[1m");
        if (to.dim) try w.interface.writeAll("\x1b[2m");
        if (to.italic) try w.interface.writeAll("\x1b[3m");
        if (to.underline) try w.interface.writeAll("\x1b[4m");
        if (to.reverse) try w.interface.writeAll("\x1b[7m");
        if (to.fg) |fg| try w.interface.print("\x1b[38;2;{d};{d};{d}m", .{ fg.r, fg.g, fg.b });
        if (to.bg) |bg| try w.interface.print("\x1b[48;2;{d};{d};{d}m", .{ bg.r, bg.g, bg.b });
    }

    pub fn flush(self: *Tty) !void {
        var buf: [32768]u8 = undefined;
        var writer = self.stdout.writer(&buf);
        var cursor_x: u16 = 65535;
        var cursor_y: u16 = 65535;
        var current_style: palette.Style = .{};
        for (0..self.height) |yy| {
            for (0..self.width) |xx| {
                const idx = yy * self.width + xx;
                const b = self.back[idx];
                const f = self.front[idx];
                if (!b.eql(f)) {
                    if (cursor_x != xx or cursor_y != yy) {
                        try writer.interface.print("\x1b[{d};{d}H", .{ yy + 1, xx + 1 });
                    }
                    try ansiStyle(&writer, current_style, b.style);
                    current_style = b.style;
                    var enc: [4]u8 = undefined;
                    const n = try std.unicode.utf8Encode(b.cp, &enc);
                    try writer.interface.writeAll(enc[0..n]);
                    self.front[idx] = b;
                    cursor_x = @as(u16, @intCast(xx)) + 1;
                    cursor_y = @as(u16, @intCast(yy));
                }
            }
        }
        try writer.interface.writeAll("\x1b[0m");
        try writer.interface.flush();
    }

    pub fn poll(self: *Tty, timeout_ms: u32) !Key {
        var fds = [_]std.posix.pollfd{.{ .fd = self.stdin.handle, .events = std.posix.POLL.IN, .revents = 0 }};
        if (try std.posix.poll(&fds, @intCast(timeout_ms)) == 0) return .none;
        var buf: [32]u8 = undefined;
        const n = try self.stdin.read(&buf);
        if (n == 0) return .none;
        return parseKey(buf[0..n]);
    }
};

pub fn parseKey(bytes: []const u8) Key {
    if (bytes.len == 0) return .none;
    const b = bytes[0];
    if (b == 0x1b) {
        if (bytes.len >= 6 and bytes[1] == '[' and bytes[2] == '<') {
            if (parseMouse(bytes)) |m| return .{ .mouse = m };
        }
        if (bytes.len >= 3 and bytes[1] == '[') {
            return switch (bytes[2]) {
                'A' => .up,
                'B' => .down,
                'C' => .right,
                'D' => .left,
                'H' => .home,
                'F' => .end,
                '3' => .delete,
                '5' => .page_up,
                '6' => .page_down,
                else => .esc,
            };
        }
        if (bytes.len >= 2) {
            var view = std.unicode.Utf8View.init(bytes[1..]) catch return .esc;
            var it = view.iterator();
            return .{ .alt = it.nextCodepoint() orelse bytes[1] };
        }
        return .esc;
    }
    if (b == 9) return .tab;
    if (b == 10 or b == 13) return .enter;
    if (b == 127 or b == 8) return .backspace;
    if (b < 32) return .{ .ctrl = b };
    var view = std.unicode.Utf8View.init(bytes) catch return .none;
    var it = view.iterator();
    return .{ .char = it.nextCodepoint() orelse b };
}

fn parseMouse(bytes: []const u8) ?MouseEvent {
    var i: usize = 3;
    var vals = [_]u16{0, 0, 0};
    var vi: usize = 0;
    while (i < bytes.len and vi < vals.len) {
        var v: u16 = 0;
        var any = false;
        while (i < bytes.len and bytes[i] >= '0' and bytes[i] <= '9') : (i += 1) {
            v = v * 10 + @as(u16, @intCast(bytes[i] - '0'));
            any = true;
        }
        if (!any) return null;
        vals[vi] = v;
        vi += 1;
        if (i < bytes.len and bytes[i] == ';') i += 1 else break;
    }
    if (vi < 3) return null;
    if (bytes[bytes.len - 1] != 'M') return null;
    return .{ .button = @intCast(vals[0]), .x = if (vals[1] == 0) 0 else vals[1] - 1, .y = if (vals[2] == 0) 0 else vals[2] - 1 };
}

test "key parser" {
    try std.testing.expect(parseKey("\x1b[A") == .up);
    try std.testing.expect(parseKey("\t") == .tab);
    try std.testing.expect(parseKey("\r") == .enter);
}
