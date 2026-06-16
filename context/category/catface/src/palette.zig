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
            .r = @intCast(@min(255, (@as(u16, self.r) * numerator) / denominator)),
            .g = @intCast(@min(255, (@as(u16, self.g) * numerator) / denominator)),
            .b = @intCast(@min(255, (@as(u16, self.b) * numerator) / denominator)),
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
    bg: Color = Color.rgb(0x10, 0x10, 0x16),
    panel: Color = Color.rgb(0x17, 0x17, 0x22),
    panel_alt: Color = Color.rgb(0x1e, 0x1e, 0x2d),
    ink: Color = Color.rgb(0xe8, 0xe6, 0xf2),
    mute: Color = Color.rgb(0x7e, 0x7a, 0x91),
    edge: Color = Color.rgb(0x55, 0x50, 0x6e),
    accent: Color = Color.rgb(0x95, 0x87, 0xdd),
    accent2: Color = Color.rgb(0x49, 0xbd, 0xb0),
    warn: Color = Color.rgb(0xea, 0xe4, 0x6a),
    bad: Color = Color.rgb(0xe8, 0x4c, 0x58),
    good: Color = Color.rgb(0x65, 0xe6, 0xa7),
    record: Color = Color.rgb(0xc6, 0xe8, 0x7a),
    heading: Color = Color.rgb(0x42, 0xa5, 0xf5),
    script: Color = Color.rgb(0xff, 0xa5, 0x00),
    concept: Color = Color.rgb(0xeb, 0x59, 0x5a),
    test_color: Color = Color.rgb(0x65, 0xe6, 0xa7),
    todo: Color = Color.rgb(0xea, 0xe4, 0x6a),
    done: Color = Color.rgb(0x72, 0xe0, 0x9b),
    info: Color = Color.rgb(0x8b, 0xc8, 0xff),

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
