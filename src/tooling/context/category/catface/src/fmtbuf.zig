const std = @import("std");

/// Small formatting helper for Zig versions whose managed ArrayList no longer
/// exposes writer(). Catface's formatted UI/status strings are bounded; if a
/// single formatted fragment is absurdly large, preserve progress with a clear
/// marker instead of failing compilation-portability.
pub fn print(buf: *std.array_list.Managed(u8), comptime fmt: []const u8, args: anytype) !void {
    var scratch: [16384]u8 = undefined;
    const text = std.fmt.bufPrint(&scratch, fmt, args) catch {
        try buf.appendSlice("[catface: formatted fragment too large]");
        return;
    };
    try buf.appendSlice(text);
}

test "format into managed buffer" {
    var buf = std.array_list.Managed(u8).init(std.testing.allocator);
    defer buf.deinit();
    try print(&buf, "hello {s} {d}", .{ "cat", 7 });
    try std.testing.expect(std.mem.eql(u8, buf.items, "hello cat 7"));
}
