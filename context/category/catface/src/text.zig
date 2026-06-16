const std = @import("std");

pub const SliceLine = struct {
    start: usize,
    end: usize,
};

pub fn visibleWidth(bytes: []const u8) usize {
    var count: usize = 0;
    var view = std.unicode.Utf8View.init(bytes) catch return bytes.len;
    var it = view.iterator();
    while (it.nextCodepoint()) |cp| {
        count += if (isZeroWidth(cp)) 0 else if (isWide(cp)) 2 else 1;
    }
    return count;
}

pub fn truncateUtf8(bytes: []const u8, max_width: usize) []const u8 {
    if (max_width == 0) return bytes[0..0];
    var width: usize = 0;
    var end: usize = 0;
    var view = std.unicode.Utf8View.init(bytes) catch return bytes[0..@min(bytes.len, max_width)];
    var it = view.iterator();
    while (it.nextCodepointSlice()) |slice| {
        const cp = std.unicode.utf8Decode(slice) catch break;
        const w: usize = if (isZeroWidth(cp)) 0 else if (isWide(cp)) 2 else 1;
        if (width + w > max_width) break;
        width += w;
        end = @intFromPtr(slice.ptr) - @intFromPtr(bytes.ptr) + slice.len;
    }
    return bytes[0..end];
}

pub fn nextWrappedLine(bytes: []const u8, start: usize, max_width: usize) ?SliceLine {
    if (start >= bytes.len) return null;
    var width: usize = 0;
    var end = start;
    var last_space: ?usize = null;
    var i = start;
    while (i < bytes.len) {
        const len = utf8Len(bytes[i]) catch 1;
        const slice = bytes[i..@min(bytes.len, i + len)];
        const cp = std.unicode.utf8Decode(slice) catch bytes[i];
        if (cp == '\n') return .{ .start = start, .end = i };
        const w: usize = if (isZeroWidth(cp)) 0 else if (isWide(cp)) 2 else 1;
        if (width + w > max_width) {
            if (last_space) |sp| return .{ .start = start, .end = sp };
            return .{ .start = start, .end = end };
        }
        if (cp == ' ' or cp == '\t') last_space = i;
        width += w;
        i += len;
        end = i;
    }
    return .{ .start = start, .end = end };
}

pub fn skipBreak(bytes: []const u8, start_index: usize) usize {
    var i = start_index;
    while (i < bytes.len and (bytes[i] == ' ' or bytes[i] == '\t' or bytes[i] == '\n' or bytes[i] == '\r')) i += 1;
    return i;
}

fn utf8Len(first: u8) !usize {
    if (first < 0x80) return 1;
    if ((first & 0xe0) == 0xc0) return 2;
    if ((first & 0xf0) == 0xe0) return 3;
    if ((first & 0xf8) == 0xf0) return 4;
    return error.InvalidUtf8;
}

fn isZeroWidth(cp: u21) bool {
    return (cp >= 0x0300 and cp <= 0x036f) or cp == 0xfe0f;
}

fn isWide(cp: u21) bool {
    return (cp >= 0x1100 and cp <= 0x115f) or
        (cp >= 0x2e80 and cp <= 0xa4cf) or
        (cp >= 0xac00 and cp <= 0xd7a3) or
        (cp >= 0xf900 and cp <= 0xfaff) or
        (cp >= 0x1f300 and cp <= 0x1faff);
}

pub fn normalizeToken(buf: *std.array_list.Managed(u8), raw: []const u8) ![]const u8 {
    const start = buf.items.len;
    for (raw) |c| {
        if (std.ascii.isWhitespace(c)) continue;
        try buf.append(std.ascii.toLower(c));
    }
    return buf.items[start..];
}

pub fn containsFold(haystack: []const u8, needle: []const u8) bool {
    if (needle.len == 0) return true;
    if (needle.len > haystack.len) return false;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) : (i += 1) {
        var ok = true;
        for (needle, 0..) |nc, j| {
            if (std.ascii.toLower(haystack[i + j]) != std.ascii.toLower(nc)) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

test "truncate utf8" {
    try std.testing.expect(std.mem.eql(u8, truncateUtf8("abcdef", 3), "abc"));
    try std.testing.expect(visibleWidth("abc") == 3);
}
