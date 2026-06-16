const std = @import("std");
const model = @import("model.zig");
const query = @import("query.zig");
const tree = @import("tree.zig");
const perf = @import("perf.zig");
const search_index = @import("index.zig");

pub const Pane = enum { left, right };
pub const ViewMode = enum { search, outgoing, incoming, neighborhood, projection };

pub const HistoryEntry = struct {
    query: []const u8,
    focus_id: []const u8,
};

const QueryCacheEntry = struct {
    results: []usize,
};

const QueryCacheLimit = 64;

const BlinkDelayMs: i64 = 500;
const BlinkIntervalMs: i64 = 500;

pub const State = struct {
    allocator: std.mem.Allocator,
    query_buffer: std.array_list.Managed(u8),
    kill_ring: std.array_list.Managed(u8),
    results: std.array_list.Managed(usize),
    selected: usize = 0,
    scroll: usize = 0,
    active: Pane = .left,
    mode: ViewMode = .search,
    focus: ?usize = null,
    message: []const u8 = "",
    history: std.array_list.Managed(HistoryEntry),
    query_cache: std.StringHashMap(QueryCacheEntry),
    focus_stack: std.array_list.Managed(usize),
    history_index: usize = 0,
    cursor: usize = 0,
    frame_ms: i64 = 0,
    last_input_ms: i64 = 0,
    last_command_was_kill: bool = false,
    show_tutorial: bool = false,
    query_dirty: bool = true,
    screen_dirty: bool = true,
    relation_tree: tree.State = tree.State.init(),
    perf_stats: perf.Stats = .{},
    last_blink_visible: bool = true,
    result_view_rows: usize = 20,

    pub fn init(allocator: std.mem.Allocator) !State {
        return .{
            .allocator = allocator,
            .query_buffer = std.array_list.Managed(u8).init(allocator),
            .kill_ring = std.array_list.Managed(u8).init(allocator),
            .results = std.array_list.Managed(usize).init(allocator),
            .history = std.array_list.Managed(HistoryEntry).init(allocator),
            .query_cache = std.StringHashMap(QueryCacheEntry).init(allocator),
            .focus_stack = std.array_list.Managed(usize).init(allocator),
            .last_input_ms = 0,
        };
    }

    pub fn deinit(self: *State) void {
        self.query_buffer.deinit();
        self.kill_ring.deinit();
        self.results.deinit();
        for (self.history.items) |h| {
            self.allocator.free(h.query);
            self.allocator.free(h.focus_id);
        }
        self.history.deinit();
        self.clearQueryCache();
        self.query_cache.deinit();
        self.focus_stack.deinit();
    }

    pub fn refresh(self: *State, ctx: *const model.Context) !void {
        try self.refreshWithIndex(ctx, null);
    }

    pub fn refreshIndexed(self: *State, ctx: *const model.Context, idx: *const search_index.SearchIndex) !void {
        try self.refreshWithIndex(ctx, idx);
    }

    fn refreshWithIndex(self: *State, ctx: *const model.Context, idx_opt: ?*const search_index.SearchIndex) !void {
        if (!self.query_dirty) {
            self.perf_stats.cached_refreshes += 1;
            return;
        }
        const start = perf.nowNs();
        if (self.query_cache.get(self.query_buffer.items)) |cached| {
            self.results.clearRetainingCapacity();
            try self.results.appendSlice(cached.results);
            self.finishRefreshFromResults();
            self.perf_stats.last_query_ns = perf.nanosSince(start);
            self.perf_stats.cached_refreshes += 1;
            return;
        }
        var res = blk: {
            if (idx_opt) |idx| {
                break :blk try query.evaluateIndexed(self.allocator, ctx, idx, self.query_buffer.items, .{ .limit = 500 });
            } else {
                break :blk try query.evaluate(self.allocator, ctx, self.query_buffer.items, .{ .limit = 500 });
            }
        };
        defer res.deinit();
        self.results.clearRetainingCapacity();
        for (res.items) |r| try self.results.append(r.object_index);
        try self.rememberQueryResult(self.query_buffer.items, self.results.items);
        self.finishRefreshFromResults();
        self.perf_stats.last_query_ns = perf.nanosSince(start);
        self.perf_stats.query_runs += 1;
    }

    fn finishRefreshFromResults(self: *State) void {
        if (self.selected >= self.results.items.len) self.selected = if (self.results.items.len == 0) 0 else self.results.items.len - 1;
        self.focus = if (self.results.items.len == 0) null else self.results.items[self.selected];
        self.ensureVisible(self.result_view_rows);
        if (self.cursor > self.query_buffer.items.len) self.cursor = self.query_buffer.items.len;
        self.query_dirty = false;
        self.screen_dirty = true;
    }

    fn rememberQueryResult(self: *State, query_text: []const u8, result_indexes: []const usize) !void {
        if (query_text.len == 0 or result_indexes.len == 0) return;
        if (self.query_cache.count() >= QueryCacheLimit) self.clearQueryCache();
        const key = try self.allocator.dupe(u8, query_text);
        errdefer self.allocator.free(key);
        const values = try self.allocator.dupe(usize, result_indexes);
        errdefer self.allocator.free(values);
        try self.query_cache.put(key, .{ .results = values });
    }

    fn clearQueryCache(self: *State) void {
        var it = self.query_cache.iterator();
        while (it.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            self.allocator.free(entry.value_ptr.*.results);
        }
        self.query_cache.clearRetainingCapacity();
    }


    pub fn setResultViewport(self: *State, rows: usize) void {
        self.result_view_rows = if (rows == 0) 1 else rows;
        self.ensureVisible(self.result_view_rows);
    }

    pub fn ensureVisible(self: *State, height: usize) void {
        if (height == 0 or self.results.items.len == 0) {
            self.scroll = 0;
            return;
        }
        if (self.selected < self.scroll) self.scroll = self.selected;
        if (self.selected >= self.scroll + height) self.scroll = self.selected - height + 1;
        const max_scroll = if (self.results.items.len > height) self.results.items.len - height else 0;
        if (self.scroll > max_scroll) self.scroll = max_scroll;
    }

    pub fn move(self: *State, delta: isize) void {
        self.last_command_was_kill = false;
        if (self.results.items.len == 0) return;
        const len: isize = @intCast(self.results.items.len);
        var next: isize = @intCast(self.selected);
        next += delta;
        if (next < 0) next = 0;
        if (next >= len) next = len - 1;
        self.selected = @intCast(next);
        self.focus = self.results.items[self.selected];
        self.ensureVisible(self.result_view_rows);
        self.screen_dirty = true;
    }

    pub fn setQuery(self: *State, text: []const u8) !void {
        self.query_buffer.clearRetainingCapacity();
        try self.query_buffer.appendSlice(text);
        self.cursor = self.query_buffer.items.len;
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }

    pub fn appendUtf8(self: *State, cp: u21) !void {
        var tmp: [4]u8 = undefined;
        const n = try std.unicode.utf8Encode(cp, &tmp);
        try self.insertBytes(tmp[0..n]);
    }

    pub fn insertBytes(self: *State, bytes: []const u8) !void {
        const old_len = self.query_buffer.items.len;
        try self.query_buffer.appendSlice(bytes);
        if (self.cursor < old_len) {
            std.mem.copyBackwards(u8, self.query_buffer.items[self.cursor + bytes.len .. old_len + bytes.len], self.query_buffer.items[self.cursor..old_len]);
            std.mem.copyForwards(u8, self.query_buffer.items[self.cursor .. self.cursor + bytes.len], bytes);
        }
        self.cursor += bytes.len;
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }

    pub fn backspace(self: *State) void {
        if (self.cursor == 0 or self.query_buffer.items.len == 0) return;
        var start = self.cursor - 1;
        while (start > 0 and (self.query_buffer.items[start] & 0b1100_0000) == 0b1000_0000) start -= 1;
        self.deleteRange(start, self.cursor);
        self.cursor = start;
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }

    pub fn deleteForward(self: *State) void {
        if (self.cursor >= self.query_buffer.items.len) return;
        var end = self.cursor + 1;
        while (end < self.query_buffer.items.len and (self.query_buffer.items[end] & 0b1100_0000) == 0b1000_0000) end += 1;
        self.deleteRange(self.cursor, end);
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }

    pub fn beginningOfLine(self: *State) void { self.cursor = 0; self.resetBlink(); self.screen_dirty = true; self.last_command_was_kill = false; }
    pub fn endOfLine(self: *State) void { self.cursor = self.query_buffer.items.len; self.resetBlink(); self.screen_dirty = true; self.last_command_was_kill = false; }
    pub fn moveCursorLeft(self: *State) void { if (self.cursor > 0) self.cursor -= 1; self.resetBlink(); self.screen_dirty = true; self.last_command_was_kill = false; }
    pub fn moveCursorRight(self: *State) void { if (self.cursor < self.query_buffer.items.len) self.cursor += 1; self.resetBlink(); self.screen_dirty = true; self.last_command_was_kill = false; }

    pub fn killToEnd(self: *State) !void {
        if (self.cursor >= self.query_buffer.items.len) return;
        try self.recordKill(self.query_buffer.items[self.cursor..]);
        while (self.query_buffer.items.len > self.cursor) _ = self.query_buffer.pop();
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = true;
        self.message = "killed to end; C-y yanks";
    }

    pub fn killWord(self: *State) !void {
        if (self.cursor >= self.query_buffer.items.len) return;
        var end = self.cursor;
        while (end < self.query_buffer.items.len and std.ascii.isWhitespace(self.query_buffer.items[end])) end += 1;
        while (end < self.query_buffer.items.len and !std.ascii.isWhitespace(self.query_buffer.items[end])) end += 1;
        if (end == self.cursor) return;
        try self.recordKill(self.query_buffer.items[self.cursor..end]);
        self.deleteRange(self.cursor, end);
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = true;
        self.message = "killed word; C-y yanks";
    }

    pub fn yank(self: *State) !void {
        if (self.kill_ring.items.len == 0) return;
        try self.insertBytes(self.kill_ring.items);
        self.message = "yanked kill ring";
        self.query_dirty = true;
        self.screen_dirty = true;
    }

    fn recordKill(self: *State, bytes: []const u8) !void {
        if (!self.last_command_was_kill) self.kill_ring.clearRetainingCapacity();
        try self.kill_ring.appendSlice(bytes);
    }

    fn deleteRange(self: *State, start: usize, end: usize) void {
        if (end <= start or start >= self.query_buffer.items.len) return;
        const real_end = @min(end, self.query_buffer.items.len);
        const n = real_end - start;
        std.mem.copyForwards(u8, self.query_buffer.items[start..], self.query_buffer.items[real_end..]);
        var i: usize = 0;
        while (i < n) : (i += 1) _ = self.query_buffer.pop();
        if (self.cursor > self.query_buffer.items.len) self.cursor = self.query_buffer.items.len;
    }


    pub fn advanceClock(self: *State, delta_ms: i64) void {
        self.frame_ms += delta_ms;
        if (self.frame_ms < 0) self.frame_ms = 0;
        const now_visible = self.cursorVisible(self.frame_ms);
        if (now_visible != self.last_blink_visible) {
            self.last_blink_visible = now_visible;
            self.screen_dirty = true;
        }
    }

    pub fn cursorVisible(self: *const State, now_ms: i64) bool {
        const dt = now_ms - self.last_input_ms;
        if (dt < BlinkDelayMs) return true;
        const phase = @divTrunc(dt - BlinkDelayMs, BlinkIntervalMs);
        return @mod(phase, 2) == 0;
    }

    pub fn resetBlink(self: *State) void {
        self.last_input_ms = self.frame_ms;
    }

    pub fn followFocused(self: *State, ctx: *const model.Context) !void {
        if (self.focus == null) return;
        const obj = ctx.objects.items[self.focus.?];
        try self.pushHistory(ctx);
        const q = try std.fmt.allocPrint(self.allocator, "?{s}", .{obj.id});
        defer self.allocator.free(q);
        try self.setQuery(q);
        self.message = "focused object as ?id; Alt-b returns";
    }

    pub fn appendOp(self: *State, op: []const u8, msg: []const u8) !void {
        if (self.query_buffer.items.len != 0 and self.cursor == self.query_buffer.items.len) try self.query_buffer.append(' ');
        self.cursor = self.query_buffer.items.len;
        try self.query_buffer.appendSlice(op);
        self.cursor = self.query_buffer.items.len;
        self.message = msg;
        self.resetBlink();
        self.query_dirty = true;
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }

    pub fn quickQuery(self: *State, text: []const u8, msg: []const u8) !void {
        try self.pushRawHistory();
        try self.setQuery(text);
        self.selected = 0;
        self.scroll = 0;
        self.message = msg;
        self.query_dirty = true;
        self.screen_dirty = true;
    }

    pub fn pushHistory(self: *State, ctx: *const model.Context) !void {
        const focus_id = if (self.focus) |f| ctx.objects.items[f].id else "";
        try self.history.append(.{ .query = try self.allocator.dupe(u8, self.query_buffer.items), .focus_id = try self.allocator.dupe(u8, focus_id) });
        self.history_index = self.history.items.len;
    }

    fn pushRawHistory(self: *State) !void {
        try self.history.append(.{ .query = try self.allocator.dupe(u8, self.query_buffer.items), .focus_id = try self.allocator.dupe(u8, "") });
        self.history_index = self.history.items.len;
    }

    pub fn goBack(self: *State) !void {
        if (self.history.items.len == 0 or self.history_index == 0) return;
        self.history_index -= 1;
        const h = self.history.items[self.history_index];
        try self.setQuery(h.query);
        self.message = "history back";
        self.query_dirty = true;
        self.screen_dirty = true;
    }

    pub fn toggleTutorial(self: *State) void {
        self.show_tutorial = !self.show_tutorial;
        self.message = if (self.show_tutorial) "tutorial overlay" else "tutorial hidden";
        self.screen_dirty = true;
        self.last_command_was_kill = false;
    }
    pub fn markScreenDirty(self: *State) void {
        self.screen_dirty = true;
    }

    pub fn consumeScreenDirty(self: *State) bool {
        const dirty = self.screen_dirty;
        self.screen_dirty = false;
        return dirty;
    }

    pub fn scrollResults(self: *State, delta: isize) void {
        if (self.results.items.len == 0) return;
        self.move(delta);
    }

    pub fn pageResults(self: *State, dir: isize) void {
        const rows: isize = @intCast(self.result_view_rows);
        self.move(rows * dir);
    }

    pub fn moveTreeCursor(self: *State, delta: isize) void {
        self.relation_tree.moveCursor(delta);
        self.screen_dirty = true;
    }

    pub fn scrollTree(self: *State, delta: isize) void {
        self.relation_tree.scrollBy(delta);
        self.screen_dirty = true;
    }

    pub fn applyTreeAction(self: *State, action: tree.Action) void {
        self.relation_tree.apply(action);
        self.screen_dirty = true;
    }

    pub fn pushFocusBack(self: *State, idx: ?usize) !void {
        if (idx) |value| {
            if (self.focus_stack.items.len == 0 or self.focus_stack.items[self.focus_stack.items.len - 1] != value) {
                try self.focus_stack.append(value);
            }
        }
    }

    pub fn goBackFocus(self: *State) bool {
        if (self.focus_stack.items.len == 0) return false;
        const value = self.focus_stack.items[self.focus_stack.items.len - 1];
        _ = self.focus_stack.pop();
        self.focus = value;
        for (self.results.items, 0..) |object_index, result_index| {
            if (object_index == value) {
                self.selected = result_index;
                break;
            }
        }
        self.ensureVisible(self.result_view_rows);
        self.message = "relation back";
        self.screen_dirty = true;
        return true;
    }

};

