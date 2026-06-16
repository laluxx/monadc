const std = @import("std");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const model = @import("model.zig");
const render = @import("render.zig");
const ui = @import("ui.zig");
const perf = @import("perf.zig");
const tree = @import("tree.zig");
const search_index = @import("index.zig");

const IdlePollMs: u32 = 25;

pub fn run(allocator: std.mem.Allocator, ctx: *model.Context) !void {
    var tty = try terminal.Tty.init(allocator);
    defer tty.deinit();

    var search = try search_index.SearchIndex.build(allocator, ctx);
    defer search.deinit();

    var state = try ui.State.init(allocator);
    defer state.deinit();
    try state.setQuery("");
    state.message = "Type naturally. Try @todo, @hot, title:reader, @tests -> reader, or %verifies @tests -> codegen. ? help.";
    try state.refreshIndexed(ctx, &search);

    while (true) {
        syncViewports(&state, ctx, &search, tty.width, tty.height);
        if (state.consumeScreenDirty()) {
            try draw(tty, ctx, &search, &state);
        }
        const key = try tty.poll(IdlePollMs);
        state.advanceClock(IdlePollMs);
        if (key == .none) continue;
        const done = try handleKey(&state, ctx, &search, key, tty.width, tty.height);
        if (done) break;
        try state.refreshIndexed(ctx, &search);
    }
}

fn draw(tty: *terminal.Tty, ctx: *const model.Context, search: *const search_index.SearchIndex, state: *ui.State) !void {
    const frame_start = perf.nowNs();
    try tty.resize();
    const theme = palette.default_theme;
    const lay = render.layout(tty.width, tty.height);
    syncViewports(state, ctx, search, tty.width, tty.height);
    render.drawHeader(tty, lay.header, state.query_buffer.items, state.cursor, state.cursorVisible(state.frame_ms), state.active == .left, ctx, state.results.items.len, theme);
    render.drawResults(tty, lay.left, ctx, state.results.items, state.selected, state.scroll, state.active == .left, theme);
    render.drawDetailIndexed(tty, lay.right, ctx, search, state.focus, &state.relation_tree, state.focus_stack.items, state.active == .right, theme);
    render.drawFooter(tty, lay.footer, state.message, state.perf_stats, theme);
    if (state.show_tutorial) render.drawTutorial(tty, lay, theme);
    const flush_start = perf.nowNs();
    try tty.flush();
    state.perf_stats.last_flush_ns = perf.nanosSince(flush_start);
    state.perf_stats.last_frame_ns = perf.nanosSince(frame_start);
    state.perf_stats.redraws += 1;
}

fn syncViewports(state: *ui.State, ctx: *const model.Context, search: *const search_index.SearchIndex, width: u16, height: u16) void {
    const lay = render.layout(width, height);
    state.setResultViewport(render.resultVisibleRows(lay.left));
    const tree_rows = render.detailTreeVisibleRows(lay.right, ctx, search, state.focus);
    const row_count = render.detailTreeRowCountIndexed(ctx, search, state.focus, &state.relation_tree);
    state.relation_tree.sync(row_count, tree_rows);
}

