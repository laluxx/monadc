const std = @import("std");

pub const Color = struct {
    r: u8,
    g: u8,
    b: u8,

    pub fn rgb(r: u8, g: u8, b: u8) Color {
        return .{ .r = r, .g = g, .b = b };
    }

    pub fn scale(self: Color, numerator: u16, denominator: u16) Color {
        return .{
            .r = @intCast(@min(255, @divTrunc(@as(u16, self.r) * numerator, denominator))),
            .g = @intCast(@min(255, @divTrunc(@as(u16, self.g) * numerator, denominator))),
            .b = @intCast(@min(255, @divTrunc(@as(u16, self.b) * numerator, denominator))),
        };
    }
};

pub const Style = struct {
    fg: ?Color = null,
    bg: ?Color = null,
    bold: bool = false,
    dim: bool = false,
    italic: bool = false,
    underline: bool = false,
    reverse: bool = false,

    pub fn focused(self: Style) Style {
        var s = self;
        s.bold = true;
        if (s.fg) |c| s.fg = c.scale(125, 100);
        return s;
    }

    pub fn quiet(self: Style) Style {
        var s = self;
        s.dim = true;
        if (s.fg) |c| s.fg = c.scale(65, 100);
        return s;
    }
};

pub const Theme = struct {
    bg: Color = Color.rgb(0x08, 0x0a, 0x12),
    panel: Color = Color.rgb(0x10, 0x14, 0x20),
    panel_alt: Color = Color.rgb(0x1c, 0x25, 0x39),
    ink: Color = Color.rgb(0xec, 0xf2, 0xff),
    mute: Color = Color.rgb(0x86, 0x90, 0xa8),
    edge: Color = Color.rgb(0x34, 0x3d, 0x55),
    accent: Color = Color.rgb(0x8b, 0x7c, 0xff),
    accent2: Color = Color.rgb(0x28, 0xd4, 0xbe),
    warn: Color = Color.rgb(0xf0, 0xd9, 0x62),
    bad: Color = Color.rgb(0xff, 0x5d, 0x73),
    good: Color = Color.rgb(0x55, 0xe6, 0xa5),
    // Generic records are deliberately neutral; OBS has its own cyan lane so it
    // never collides with the green used for tests/done evidence.
    record: Color = Color.rgb(0xba, 0xc4, 0xd8),
    obs: Color = Color.rgb(0x52, 0xd6, 0xff),
    function_color: Color = Color.rgb(0xd0, 0xbc, 0xff),
    heading: Color = Color.rgb(0x5e, 0xa7, 0xff),
    script: Color = Color.rgb(0xff, 0xb8, 0x5c),
    concept: Color = Color.rgb(0xff, 0x7a, 0xa8),
    test_color: Color = Color.rgb(0x55, 0xe6, 0xa5),
    todo: Color = Color.rgb(0xf0, 0xd9, 0x62),
    done: Color = Color.rgb(0x75, 0xe6, 0x95),
    info: Color = Color.rgb(0x85, 0xca, 0xff),

    pub fn kindColor(self: Theme, kind: anytype) Color {
        const name = @tagName(kind);
        if (std.mem.eql(u8, name, "record")) return self.record;
        if (std.mem.eql(u8, name, "heading")) return self.heading;
        if (std.mem.eql(u8, name, "script")) return self.script;
        if (std.mem.eql(u8, name, "concept")) return self.concept;
        if (std.mem.eql(u8, name, "test_kind")) return self.test_color;
        if (std.mem.eql(u8, name, "todo")) return self.todo;
        if (std.mem.eql(u8, name, "done")) return self.done;
        if (std.mem.eql(u8, name, "info")) return self.info;
        if (std.mem.eql(u8, name, "function_kind")) return self.function_color;
        if (std.mem.eql(u8, name, "report")) return self.warn;
        return self.ink;
    }
};

pub const default_theme = Theme{};

test "palette scaling" {
    const c = Color.rgb(100, 50, 10).scale(2, 1);
    try std.testing.expect(c.r == 200);
    try std.testing.expect(c.g == 100);
    try std.testing.expect(c.b == 20);
}