pub fn helpText() []const u8 {
    return "Type naturally. Words fuzzy-search everything. Use @todo/@hot/@blocked/@notes/@functions/@tests/@source/@reader/@wisp, :Kind, title:/path:/id:/preview:/tag:/function:/sig:, %edge-kind, ?id/#id, a -> a, a -> b, a <- b, >, <, ~, proj.";
}

test "state refresh caches results until query changes" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.a", .kind = .todo, .title = "TODO A", .path = "context/a.org", .preview = "TODO A" });
    var state = try State.init(std.testing.allocator);
    defer state.deinit();
    try state.setQuery("@todo");
    try state.refresh(&ctx);
    const runs = state.perf_stats.query_runs;
    try state.refresh(&ctx);
    try std.testing.expect(state.perf_stats.query_runs == runs);
    try std.testing.expect(state.perf_stats.cached_refreshes == 1);
    state.setResultViewport(1);
    state.move(10);
    try std.testing.expect(state.scroll == state.selected);
    try state.appendUtf8('x');
    try state.refresh(&ctx);
    try std.testing.expect(state.perf_stats.query_runs == runs + 1);
}

test "query result cache serves repeated dirty queries without recompute" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.a", .kind = .todo, .title = "TODO A", .path = "context/a.org", .preview = "TODO A" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var state = try State.init(std.testing.allocator);
    defer state.deinit();
    try state.setQuery("@todo");
    try state.refreshIndexed(&ctx, &idx);
    const runs = state.perf_stats.query_runs;
    try state.setQuery("@todo");
    try state.refreshIndexed(&ctx, &idx);
    try std.testing.expect(state.perf_stats.query_runs == runs);
    try std.testing.expect(state.perf_stats.cached_refreshes >= 1);
}

test "relation focus stack supports h-style back navigation" {
    var state = try State.init(std.testing.allocator);
    defer state.deinit();
    try state.results.appendSlice(&[_]usize{ 2, 4, 8 });
    state.focus = 4;
    state.selected = 1;
    try state.pushFocusBack(state.focus);
    state.focus = 8;
    state.selected = 2;
    try std.testing.expect(state.goBackFocus());
    try std.testing.expect(state.focus.? == 4);
    try std.testing.expect(state.selected == 1);
}