fn handleKey(state: *ui.State, ctx: *const model.Context, search: *const search_index.SearchIndex, key: terminal.Key, width: u16, height: u16) !bool {
    switch (key) {
        .none => return false,
        .resize => { state.markScreenDirty(); return false; },
        .esc => {
            if (state.show_tutorial) {
                state.toggleTutorial();
                return false;
            }
            return true;
        },
        .tab => { state.active = if (state.active == .left) .right else .left; state.message = if (state.active == .left) "search pane: typing edits the prompt" else "relation pane: n/p/j/k move tree, RET/l opens, h backs up; C-n/C-p move results"; state.markScreenDirty(); },
        .up => if (state.active == .right) state.moveTreeCursor(-1) else state.move(-1),
        .down => if (state.active == .right) state.moveTreeCursor(1) else state.move(1),
        .left => if (state.active == .right) state.moveTreeCursor(-1) else state.moveCursorLeft(),
        .right => if (state.active == .right) try openTreeCursor(state, ctx, search) else state.moveCursorRight(),
        .home => if (state.active == .right) state.moveTreeCursor(-9999) else state.beginningOfLine(),
        .end => if (state.active == .right) state.moveTreeCursor(9999) else state.endOfLine(),
        .page_up => if (state.active == .right) state.scrollTree(-10) else state.pageResults(-1),
        .page_down => if (state.active == .right) state.scrollTree(10) else state.pageResults(1),
        .enter => if (state.active == .right) try openTreeCursor(state, ctx, search) else try state.followFocused(ctx),
        .backspace => if (state.active == .left) state.backspace() else { state.message = "relation pane active: Tab returns to search"; state.markScreenDirty(); },
        .delete => if (state.active == .left) state.deleteForward() else { state.message = "relation pane active: Tab returns to search"; state.markScreenDirty(); },
        .mouse => |m| try handleMouse(state, ctx, search, m, width, height),
        .alt => |c| switch (c) {
            'd', 'D' => try state.killWord(),
            't', 'T' => try state.quickQuery("@todo", "TODO work queue"),
            'n', 'N' => try state.quickQuery("@notes", "notes/context records"),
            'e', 'E' => try state.quickQuery("@tests", "tests namespace"),
            's', 'S' => try state.quickQuery("@source", "source/scripts namespace"),
            'i', 'I' => try state.quickQuery("@info", "info/docs namespace"),
            'f', 'F' => try state.quickQuery("@functions", "core/function/type namespace"),
            'r', 'R' => try state.quickQuery(":Record", "context record objects"),
            'u', 'U' => try state.quickQuery("@bugs", "bug/failure/error surface"),
            'w', 'W' => try state.quickQuery("@wisp", "Wisp language surface"),
            'm', 'M' => try state.quickQuery("@reader", "reader/parser surface"),
            'c', 'C' => try state.quickQuery("@codegen", "codegen/runtime surface"),
            'v', 'V' => try state.appendOp("%verifies", "edge filter: verifies"),
            'x', 'X' => try state.appendOp("%blocks", "edge filter: blocks"),
            'o', 'O' => try state.appendOp(">", "outgoing Hom(object, -)"),
            '<' => try state.appendOp("<", "incoming Hom(-, object)"),
            'g', 'G' => try state.appendOp("~", "graph neighborhood"),
            'p', 'P' => try state.appendOp("proj", "projection / concept closure"),
            'b', 'B' => try state.goBack(),
            else => {},
        },
        .ctrl => |c| switch (c) {
            1 => state.beginningOfLine(),       // C-a
            3 => return true,                  // C-c
            4 => state.deleteForward(),        // C-d
            5 => state.endOfLine(),            // C-e
            7 => try state.appendOp("~", "graph neighborhood"), // C-g
            8 => state.backspace(),            // C-h
            11 => try state.killToEnd(),       // C-k
            12 => { try state.setQuery(""); state.message = "cleared query"; }, // C-l
            14 => state.move(1),             // C-n always moves the main result list
            16 => state.move(-1),            // C-p always moves the main result list
            20 => try state.quickQuery("@todo", "TODO work queue"), // C-t
            21 => { try state.setQuery(""); state.message = "cleared query"; }, // C-u
            25 => try state.yank(),            // C-y
            else => {},
        },
        .char => |cp| {
            if (state.active == .right) {
                switch (cp) {
                    'n', 'j' => state.moveTreeCursor(1),
                    'p', 'k' => state.moveTreeCursor(-1),
                    'h' => { if (!state.goBackFocus()) { state.message = "no relation back target"; state.markScreenDirty(); } },
                    'o', 'l', '\r', '\n' => try openTreeCursor(state, ctx, search),
                    '?' => state.toggleTutorial(),
                    else => { state.message = "relation pane active: n/p/j/k move tree, RET/l opens, h backs up; C-n/C-p move results"; state.markScreenDirty(); },
                }
            } else {
                switch (cp) {
                    '?' => if (state.query_buffer.items.len == 0) state.toggleTutorial() else try state.appendUtf8(cp),
                    else => try state.appendUtf8(cp),
                }
            }
        },
    }
    return false;
}

fn openTreeCursor(state: *ui.State, ctx: *const model.Context, search: *const search_index.SearchIndex) !void {
    const action = render.detailTreeActionAtCursorIndexed(ctx, search, state.focus, &state.relation_tree);
    switch (action) {
        .select => |target| {
            try state.pushFocusBack(state.focus);
            state.selected = findResultIndex(state.results.items, target) orelse state.selected;
            state.focus = target;
            state.message = "opened relation target; h goes back, Tab returns to search";
            state.markScreenDirty();
        },
        .toggle_dir, .toggle_group => {
            state.applyTreeAction(action);
            const rows = render.detailTreeRowCountIndexed(ctx, search, state.focus, &state.relation_tree);
            state.relation_tree.sync(rows, state.relation_tree.view_rows);
            state.message = "toggled relation-tree heading";
        },
        .none => {
            state.message = "no relation row under cursor";
            state.markScreenDirty();
        },
    }
}

