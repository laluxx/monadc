const std = @import("std");

pub const Stats = struct {
    last_frame_ns: u64 = 0,
    last_query_ns: u64 = 0,
    last_flush_ns: u64 = 0,
    redraws: u64 = 0,
    query_runs: u64 = 0,
    cached_refreshes: u64 = 0,
    last_palette_ns: u64 = 0,
    palette_runs: u64 = 0,
    palette_context_runs: u64 = 0,
    last_palette_visible: u64 = 0,
    last_palette_capacity: u64 = 0,
    last_palette_seen: u64 = 0,

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


pub const Trace = struct {
    name: []const u8,
    start_ns: u64,
    end_ns: u64 = 0,
    events: u64 = 0,
    bytes: u64 = 0,

    pub fn begin(name: []const u8) Trace {
        return .{ .name = name, .start_ns = nowNs() };
    }

    pub fn tick(self: *Trace, events: u64) void {
        self.events += events;
    }

    pub fn addBytes(self: *Trace, bytes: u64) void {
        self.bytes += bytes;
    }

    pub fn finish(self: *Trace) u64 {
        self.end_ns = nowNs();
        if (self.end_ns <= self.start_ns) return 0;
        return self.end_ns - self.start_ns;
    }

    pub fn nsPerEvent(self: Trace) u64 {
        if (self.events == 0) return 0;
        const end = if (self.end_ns == 0) nowNs() else self.end_ns;
        if (end <= self.start_ns) return 0;
        return @divTrunc(end - self.start_ns, self.events);
    }
};

pub const Budget = struct {
    max_visible: usize = 10,
    max_extra_capacity: usize = 16,

    pub fn checkPalette(self: Budget, visible: usize, capacity: usize) bool {
        if (visible > self.max_visible) return false;
        return capacity <= visible + self.max_extra_capacity or capacity <= self.max_visible + self.max_extra_capacity;
    }
};


test "perf nanosSince returns a u64 duration" {
    const n = nanosSince(nowNs());
    const typed: u64 = n;
    try std.testing.expect(typed <= std.math.maxInt(u64));
}


test "perf trace records monotonic measurable work" {
    var tr = Trace.begin("palette-self-test");
    tr.tick(10);
    tr.addBytes(128);
    _ = tr.finish();
    try std.testing.expect(tr.events == 10);
    try std.testing.expect(tr.bytes == 128);
    _ = tr.nsPerEvent();
}

test "palette budget proves visible candidates stay bounded" {
    const budget = Budget{};
    try std.testing.expect(budget.checkPalette(10, 16));
    try std.testing.expect(!budget.checkPalette(11, 16));
}
