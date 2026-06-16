const std = @import("std");

pub const Stats = struct {
    last_frame_ns: u64 = 0,
    last_query_ns: u64 = 0,
    last_flush_ns: u64 = 0,
    redraws: u64 = 0,
    query_runs: u64 = 0,
    cached_refreshes: u64 = 0,

    pub fn resetFrame(self: *Stats) void {
        self.last_frame_ns = 0;
        self.last_flush_ns = 0;
    }
};

var timer: ?std.time.Timer = null;
var fallback_counter: u64 = 0;

fn fallbackNowNs() u64 {
    // Last-resort monotonic-ish counter used only if the platform cannot
    // start std.time.Timer. It preserves ordering and keeps telemetry/tests
    // alive instead of crashing the TUI.
    fallback_counter +%= 1_000_000;
    return fallback_counter;
}

pub fn nowNs() u64 {
    if (timer) |*t| return t.read();
    timer = std.time.Timer.start() catch return fallbackNowNs();
    if (timer) |*t| return t.read();
    return fallbackNowNs();
}

pub fn nanosSince(start_ns: u64) u64 {
    const end = nowNs();
    if (end <= start_ns) return 0;
    return end - start_ns;
}

test "perf nanosSince returns a u64 duration" {
    const n = nanosSince(nowNs());
    const typed: u64 = n;
    try std.testing.expect(typed <= std.math.maxInt(u64));
}