fn handleMouse(state: *ui.State, ctx: *const model.Context, search: *const search_index.SearchIndex, m: terminal.MouseEvent, width: u16, height: u16) !void {
    const lay = render.layout(width, height);
    if (m.kind == .release or m.kind == .drag or m.kind == .unknown) return;

    const in_left = m.y >= lay.left.y + 1 and m.y + 1 < lay.left.bottom() and m.x >= lay.left.x and m.x < lay.left.right();
    const in_right = m.x >= lay.right.x and m.x < lay.right.right() and m.y >= lay.right.y and m.y < lay.right.bottom();

    if (m.kind == .scroll_up or m.kind == .scroll_down) {
        const delta: isize = if (m.kind == .scroll_up) -3 else 3;
        if (in_right) {
            state.active = .right;
            state.scrollTree(delta);
            state.message = "scrolled relation tree";
            state.markScreenDirty();
        } else {
            state.active = .left;
            state.scrollResults(delta);
            state.message = "scrolled results";
            state.markScreenDirty();
        }
        return;
    }

    if (in_left) {
        const row: usize = @intCast(m.y - lay.left.y - 1);
        const idx = state.scroll + @divTrunc(row, render.ResultRowHeight);
        if (idx < state.results.items.len) {
            state.selected = idx;
            state.focus = state.results.items[idx];
            state.active = .left;
            state.message = "selected result; Enter focuses it";
            state.markScreenDirty();
        }
    } else if (in_right) {
        state.active = .right;
        const tree_rows = render.detailTreeVisibleRows(lay.right, ctx, search, state.focus);
        const row_count = render.detailTreeRowCountIndexed(ctx, search, state.focus, &state.relation_tree);
        state.relation_tree.sync(row_count, tree_rows);
        const action = render.detailHitTestIndexed(ctx, search, lay.right, state.focus, &state.relation_tree, m.y);
        switch (action) {
            .select => |target| {
                try state.pushFocusBack(state.focus);
                state.selected = findResultIndex(state.results.items, target) orelse state.selected;
                state.focus = target;
                state.message = "selected relation target; h goes back";
                state.markScreenDirty();
            },
            .toggle_dir => {
                state.applyTreeAction(action);
                state.message = "toggled relation-tree direction";
            },
            .toggle_group => {
                state.applyTreeAction(action);
                state.message = "toggled relation-tree group";
            },
            .none => {
                state.message = "detail pane focused; click tree arrows/headings or an object row";
                state.markScreenDirty();
            },
        }
    }
}

fn findResultIndex(results: []const usize, target: usize) ?usize {
    for (results, 0..) |idx, i| {
        if (idx == target) return i;
    }
    return null;
}

test "mouse release is ignored rather than treated as quit" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    try handleMouse(&state, &ctx, &search, .{ .x = 0, .y = 0, .button = 0, .kind = .release }, 80, 24);
    try std.testing.expect(!state.screen_dirty or state.message.len == 0);
}

test "tree toggle action mutates tree state" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    try std.testing.expect(state.relation_tree.dirOpen(.out));
    state.applyTreeAction(.{ .toggle_dir = .out });
    try std.testing.expect(!state.relation_tree.dirOpen(.out));
}

test "C-n and C-p keep moving the main result list when relation pane is focused" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "a", .kind = .record, .title = "A" });
    _ = try ctx.addObject(.{ .id = "b", .kind = .record, .title = "B" });
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    try state.setQuery(":Record");
    try state.refreshIndexed(&ctx, &search);
    state.active = .right;
    try std.testing.expect(state.selected == 0);
    _ = try handleKey(&state, &ctx, &search, .{ .ctrl = 14 }, 80, 24);
    try std.testing.expect(state.selected == 1);
    _ = try handleKey(&state, &ctx, &search, .{ .ctrl = 16 }, 80, 24);
    try std.testing.expect(state.selected == 0);
}

test "h in relation pane jumps back through relation focus stack" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "a", .kind = .record, .title = "A" });
    _ = try ctx.addObject(.{ .id = "b", .kind = .record, .title = "B" });
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    try state.setQuery(":Record");
    try state.refreshIndexed(&ctx, &search);
    state.active = .right;
    try state.pushFocusBack(state.focus);
    state.focus = 1;
    state.selected = 1;
    _ = try handleKey(&state, &ctx, &search, .{ .char = 'h' }, 80, 24);
    try std.testing.expect(state.focus.? == 0);
}
