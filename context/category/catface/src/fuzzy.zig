const std = @import("std");

pub const Score = struct {
    matched: bool,
    value: i32,
};

pub fn score(haystack: []const u8, needle: []const u8) Score {
    if (needle.len == 0) return .{ .matched = true, .value = 0 };
    var h_i: usize = 0;
    var n_i: usize = 0;
    var value: i32 = 0;
    var streak: i32 = 0;
    var last_match: usize = 0;
    while (h_i < haystack.len and n_i < needle.len) : (h_i += 1) {
        const hc = std.ascii.toLower(haystack[h_i]);
        const nc = std.ascii.toLower(needle[n_i]);
        if (hc == nc) {
            value += 8;
            if (h_i == n_i) value += 8;
            if (h_i == 0 or isBoundary(haystack[h_i - 1])) value += 12;
            if (n_i > 0 and h_i == last_match + 1) {
                streak += 1;
                value += 4 * streak;
            } else streak = 0;
            last_match = h_i;
            n_i += 1;
        } else {
            value -= 1;
        }
    }
    if (n_i != needle.len) return .{ .matched = false, .value = -999999 };
    value -= @intCast(@divTrunc(haystack.len, 12));
    return .{ .matched = true, .value = value };
}

pub fn multiScore(haystack: []const u8, terms: []const []const u8) Score {
    var total: i32 = 0;
    for (terms) |term| {
        const s = score(haystack, term);
        if (!s.matched) return s;
        total += s.value;
    }
    return .{ .matched = true, .value = total };
}

fn isBoundary(c: u8) bool {
    return c == '-' or c == '_' or c == '.' or c == '/' or c == ' ' or c == ':';
}

test "fuzzy subsequence" {
    const s = score("typed define wisp", "tdw");
    try std.testing.expect(s.matched);
    try std.testing.expect(score("reader", "zzz").matched == false);
}
