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
    shift_tab,
    esc,
    resize,
};

pub const MouseKind = enum {
    press,
    release,
    drag,
    scroll_up,
    scroll_down,
    scroll_left,
    scroll_right,
    unknown,
};

pub const MouseEvent = struct {
    x: u16,
    y: u16,
    button: u16,
    kind: MouseKind = .press,
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
    dirty: bool = true,
    dirty_min_x: u16 = 0,
    dirty_min_y: u16 = 0,
    dirty_max_x: u16 = 0,
    dirty_max_y: u16 = 0,

    pub fn init(allocator: std.mem.Allocator) !*Tty {
        const tty = try allocator.create(Tty);
        tty.* = .{
            .allocator = allocator,
            .stdout = .{ .handle = std.posix.STDOUT_FILENO },
            .stdin = .{ .handle = std.posix.STDIN_FILENO },
            .original = try std.posix.tcgetattr(std.posix.STDIN_FILENO),
        };
        try tty.enableRaw();
        // Alternate screen, hide cursor, disable wrap, enable SGR mouse. 1003 gives hover/motion events for live links;
        // dirty-region rendering keeps the extra mouse traffic cheap.
        try tty.stdout.writeAll("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h\x1b[?1004h\x1b[0m");
        try tty.resize();
        return tty;
    }

    pub fn deinit(self: *Tty) void {
        self.stdout.writeAll("\x1b[0m\x1b[?1004l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l") catch {};
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
        const old_len = self.front.len;
        const size_changed = self.width != ws.col or self.height != ws.row or old_len == 0;
        self.width = ws.col;
        self.height = ws.row;
        const len = @as(usize, self.width) * @as(usize, self.height);
        self.front = try self.allocator.realloc(self.front, len);
        self.back = try self.allocator.realloc(self.back, len);
        if (size_changed) {
            @memset(self.front, .{ .cp = 0 });
            @memset(self.back, .{});
            self.markAllDirty();
        }
    }

    fn markAllDirty(self: *Tty) void {
        self.dirty = true;
        self.dirty_min_x = 0;
        self.dirty_min_y = 0;
        self.dirty_max_x = if (self.width == 0) 0 else self.width - 1;
        self.dirty_max_y = if (self.height == 0) 0 else self.height - 1;
    }

    fn markDirty(self: *Tty, x: u16, y: u16) void {
        if (!self.dirty) {
            self.dirty = true;
            self.dirty_min_x = x;
            self.dirty_max_x = x;
            self.dirty_min_y = y;
            self.dirty_max_y = y;
            return;
        }
        if (x < self.dirty_min_x) self.dirty_min_x = x;
        if (x > self.dirty_max_x) self.dirty_max_x = x;
        if (y < self.dirty_min_y) self.dirty_min_y = y;
        if (y > self.dirty_max_y) self.dirty_max_y = y;
    }

    pub fn clear(self: *Tty, style: palette.Style) void {
        for (0..self.height) |yy| {
            for (0..self.width) |xx| {
                self.set(@intCast(xx), @intCast(yy), ' ', style);
            }
        }
    }

    pub fn set(self: *Tty, x: u16, y: u16, cp: u21, style: palette.Style) void {
        if (x >= self.width or y >= self.height) return;
        const idx = @as(usize, y) * @as(usize, self.width) + @as(usize, x);
        const next = Cell{ .cp = cp, .style = style };
        if (self.back[idx].eql(next)) return;
        self.back[idx] = next;
        self.markDirty(x, y);
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
        var truncated = false;
        var view = std.unicode.Utf8View.init(bytes) catch return;
        var it = view.iterator();
        while (it.nextCodepoint()) |cp| {
            if (used >= width) {
                truncated = true;
                break;
            }
            self.set(xx, y, cp, style);
            xx += 1;
            used += 1;
        }
        if (truncated and width > 1) {
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
        if (!self.dirty) return;
        var buf: [32768]u8 = undefined;
        var writer = self.stdout.writer(&buf);
        var current_style: palette.Style = .{};
        const min_y = self.dirty_min_y;
        const max_y = @min(self.dirty_max_y, if (self.height == 0) 0 else self.height - 1);
        const min_x = self.dirty_min_x;
        const max_x = @min(self.dirty_max_x, if (self.width == 0) 0 else self.width - 1);
        var yy: u16 = min_y;
        while (yy <= max_y) : (yy += 1) {
            var xx: u16 = min_x;
            while (xx <= max_x) {
                var idx = @as(usize, yy) * @as(usize, self.width) + @as(usize, xx);
                if (self.back[idx].eql(self.front[idx])) {
                    if (xx == max_x) break;
                    xx += 1;
                    continue;
                }
                try writer.interface.print("\x1b[{d};{d}H", .{ yy + 1, xx + 1 });
                while (xx <= max_x) {
                    idx = @as(usize, yy) * @as(usize, self.width) + @as(usize, xx);
                    const b = self.back[idx];
                    if (b.eql(self.front[idx])) break;
                    try ansiStyle(&writer, current_style, b.style);
                    current_style = b.style;
                    var enc: [4]u8 = undefined;
                    const n = try std.unicode.utf8Encode(b.cp, &enc);
                    try writer.interface.writeAll(enc[0..n]);
                    self.front[idx] = b;
                    if (xx == max_x) break;
                    xx += 1;
                }
                if (xx == max_x) break;
            }
            if (yy == max_y) break;
        }
        try writer.interface.writeAll("\x1b[0m");
        try writer.interface.flush();
        self.dirty = false;
    }

    pub fn poll(self: *Tty, timeout_ms: u32) !Key {
        var fds = [_]std.posix.pollfd{.{ .fd = self.stdin.handle, .events = std.posix.POLL.IN, .revents = 0 }};
        if (try std.posix.poll(&fds, @intCast(timeout_ms)) == 0) return .none;
        var buf: [128]u8 = undefined;
        var n = try self.stdin.read(&buf);
        if (n == 0) return .none;

        // SGR mouse packets are escape sequences and terminals may split them
        // across reads.  If we drop a partial packet, the tail (for example
        // `64;10;5M`) is later interpreted as normal text and pollutes the
        // minibuffer.  Coalesce short escape packets before parseKey.
        var spins: usize = 0;
        while (isIncompleteEscape(buf[0..n]) and n < buf.len and spins < 8) : (spins += 1) {
            var more_fds = [_]std.posix.pollfd{.{ .fd = self.stdin.handle, .events = std.posix.POLL.IN, .revents = 0 }};
            if (try std.posix.poll(&more_fds, 2) == 0) break;
            const m = try self.stdin.read(buf[n..]);
            if (m == 0) break;
            n += m;
        }
        if (isIncompleteEscape(buf[0..n])) return .none;
        return parseKey(buf[0..n]);
    }
};

fn isIncompleteEscape(bytes: []const u8) bool {
    if (bytes.len == 0 or bytes[0] != 0x1b) return false;
    if (bytes.len == 1) return true;
    if (bytes[1] != '[') return false;
    if (bytes.len == 2) return true;
    if (bytes[2] == '<') {
        const last = bytes[bytes.len - 1];
        return last != 'M' and last != 'm';
    }
    const last = bytes[bytes.len - 1];
    return !(last >= '@' and last <= '~');
}

pub fn clippedWouldTruncate(bytes: []const u8, width: u16) bool {
    if (width == 0) return bytes.len != 0;
    var used: u16 = 0;
    var view = std.unicode.Utf8View.init(bytes) catch return false;
    var it = view.iterator();
    while (it.nextCodepoint()) |_| {
        if (used >= width) return true;
        used += 1;
    }
    return false;
}

test "escape coalescing detects partial sgr mouse packets" {
    try std.testing.expect(isIncompleteEscape("\x1b"));
    try std.testing.expect(isIncompleteEscape("\x1b[<64;10"));
    try std.testing.expect(!isIncompleteEscape("\x1b[<64;10;5M"));
    try std.testing.expect(!isIncompleteEscape("a"));
}

test "text clipping does not ellipsize exact-width tags" {
    try std.testing.expect(!clippedWouldTruncate("NOTE", 4));
    try std.testing.expect(!clippedWouldTruncate("TODO", 4));
    try std.testing.expect(clippedWouldTruncate("NOTE", 3));
}

pub fn parseKey(bytes: []const u8) Key {
    if (bytes.len == 0) return .none;
    const b = bytes[0];
    if (b == 0x1b) {
        if (bytes.len == 1) return .esc;
        if (bytes.len >= 3 and bytes[1] == '[' and bytes[2] == '<') {
            if (parseSgrMouse(bytes)) |m| return .{ .mouse = m };
            return .none;
        }
        if (bytes.len >= 3 and bytes[1] == '[') {
            return parseCsi(bytes[2..]);
        }
        if (bytes.len >= 2) {
            var view = std.unicode.Utf8View.init(bytes[1..]) catch return .none;
            var it = view.iterator();
            return .{ .alt = it.nextCodepoint() orelse bytes[1] };
        }
        return .none;
    }
    if (b == 9) return .tab;
    if (b == 10 or b == 13) return .enter;
    if (b == 127) return .backspace;
    if (b < 32) return .{ .ctrl = b };
    var view = std.unicode.Utf8View.init(bytes) catch return .none;
    var it = view.iterator();
    return .{ .char = it.nextCodepoint() orelse b };
}

fn parseCsi(bytes: []const u8) Key {
    if (bytes.len == 0) return .none;
    if (bytes[bytes.len - 1] == 'u') return parseCsiU(bytes);
    return switch (bytes[0]) {
        'A' => .up,
        'B' => .down,
        'C' => .right,
        'D' => .left,
        'H' => .home,
        'F' => .end,
        'Z' => .shift_tab,
        '3' => .delete,
        '5' => .page_up,
        '6' => .page_down,
        else => .none,
    };
}


fn parseCsiU(bytes: []const u8) Key {
    // Kitty/xterm modifyOtherKeys style: CSI codepoint ; modifier u.
    // This lets terminals that support enhanced keyboard reporting distinguish
    // physical TAB from C-i, so TAB can be completion and C-i can stay @info.
    if (bytes.len < 2 or bytes[bytes.len - 1] != 'u') return .none;
    const body = bytes[0 .. bytes.len - 1];
    var parts = std.mem.splitScalar(u8, body, ';');
    const cp_text = parts.next() orelse return .none;
    const cp = std.fmt.parseInt(u21, cp_text, 10) catch return .none;
    const mod_text = parts.next() orelse "1";
    const mod_value = std.fmt.parseInt(u8, mod_text, 10) catch 1;
    const flags: u8 = if (mod_value == 0) 0 else mod_value - 1;
    const has_shift = (flags & 1) != 0;
    const has_alt = (flags & 2) != 0;
    const has_ctrl = (flags & 4) != 0;
    if (has_ctrl and cp >= 'a' and cp <= 'z') return .{ .ctrl = @as(u8, @intCast(cp - 'a' + 1)) };
    if (has_ctrl and cp >= 'A' and cp <= 'Z') return .{ .ctrl = @as(u8, @intCast(cp - 'A' + 1)) };
    if (has_ctrl and cp == 'i') return .{ .ctrl = 9 };
    if (has_ctrl and cp == 9) return .{ .ctrl = 9 };
    if (has_alt) return .{ .alt = cp };
    if (cp == 9) return if (has_shift) .shift_tab else .tab;
    if (cp == 13 or cp == 10) return .enter;
    if (cp == 127) return .backspace;
    return .{ .char = cp };
}

fn parseSgrMouse(bytes: []const u8) ?MouseEvent {
    if (bytes.len < 6 or bytes[0] != 0x1b or bytes[1] != '[' or bytes[2] != '<') return null;
    const final = bytes[bytes.len - 1];
    if (final != 'M' and final != 'm') return null;
    var i: usize = 3;
    var vals = [_]u16{ 0, 0, 0 };
    var vi: usize = 0;
    while (i + 1 < bytes.len and vi < vals.len) {
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
    const code = vals[0];
    const kind: MouseKind = if (final == 'm' or code == 3)
        .release
    else if (code == 64)
        .scroll_up
    else if (code == 65)
        .scroll_down
    else if (code == 66)
        .scroll_left
    else if (code == 67)
        .scroll_right
    else if ((code & 32) != 0)
        .drag
    else
        .press;
    return .{
        .button = code,
        .kind = kind,
        .x = if (vals[1] == 0) 0 else vals[1] - 1,
        .y = if (vals[2] == 0) 0 else vals[2] - 1,
    };
}

test "key parser" {
    try std.testing.expect(parseKey("\x1b[A") == .up);
    try std.testing.expect(parseKey("\t") == .tab);
    try std.testing.expect(parseKey("\r") == .enter);
    try std.testing.expect(parseKey("\x1b[Z") == .shift_tab);
    switch (parseKey("\x08")) {
        .ctrl => |c| try std.testing.expect(c == 8),
        else => return error.ExpectedCtrlH,
    }
    switch (parseKey("\x1b[105;5u")) {
        .ctrl => |c| try std.testing.expect(c == 9),
        else => return error.ExpectedCtrlI,
    }
}

test "sgr mouse parser accepts press release and scroll without escaping" {
    switch (parseKey("\x1b[<0;10;5M")) {
        .mouse => |m| {
            try std.testing.expect(m.kind == .press);
            try std.testing.expect(m.x == 9);
            try std.testing.expect(m.y == 4);
        },
        else => return error.ExpectedMousePress,
    }
    switch (parseKey("\x1b[<0;10;5m")) {
        .mouse => |m| try std.testing.expect(m.kind == .release),
        else => return error.ExpectedMouseRelease,
    }
    switch (parseKey("\x1b[<64;10;5M")) {
        .mouse => |m| try std.testing.expect(m.kind == .scroll_up),
        else => return error.ExpectedMouseScroll,
    }
    try std.testing.expect(parseKey("\x1b[<bad") == .none);
}
