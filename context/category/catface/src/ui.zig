const std = @import("std");
const model = @import("model.zig");
const query = @import("query.zig");
const tree = @import("tree.zig");
const perf = @import("perf.zig");
const search_index = @import("index.zig");
const command_palette = @import("command_palette.zig");
const which_key = @import("which_key.zig");

pub const Pane = enum { left, right };
pub const ViewMode = enum { search, outgoing, incoming, neighborhood, projection };
pub const PromptMode = enum { search, command_palette, directory };
pub const PendingChord = enum { none, ctrl_h, ctrl_c, ctrl_x, describe_key };
pub const ContentMode = enum { none, test_contract, info_page, function_type, source_file, record_card, org_doc };

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
    prompt_mode: PromptMode = .search,
    pending_chord: PendingChord = .none,
    prefix_started_ms: i64 = 0,
    which_key_forced: bool = false,
    major_mode_name: []const u8 = "catface-search-mode",
    content_mode: ContentMode = .none,
    palette_buffer: std.array_list.Managed(u8),
    palette_cursor: usize = 0,
    palette_matches: std.array_list.Managed(command_palette.Match),
    palette_selected: usize = 0,
    directory_buffer: std.array_list.Managed(u8),
    directory_cursor: usize = 0,
    right_hover: bool = false,
    right_hover_x: u16 = 0,
    right_hover_y: u16 = 0,
    right_press: bool = false,
    right_press_x: u16 = 0,
    right_press_y: u16 = 0,
    viewer_open: bool = false,
    viewer_scroll: usize = 0,
    org_scroll: usize = 0,
    org_cursor: usize = 0,

    pub fn init(allocator: std.mem.Allocator) !State {
        return .{
            .allocator = allocator,
            .query_buffer = std.array_list.Managed(u8).init(allocator),
            .kill_ring = std.array_list.Managed(u8).init(allocator),
            .results = std.array_list.Managed(usize).init(allocator),
            .history = std.array_list.Managed(HistoryEntry).init(allocator),
            .query_cache = std.StringHashMap(QueryCacheEntry).init(allocator),
            .focus_stack = std.array_list.Managed(usize).init(allocator),
            .palette_buffer = std.array_list.Managed(u8).init(allocator),
            .palette_matches = std.array_list.Managed(command_palette.Match).init(allocator),
            .directory_buffer = std.array_list.Managed(u8).init(allocator),
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
        self.palette_buffer.deinit();
        self.palette_matches.deinit();
        self.directory_buffer.deinit();
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
        self.org_scroll = 0;
        self.org_cursor = 0;
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
        self.org_scroll = 0;
        self.org_cursor = 0;
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
        if (self.pending_chord != .none and which_key.shouldDisplay(self.pending_chord, self.frame_ms, self.prefix_started_ms, self.which_key_forced)) {
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

    pub fn startKeyPrefix(self: *State, prefix: PendingChord) void {
        self.pending_chord = prefix;
        self.prefix_started_ms = self.frame_ms;
        self.which_key_forced = false;
        self.screen_dirty = true;
    }

    pub fn forceWhichKey(self: *State) void {
        self.which_key_forced = true;
        self.screen_dirty = true;
    }

    pub fn clearKeyPrefix(self: *State) void {
        self.pending_chord = .none;
        self.which_key_forced = false;
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

    pub fn setRightHover(self: *State, x: u16, y: u16) void {
        if (!self.right_hover or self.right_hover_x != x or self.right_hover_y != y) {
            self.right_hover = true;
            self.right_hover_x = x;
            self.right_hover_y = y;
            self.screen_dirty = true;
        }
    }

    pub fn clearRightHover(self: *State) void {
        if (self.right_hover) {
            self.right_hover = false;
            self.screen_dirty = true;
        }
    }

    pub fn armRightClick(self: *State, x: u16, y: u16) void {
        self.right_press = true;
        self.right_press_x = x;
        self.right_press_y = y;
        self.setRightHover(x, y);
    }

    pub fn clearRightClick(self: *State) void {
        self.right_press = false;
    }

    pub fn openViewer(self: *State) void {
        self.viewer_open = true;
        self.viewer_scroll = self.org_scroll;
        self.active = .right;
        self.major_mode_name = "catface-org-view-mode";
        self.message = "right document viewer: q closes, wheel/Page scrolls, release activates links";
        self.screen_dirty = true;
    }

    pub fn closeViewer(self: *State) void {
        self.viewer_open = false;
        self.major_mode_name = "catface-search-mode";
        self.message = "closed right document viewer";
        self.screen_dirty = true;
    }

    pub fn scrollViewer(self: *State, delta: isize) void {
        if (delta < 0) {
            const amount: usize = @intCast(-delta);
            self.viewer_scroll = if (amount > self.viewer_scroll) 0 else self.viewer_scroll - amount;
        } else if (delta > 0) {
            self.viewer_scroll = @min(self.viewer_scroll + @as(usize, @intCast(delta)), 100000);
        }
        self.screen_dirty = true;
    }

    pub fn scrollOrg(self: *State, delta: isize) void {
        if (delta < 0) {
            const amount: usize = @intCast(-delta);
            self.org_scroll = if (amount > self.org_scroll) 0 else self.org_scroll - amount;
        } else if (delta > 0) {
            self.org_scroll = @min(self.org_scroll + @as(usize, @intCast(delta)), 100000);
        }
        if (self.org_cursor < self.org_scroll) self.org_cursor = self.org_scroll;
        self.screen_dirty = true;
    }

    pub fn setOrgCursor(self: *State, line: usize) void {
        self.org_cursor = line;
        if (line < self.org_scroll) self.org_scroll = line;
        if (line >= self.org_scroll + 8) self.org_scroll = line - 7;
        self.screen_dirty = true;
    }


    pub fn enterContentMode(self: *State, ctx: *const model.Context) void {
        if (self.focus) |f| {
            const obj = ctx.objects.items[f];
            self.content_mode = inferContentMode(obj);
            self.viewer_open = false;
            self.viewer_scroll = 0;
            self.org_scroll = 0;
            self.org_cursor = 0;
            self.active = .right;
            self.major_mode_name = majorModeName(self.content_mode);
            self.message = contentModeMessage(self.content_mode);
            self.screen_dirty = true;
            self.last_command_was_kill = false;
        }
    }

    pub fn openPalette(self: *State, seed: []const u8) !void {
        self.prompt_mode = .command_palette;
        self.major_mode_name = "catface-completion-mode";
        self.palette_buffer.clearRetainingCapacity();
        try self.palette_buffer.appendSlice(seed);
        self.palette_cursor = self.palette_buffer.items.len;
        self.palette_selected = 0;
        self.pending_chord = .none;
        self.message = "completion: orderless text, C-n/C-p move, C-c p first, C-c n last, RET accepts";
        self.screen_dirty = true;
    }

    pub fn closePalette(self: *State) void {
        self.prompt_mode = .search;
        self.major_mode_name = if (self.active == .right) "catface-org-view-mode" else "catface-search-mode";
        self.palette_buffer.clearRetainingCapacity();
        self.palette_matches.clearRetainingCapacity();
        self.palette_selected = 0;
        self.message = "completion closed";
        self.screen_dirty = true;
    }

    pub fn refreshPalette(self: *State) !void {
        const start = perf.nowNs();
        try command_palette.searchInto(&self.palette_matches, self.palette_buffer.items, 10);
        self.perf_stats.last_palette_ns = perf.nanosSince(start);
        self.perf_stats.palette_runs += 1;
        self.perf_stats.last_palette_visible = @intCast(self.palette_matches.items.len);
        self.perf_stats.last_palette_capacity = @intCast(self.palette_matches.capacity);
        self.perf_stats.last_palette_seen = @intCast(self.palette_matches.items.len);
        if (self.palette_selected >= self.palette_matches.items.len) self.palette_selected = if (self.palette_matches.items.len == 0) 0 else self.palette_matches.items.len - 1;
        self.screen_dirty = true;
    }

    pub fn refreshPaletteContextual(self: *State, ctx: *const model.Context, idx: *const search_index.SearchIndex) !void {
        const start = perf.nowNs();
        try command_palette.searchContextInto(self.allocator, &self.palette_matches, ctx, idx, self.palette_buffer.items, 10);
        self.perf_stats.last_palette_ns = perf.nanosSince(start);
        self.perf_stats.palette_context_runs += 1;
        self.perf_stats.last_palette_visible = @intCast(self.palette_matches.items.len);
        self.perf_stats.last_palette_capacity = @intCast(self.palette_matches.capacity);
        self.perf_stats.last_palette_seen = @intCast(self.palette_matches.items.len);
        if (self.palette_selected >= self.palette_matches.items.len) self.palette_selected = if (self.palette_matches.items.len == 0) 0 else self.palette_matches.items.len - 1;
        self.screen_dirty = true;
    }

    pub fn paletteMove(self: *State, delta: isize) void {
        if (self.palette_matches.items.len == 0) return;
        const len: isize = @intCast(self.palette_matches.items.len);
        var next: isize = @intCast(self.palette_selected);
        next += delta;
        if (next < 0) next = 0;
        if (next >= len) next = len - 1;
        self.palette_selected = @intCast(next);
        self.screen_dirty = true;
    }

    pub fn paletteFirst(self: *State) void {
        self.palette_selected = 0;
        self.screen_dirty = true;
    }

    pub fn paletteLast(self: *State) void {
        if (self.palette_matches.items.len != 0) self.palette_selected = self.palette_matches.items.len - 1;
        self.screen_dirty = true;
    }

    pub fn paletteInsertUtf8(self: *State, cp: u21) !void {
        var tmp: [4]u8 = undefined;
        const n = try std.unicode.utf8Encode(cp, &tmp);
        try insertInto(&self.palette_buffer, &self.palette_cursor, tmp[0..n]);
        self.palette_selected = 0;
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn paletteBackspace(self: *State) !void {
        if (self.palette_cursor == 0 or self.palette_buffer.items.len == 0) return;
        deleteBefore(&self.palette_buffer, &self.palette_cursor);
        self.palette_selected = 0;
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn paletteDeleteForward(self: *State) !void {
        if (self.palette_cursor >= self.palette_buffer.items.len) return;
        deleteAt(&self.palette_buffer, self.palette_cursor);
        self.palette_selected = 0;
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn paletteClear(self: *State) !void {
        self.palette_buffer.clearRetainingCapacity();
        self.palette_cursor = 0;
        self.palette_selected = 0;
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn paletteCursorLeft(self: *State) void { if (self.palette_cursor > 0) self.palette_cursor -= 1; self.resetBlink(); self.screen_dirty = true; }
    pub fn paletteCursorRight(self: *State) void { if (self.palette_cursor < self.palette_buffer.items.len) self.palette_cursor += 1; self.resetBlink(); self.screen_dirty = true; }
    pub fn paletteBeginningOfLine(self: *State) void { self.palette_cursor = 0; self.resetBlink(); self.screen_dirty = true; }
    pub fn paletteEndOfLine(self: *State) void { self.palette_cursor = self.palette_buffer.items.len; self.resetBlink(); self.screen_dirty = true; }

    pub fn selectedPaletteCommand(self: *const State) ?command_palette.CommandItem {
        if (self.palette_matches.items.len == 0) return null;
        const match = self.palette_matches.items[self.palette_selected];
        if (match.kind != .command) return null;
        return command_palette.commands[match.index];
    }

    pub fn selectedPaletteMatch(self: *const State) ?command_palette.Match {
        if (self.palette_matches.items.len == 0) return null;
        return self.palette_matches.items[self.palette_selected];
    }

    pub fn openDirectoryPrompt(self: *State, root: []const u8) !void {
        self.prompt_mode = .directory;
        self.major_mode_name = "catface-minibuffer-directory-mode";
        self.directory_buffer.clearRetainingCapacity();
        try self.directory_buffer.appendSlice(root);
        self.directory_cursor = self.directory_buffer.items.len;
        self.pending_chord = .none;
        self.message = "directory: edit root and press RET to reload recursively; Esc cancels";
        self.screen_dirty = true;
    }

    pub fn closeDirectoryPrompt(self: *State) void {
        self.prompt_mode = .search;
        self.major_mode_name = if (self.active == .right) "catface-org-view-mode" else "catface-search-mode";
        self.directory_buffer.clearRetainingCapacity();
        self.directory_cursor = 0;
        self.message = "directory prompt closed";
        self.screen_dirty = true;
    }

    pub fn directoryInsertUtf8(self: *State, cp: u21) !void {
        var tmp: [4]u8 = undefined;
        const n = try std.unicode.utf8Encode(cp, &tmp);
        try insertInto(&self.directory_buffer, &self.directory_cursor, tmp[0..n]);
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn directoryBackspace(self: *State) void {
        if (self.directory_cursor == 0 or self.directory_buffer.items.len == 0) return;
        deleteBefore(&self.directory_buffer, &self.directory_cursor);
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn directoryDeleteForward(self: *State) void {
        if (self.directory_cursor >= self.directory_buffer.items.len) return;
        deleteAt(&self.directory_buffer, self.directory_cursor);
        self.resetBlink();
        self.screen_dirty = true;
    }

    pub fn directoryCursorLeft(self: *State) void { if (self.directory_cursor > 0) self.directory_cursor -= 1; self.resetBlink(); self.screen_dirty = true; }
    pub fn directoryCursorRight(self: *State) void { if (self.directory_cursor < self.directory_buffer.items.len) self.directory_cursor += 1; self.resetBlink(); self.screen_dirty = true; }
    pub fn directoryBeginningOfLine(self: *State) void { self.directory_cursor = 0; self.resetBlink(); self.screen_dirty = true; }
    pub fn directoryEndOfLine(self: *State) void { self.directory_cursor = self.directory_buffer.items.len; self.resetBlink(); self.screen_dirty = true; }

    pub fn invalidateContextCaches(self: *State) void {
        self.clearQueryCache();
        self.results.clearRetainingCapacity();
        self.focus_stack.clearRetainingCapacity();
        self.selected = 0;
        self.scroll = 0;
        self.focus = null;
        self.query_dirty = true;
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
        self.org_scroll = 0;
        self.org_cursor = 0;
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

fn insertInto(buf: *std.array_list.Managed(u8), cursor: *usize, bytes: []const u8) !void {
    const old_len = buf.items.len;
    try buf.appendSlice(bytes);
    if (cursor.* < old_len) {
        std.mem.copyBackwards(u8, buf.items[cursor.* + bytes.len .. old_len + bytes.len], buf.items[cursor.*..old_len]);
        std.mem.copyForwards(u8, buf.items[cursor.* .. cursor.* + bytes.len], bytes);
    }
    cursor.* += bytes.len;
}

fn deleteBefore(buf: *std.array_list.Managed(u8), cursor: *usize) void {
    if (cursor.* == 0 or buf.items.len == 0) return;
    var start = cursor.* - 1;
    while (start > 0 and (buf.items[start] & 0b1100_0000) == 0b1000_0000) start -= 1;
    deleteRangeIn(buf, start, cursor.*);
    cursor.* = start;
}

fn deleteAt(buf: *std.array_list.Managed(u8), cursor: usize) void {
    if (cursor >= buf.items.len) return;
    var end = cursor + 1;
    while (end < buf.items.len and (buf.items[end] & 0b1100_0000) == 0b1000_0000) end += 1;
    deleteRangeIn(buf, cursor, end);
}

fn deleteRangeIn(buf: *std.array_list.Managed(u8), start: usize, end: usize) void {
    if (end <= start or start >= buf.items.len) return;
    const real_end = @min(end, buf.items.len);
    const n = real_end - start;
    std.mem.copyForwards(u8, buf.items[start..], buf.items[real_end..]);
    var i: usize = 0;
    while (i < n) : (i += 1) _ = buf.pop();
}

fn inferContentMode(obj: model.Object) ContentMode {
    return switch (obj.kind) {
        .test_kind => .test_contract,
        .info => .info_page,
        .function_kind => .function_type,
        .source, .script, .file => .source_file,
        .record, .todo, .done => .record_card,
        .heading, .concept, .report, .unknown => .org_doc,
    };
}

fn contentModeMessage(mode: ContentMode) []const u8 {
    return switch (mode) {
        .test_contract => "test-contract mode: n/p scroll, links are clickable, q closes viewer",
        .info_page => "info-page mode: readable document viewer, org links/buttons active",
        .function_type => "function-type mode: signature, callers, evidence; q closes viewer",
        .source_file => "source-file mode: path button and source/document text active",
        .record_card => "record-card mode: metadata, trust, arrows, and links active",
        .org_doc => "org-doc mode: headings, tables, and links active",
        .none => "right pane content mode",
    };
}

fn majorModeName(mode: ContentMode) []const u8 {
    return switch (mode) {
        .test_contract => "catface-test-mode",
        .info_page => "catface-info-mode",
        .function_type => "catface-function-mode",
        .source_file => "catface-source-mode",
        .record_card => "catface-record-mode",
        .org_doc => "catface-org-mode",
        .none => "catface-search-mode",
    };
}

pub fn keyNotation(key: @import("terminal.zig").Key) []const u8 {
    return switch (key) {
        .none => "<none>",
        .resize => "<resize>",
        .esc => "ESC",
        .tab => "TAB",
        .shift_tab => "S-TAB",
        .enter => "RET",
        .backspace => "DEL",
        .delete => "<delete>",
        .up => "<up>",
        .down => "<down>",
        .left => "<left>",
        .right => "<right>",
        .home => "<home>",
        .end => "<end>",
        .page_up => "<prior>",
        .page_down => "<next>",
        .ctrl => |c| ctrlNotation(c),
        .alt => |c| altNotation(c),
        .char => |c| charNotation(c),
        .mouse => "<mouse>",
    };
}

fn ctrlNotation(c: u8) []const u8 {
    return switch (c) {
        0 => "C-SPC",
        7 => "C-g",
        8 => "C-h",
        9 => "C-i",
        13 => "C-m",
        else => blk: {
            if (c >= 1 and c <= 26) {
                const table = [_][]const u8{ "C-a", "C-b", "C-c", "C-d", "C-e", "C-f", "C-g", "C-h", "C-i", "C-j", "C-k", "C-l", "C-m", "C-n", "C-o", "C-p", "C-q", "C-r", "C-s", "C-t", "C-u", "C-v", "C-w", "C-x", "C-y", "C-z" };
                break :blk table[c - 1];
            }
            break :blk "C-?";
        },
    };
}

fn altNotation(c: u21) []const u8 {
    return switch (c) {
        'a' => "M-a", 'b' => "M-b", 'c' => "M-c", 'd' => "M-d", 'e' => "M-e", 'f' => "M-f", 'g' => "M-g", 'h' => "M-h", 'i' => "M-i", 'j' => "M-j", 'k' => "M-k", 'l' => "M-l", 'm' => "M-m", 'n' => "M-n", 'o' => "M-o", 'p' => "M-p", 'q' => "M-q", 'r' => "M-r", 's' => "M-s", 't' => "M-t", 'u' => "M-u", 'v' => "M-v", 'w' => "M-w", 'x' => "M-x", 'y' => "M-y", 'z' => "M-z",
        else => "M-?",
    };
}

fn charNotation(c: u21) []const u8 {
    return switch (c) {
        ' ' => "SPC",
        '?' => "?",
        else => "self-insert-command",
    };
}

pub fn keyBrief(key: @import("terminal.zig").Key) []const u8 {
    return switch (key) {
        .tab => "completion-at-point; in right pane, jump to next Org link/button",
        .ctrl => |c| switch (c) {
            3 => "C-c prefix: C-c d directory, C-c n/p palette bounds, C-c c palette",
            7 => "keyboard-quit / cancel prompt",
            8 => "C-h prefix: C-h c describe-key-briefly, C-h k help",
            9 => "info lane (@info), when terminal distinguishes C-i from TAB",
            14 => "next-line / next candidate",
            16 => "previous-line / previous candidate",
            15 => "other-window / switch pane",
            24 => "C-x prefix: C-x C-c quit",
            else => "control command",
        },
        .enter => "enter right-pane content mode or accept prompt/candidate",
        .esc => "quit/cancel/close overlay",
        .char => |c| switch (c) { '?' => "help or insert ?", else => "self insert" },
        .alt => |c| switch (c) { 'i', 'I' => "@info", 't', 'T' => "@todo", 'f', 'F' => "@functions", 'l', 'L' => "@links", else => "meta command" },
        else => "navigation or mouse command",
    };
}

pub fn helpText() []const u8 {
    return "Type naturally. TAB completes in search and jumps links in the right pane; C-i/Alt-i jumps to @info; RET enters right content mode. Right pane: metadata chips on top, Org body scrolls independently, n/p/TAB jump Org targets, h/j/k/l drives the relation tree. C-x C-c quits. Use @todo/@hot/@blocked/@notes/@functions/@tests/@source/@reader/@wisp/@contracts/@quality/@metadata/@links/@tables/@docs, C-c d directory root switching, C-h c describe-key, :Kind, title:/path:/id:/preview:/tag:/function:/sig:, %edge-kind, ?id/#id, Int -> Int/a -> a, a -> b, a <- b, >, <, ~, proj.";
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
