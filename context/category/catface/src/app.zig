const std = @import("std");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const model = @import("model.zig");
const render = @import("render.zig");
const ui = @import("ui.zig");
const perf = @import("perf.zig");
const tree = @import("tree.zig");
const search_index = @import("index.zig");
const org = @import("org.zig");
const which_key = @import("which_key.zig");
const command_palette = @import("command_palette.zig");

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
        const done = try handleKey(&state, allocator, ctx, &search, key, tty.width, tty.height);
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
    const hover: ?render.Point = if (state.right_hover) .{ .x = state.right_hover_x, .y = state.right_hover_y } else null;
    render.drawDetailInteractive(tty, lay.right, ctx, search, state.focus, &state.relation_tree, state.focus_stack.items, state.active == .right, theme, hover, state.viewer_open, state.viewer_scroll, state.org_scroll, state.org_cursor);
    render.drawFooter(tty, lay.footer, state.message, state.perf_stats, theme);
    if (state.show_tutorial) render.drawTutorial(tty, lay, theme);
    if (state.prompt_mode == .command_palette) render.drawCommandPalette(tty, lay, ctx, state.palette_buffer.items, state.palette_cursor, state.palette_matches.items, state.palette_selected, state.cursorVisible(state.frame_ms), theme);
    if (state.prompt_mode == .directory) render.drawDirectoryPrompt(tty, lay, ctx.root, state.directory_buffer.items, state.directory_cursor, state.cursorVisible(state.frame_ms), theme);
    which_key.draw(tty, .{ .x = 0, .y = 0, .w = tty.width, .h = tty.height }, state.pending_chord, state.frame_ms, state.prefix_started_ms, state.which_key_forced, theme);
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

fn handleKey(state: *ui.State, allocator: std.mem.Allocator, ctx: *model.Context, search: *search_index.SearchIndex, key: terminal.Key, width: u16, height: u16) !bool {
    switch (key) {
        .none => return false,
        .resize => { state.markScreenDirty(); return false; },
        else => {},
    }

    if (state.pending_chord == .describe_key) {
        state.clearKeyPrefix();
        state.message = ui.keyBrief(key);
        state.markScreenDirty();
        return false;
    }

    if (state.pending_chord != .none) {
        return try handlePendingChord(state, allocator, ctx, search, key);
    }

    if (isCtrl(key, 8)) {
        state.startKeyPrefix(.ctrl_h);
        state.message = "C-h-  c describe-key-briefly, k help";
        state.markScreenDirty();
        return false;
    }
    if (isCtrl(key, 24)) {
        state.startKeyPrefix(.ctrl_x);
        state.message = "C-x-  C-c quit";
        state.markScreenDirty();
        return false;
    }
    if (isCtrl(key, 3)) {
        state.startKeyPrefix(.ctrl_c);
        state.message = "C-c-  c command palette, d directory, p first candidate, n last candidate, g cancel";
        state.markScreenDirty();
        return false;
    }

    if (state.prompt_mode != .search) {
        return try handlePromptKey(state, allocator, ctx, search, key);
    }

    switch (key) {
        .esc => {
            if (state.show_tutorial) {
                state.toggleTutorial();
                return false;
            }
            if (state.viewer_open) {
                state.closeViewer();
            }
            state.active = .right;
            state.message = "right pane focused; press i to return to search";
            state.markScreenDirty();
            return false;
        },
        .tab => if (state.active == .right) moveOrgTarget(state, ctx, 1) else { try state.openPalette(state.query_buffer.items); try state.refreshPaletteContextual(ctx, search); },
        .shift_tab => switchPane(state),
        .up => if (state.active == .right and state.viewer_open) state.scrollViewer(-1) else if (state.active == .right) state.scrollOrg(-1) else state.move(-1),
        .down => if (state.active == .right and state.viewer_open) state.scrollViewer(1) else if (state.active == .right) state.scrollOrg(1) else state.move(1),
        .left => if (state.active == .right) state.scrollOrg(-1) else state.moveCursorLeft(),
        .right => if (state.active == .right) state.scrollOrg(1) else state.moveCursorRight(),
        .home => if (state.active == .right) state.setOrgCursor(0) else state.beginningOfLine(),
        .end => if (state.active == .right) moveOrgTarget(state, ctx, -1) else state.endOfLine(),
        .page_up => if (state.active == .right and state.viewer_open) state.scrollViewer(-10) else if (state.active == .right) state.scrollOrg(-10) else state.pageResults(-1),
        .page_down => if (state.active == .right and state.viewer_open) state.scrollViewer(10) else if (state.active == .right) state.scrollOrg(10) else state.pageResults(1),
        .enter => if (state.active == .right) try openTreeCursor(state, ctx, search) else state.enterContentMode(ctx),
        .backspace => if (state.active == .left) state.backspace() else { state.message = "right pane active: C-o/Shift-Tab returns to search; q closes viewer"; state.markScreenDirty(); },
        .delete => if (state.active == .left) state.deleteForward() else { state.message = "right pane active: C-o/Shift-Tab returns to search; q closes viewer"; state.markScreenDirty(); },
        .mouse => |m| try handleMouse(state, ctx, search, m, width, height),
        .alt => |c| switch (c) {
            'd', 'D' => try state.killWord(),
            't', 'T' => try state.quickQuery("@todo", "TODO work queue"),
            'n', 'N' => try state.quickQuery("@notes", "notes/context records"),
            'e', 'E' => try state.quickQuery("@tests", "tests namespace"),
            's', 'S' => try state.quickQuery("@source", "source/scripts namespace"),
            'i', 'I' => try state.quickQuery("@info", "info/docs namespace"),
            'f', 'F' => try state.quickQuery("@functions", "core/function/type namespace"),
            'k', 'K' => try state.quickQuery("@contracts", "context contracts / explicit invariants"),
            'y', 'Y' => try state.quickQuery("@quality", "quality, trust, risk, anti-pattern metadata"),
            'a', 'A' => try state.quickQuery("@metadata", "Org properties and CONTEXT_* metadata"),
            'l', 'L' => try state.quickQuery("@links", "org/file/id link surface"),
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
            1 => state.beginningOfLine(),
            4 => state.deleteForward(),
            5 => state.endOfLine(),
            7 => { state.clearKeyPrefix(); state.closePalette(); state.closeDirectoryPrompt(); state.message = "keyboard quit"; state.markScreenDirty(); },
            9 => try state.quickQuery("@info", "info/docs namespace"),
            11 => try state.killToEnd(),
            12 => { try state.setQuery(""); state.message = "cleared query"; },
            14 => state.move(1),
            15 => switchPane(state),
            16 => state.move(-1),
            20 => try state.quickQuery("@todo", "TODO work queue"),
            21 => { try state.setQuery(""); state.message = "cleared query"; },
            25 => try state.yank(),
            else => {},
        },
        .char => |cp| try handleChar(state, ctx, search, cp),
        else => {},
    }
    return false;
}

fn moveOrgTarget(state: *ui.State, ctx: *const model.Context, dir: isize) void {
    if (state.focus) |f| {
        const obj = ctx.objects.items[f];
        if (render.nextDocumentTargetLine(obj, state.org_cursor, dir)) |line| {
            state.setOrgCursor(line);
            state.message = if (dir >= 0) "next Org link/tag" else "previous Org link/tag";
            return;
        }
    }
    state.message = "no Org link/tag target in this object";
    state.markScreenDirty();
}

fn moveOrgBottom(state: *ui.State, ctx: *const model.Context) void {
    if (state.focus) |f| {
        const count = render.documentBodyLineCount(ctx.objects.items[f]);
        if (count == 0) {
            state.setOrgCursor(0);
        } else {
            state.setOrgCursor(count - 1);
        }
        state.message = "Org bottom";
        return;
    }
    state.message = "no focused Org document";
    state.markScreenDirty();
}

fn isCtrl(key: terminal.Key, value: u8) bool {
    return switch (key) {
        .ctrl => |c| c == value,
        else => false,
    };
}

fn handleChar(state: *ui.State, ctx: *model.Context, search: *search_index.SearchIndex, cp: u21) !void {
    if (state.show_tutorial) {
        switch (cp) {
            'q', '?' => state.toggleTutorial(),
            'g', 'G' => try state.quickQuery("github README catface", "help g: GitHub/README surface"),
            'i', 'I' => try state.quickQuery("@info", "help i: info lane"),
            'f', 'F' => try state.quickQuery("@functions", "help f: function/type lane"),
            else => { state.message = "help keys: q closes, g GitHub/README, i info, f functions"; state.markScreenDirty(); },
        }
    } else if (state.active == .right) {
        if (state.viewer_open) {
            switch (cp) {
                'q' => state.closeViewer(),
                'i', 'I' => { state.active = .left; state.message = "search pane focused"; state.markScreenDirty(); },
                'n', 'j' => state.scrollViewer(1),
                'p', 'k' => state.scrollViewer(-1),
                'g' => state.scrollViewer(-100000),
                'G' => state.scrollViewer(100000),
                'h' => { if (!state.goBackFocus()) { state.message = "no relation back target"; state.markScreenDirty(); } },
                '?' => state.toggleTutorial(),
                else => { state.message = "right content mode: q closes, n/p scroll, release opens links"; state.markScreenDirty(); },
            }
        } else {
            switch (cp) {
                'i', 'I' => { state.active = .left; state.message = "search pane focused"; state.markScreenDirty(); },
                'n' => moveOrgTarget(state, ctx, 1),
                'p' => moveOrgTarget(state, ctx, -1),
                'g' => state.setOrgCursor(0),
                'G' => moveOrgBottom(state, ctx),
                'j' => state.moveTreeCursor(1),
                'k' => state.moveTreeCursor(-1),
                'h' => { if (!state.goBackFocus()) { state.message = "no relation back target"; state.markScreenDirty(); } },
                'o', 'l' => try openTreeCursor(state, ctx, search),
                'v' => state.openViewer(),
                '?' => state.toggleTutorial(),
                else => { state.message = "right pane: n/p next/previous Org link/tag, TAB next link, h/j/k/l relation tree, v viewer"; state.markScreenDirty(); },
            }
        }
    } else {
        switch (cp) {
            '?' => if (state.query_buffer.items.len == 0) state.toggleTutorial() else try state.appendUtf8(cp),
            else => try state.appendUtf8(cp),
        }
    }
}

fn handlePromptKey(state: *ui.State, allocator: std.mem.Allocator, ctx: *model.Context, search: *search_index.SearchIndex, key: terminal.Key) !bool {
    _ = allocator;
    switch (state.prompt_mode) {
        .command_palette => switch (key) {
            .esc => state.closePalette(),
            .enter => try acceptPalette(state, ctx, search),
            .up => state.paletteMove(-1),
            .down => state.paletteMove(1),
            .backspace => { try state.paletteBackspace(); try state.refreshPaletteContextual(ctx, search); },
            .left => state.paletteCursorLeft(),
            .right => state.paletteCursorRight(),
            .home => state.paletteBeginningOfLine(),
            .end => state.paletteEndOfLine(),
            .delete => { try state.paletteDeleteForward(); try state.refreshPaletteContextual(ctx, search); },
            .ctrl => |c| switch (c) {
                1 => state.paletteBeginningOfLine(),
                5 => state.paletteEndOfLine(),
                7 => state.closePalette(),
                14 => state.paletteMove(1),
                16 => state.paletteMove(-1),
                21 => { try state.paletteClear(); try state.refreshPaletteContextual(ctx, search); },
                else => {},
            },
            .char => |cp| { try state.paletteInsertUtf8(cp); try state.refreshPaletteContextual(ctx, search); },
            else => {},
        },
        .directory => switch (key) {
            .esc => state.closeDirectoryPrompt(),
            .enter => try acceptDirectory(state, ctx, search),
            .backspace => state.directoryBackspace(),
            .left => state.directoryCursorLeft(),
            .right => state.directoryCursorRight(),
            .home => state.directoryBeginningOfLine(),
            .end => state.directoryEndOfLine(),
            .delete => state.directoryDeleteForward(),
            .ctrl => |c| switch (c) {
                1 => state.directoryBeginningOfLine(),
                5 => state.directoryEndOfLine(),
                7 => state.closeDirectoryPrompt(),
                21 => { state.directory_buffer.clearRetainingCapacity(); state.directory_cursor = 0; state.markScreenDirty(); },
                else => {},
            },
            .char => |cp| try state.directoryInsertUtf8(cp),
            else => {},
        },
        .search => {},
    }
    return false;
}

fn handlePendingChord(state: *ui.State, allocator: std.mem.Allocator, ctx: *model.Context, search: *search_index.SearchIndex, key: terminal.Key) !bool {
    const pending = state.pending_chord;
    state.clearKeyPrefix();
    switch (pending) {
        .ctrl_h => switch (key) {
            .char => |cp| switch (cp) {
                'c' => { state.startKeyPrefix(.describe_key); state.message = "Describe key: press a key sequence"; state.markScreenDirty(); },
                'k' => state.toggleTutorial(),
                '?' => { state.startKeyPrefix(.ctrl_h); state.forceWhichKey(); },
                else => { state.message = "C-h is help: C-h c describe key, C-h k help"; state.markScreenDirty(); },
            },
            else => { state.message = "C-h is help: C-h c describe key, C-h k help"; state.markScreenDirty(); },
        },
        .ctrl_x => switch (key) {
            .ctrl => |c| switch (c) {
                3 => return true,
                else => { state.message = "C-x: C-c quits Catface"; state.markScreenDirty(); },
            },
            else => { state.message = "C-x: C-c quits Catface"; state.markScreenDirty(); },
        },
        .ctrl_c => switch (key) {
            .char => |cp| switch (cp) {
                'c' => { try state.openPalette(""); try state.refreshPaletteContextual(ctx, search); },
                'd' => try state.openDirectoryPrompt(ctx.root),
                'p' => { state.paletteFirst(); state.message = "first palette candidate"; },
                'n' => { state.paletteLast(); state.message = "last palette candidate"; },
                'g' => { state.message = "keyboard quit"; state.markScreenDirty(); },
                else => { state.message = "C-c: c palette, d directory, p/n palette bounds, g cancel"; state.markScreenDirty(); },
            },
            .ctrl => |c| switch (c) {
                7 => { state.message = "keyboard quit"; state.markScreenDirty(); },
                else => { state.message = "C-c prefix not bound"; state.markScreenDirty(); },
            },
            else => { state.message = "C-c prefix not bound"; state.markScreenDirty(); },
        },
        .describe_key => unreachable,
        .none => {},
    }
    _ = allocator;
    return false;
}

fn acceptPalette(state: *ui.State, ctx: *model.Context, search: *search_index.SearchIndex) !void {
    const selected_match = state.selectedPaletteMatch() orelse {
        state.closePalette();
        return;
    };
    if (selected_match.kind == .object) {
        const target = selected_match.index;
        const id = ctx.objects.items[target].id;
        const q = try std.fmt.allocPrint(state.allocator, "?{s}", .{id});
        defer state.allocator.free(q);
        state.closePalette();
        try state.quickQuery(q, "selected object candidate from contextual completion");
        try state.refreshIndexed(ctx, search);
        state.selected = findResultIndex(state.results.items, target) orelse 0;
        state.focus = target;
        state.active = .right;
        state.org_scroll = 0;
        state.org_cursor = 0;
        return;
    }
    const item = command_palette.commands[selected_match.index];
    const action = item.action;
    const query_insert = item.query_insert;
    state.closePalette();
    switch (action) {
        .query_verifies, .query_blocks, .query_outgoing, .query_incoming, .query_neighborhood, .query_projection => try state.appendOp(query_insert, item.description),
        .copy_org_link => { state.message = "copy-org-link: use the focused object id/path from the card"; state.markScreenDirty(); },
        .copy_object_id => { state.message = "copy-object-id: focused object id is visible in the card header"; state.markScreenDirty(); },
        .export_card => { state.message = "export-card: use --card <id> for a terminal-rendered card"; state.markScreenDirty(); },
        .run_check => { state.message = "check-category: use --check for full CLI report"; state.markScreenDirty(); },
        .open_source => if (state.focus) |f| try openFileSmart(state, ctx, ctx.objects.items[f].path) else { state.message = "no focused source path"; state.markScreenDirty(); },
        .show_help => state.toggleTutorial(),
        .change_directory => try state.openDirectoryPrompt(ctx.root),
        else => if (query_insert.len != 0) try state.quickQuery(query_insert, item.description),
    }
}

fn acceptDirectory(state: *ui.State, ctx: *model.Context, search: *search_index.SearchIndex) !void {
    const root = std.mem.trim(u8, state.directory_buffer.items, " \t\r\n");
    if (root.len == 0) {
        state.message = "directory cannot be empty";
        state.markScreenDirty();
        return;
    }
    const normalized_root = std.fs.cwd().realpathAlloc(state.allocator, root) catch try state.allocator.dupe(u8, root);
    defer state.allocator.free(normalized_root);
    var new_ctx = try org.loadContext(state.allocator, normalized_root);
    errdefer new_ctx.deinit();
    var new_search = try search_index.SearchIndex.build(state.allocator, &new_ctx);
    errdefer new_search.deinit();
    search.deinit();
    ctx.deinit();
    ctx.* = new_ctx;
    search.* = new_search;
    state.invalidateContextCaches();
    state.query_buffer.clearRetainingCapacity();
    state.cursor = 0;
    state.query_dirty = true;
    state.closeDirectoryPrompt();
    state.message = "changed category root; recursive scan now starts at selected directory";
}

fn switchPane(state: *ui.State) void {
    state.active = if (state.active == .left) .right else .left;
    state.message = if (state.active == .left)
        "search pane: typing edits the prompt; TAB completes; C-i/Alt-i opens @info"
    else
        "right pane: wheel scrolls Org, n/p/TAB jump Org links, h/j/k/l drives relation tree";
    state.markScreenDirty();
}

fn openTreeCursor(state: *ui.State, ctx: *const model.Context, search: *const search_index.SearchIndex) !void {
    const action = render.detailTreeActionAtCursorIndexed(ctx, search, state.focus, &state.relation_tree);
    switch (action) {
        .select => |target| {
            try state.pushFocusBack(state.focus);
            state.selected = findResultIndex(state.results.items, target) orelse state.selected;
            state.focus = target;
            state.org_scroll = 0;
            state.org_cursor = 0;
            state.message = "opened relation target; h goes back, C-o/Shift-Tab returns to search";
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

    const in_left = m.y >= lay.left.y + 1 and m.y + 1 < lay.left.bottom() and m.x >= lay.left.x and m.x < lay.left.right();
    const in_right = m.x >= lay.right.x and m.x < lay.right.right() and m.y >= lay.right.y and m.y < lay.right.bottom();

    if (m.kind == .unknown) return;

    if (m.kind == .scroll_up or m.kind == .scroll_down) {
        const delta: isize = if (m.kind == .scroll_up) -3 else 3;
        if (in_right) {
            state.active = .right;
            state.setRightHover(m.x, m.y);
            if (state.viewer_open) {
                state.scrollViewer(delta);
                state.message = "scrolled right document viewer";
            } else {
                state.scrollOrg(delta);
                state.message = "scrolled right Org content";
            }
            state.markScreenDirty();
        } else {
            state.active = .left;
            state.clearRightHover();
            state.scrollResults(delta);
            state.message = "scrolled results";
            state.markScreenDirty();
        }
        return;
    }

    if (m.kind == .drag) {
        if (in_right) {
            state.active = .right;
            state.setRightHover(m.x, m.y);
            if (!state.viewer_open) {
                if (render.detailTreeRowAtIndexed(ctx, search, lay.right, state.focus, &state.relation_tree, m.y)) |row| {
                    if (row < state.relation_tree.row_count) state.relation_tree.cursor = row;
                }
            }
            state.message = "hovering right pane; release opens underlined link/tree row";
        } else {
            state.clearRightHover();
            state.clearRightClick();
        }
        return;
    }

    if (m.kind == .release) {
        if (in_right and state.right_press) {
            state.active = .right;
            state.setRightHover(m.x, m.y);
            const armed_same_row = state.right_press_y == m.y;
            state.clearRightClick();
            if (!armed_same_row) {
                state.message = "right click canceled before release";
                state.markScreenDirty();
                return;
            }
            const text_action = render.detailTextActionAtIndexed(ctx, search, lay.right, state.focus, m.x, m.y, state.viewer_open, state.viewer_scroll, state.org_scroll);
            if (try performDetailTextAction(state, ctx, text_action)) return;
            if (!state.viewer_open) {
                const tree_rows = render.detailTreeVisibleRows(lay.right, ctx, search, state.focus);
                const row_count = render.detailTreeRowCountIndexed(ctx, search, state.focus, &state.relation_tree);
                state.relation_tree.sync(row_count, tree_rows);
                const action = render.detailHitTestIndexed(ctx, search, lay.right, state.focus, &state.relation_tree, m.y);
                try performTreeAction(state, ctx, action);
            }
            return;
        }
        state.clearRightClick();
        return;
    }

    if (in_left) {
        state.clearRightHover();
        state.clearRightClick();
        const row: usize = @intCast(m.y - lay.left.y - 1);
        const idx = state.scroll + @divTrunc(row, render.ResultRowHeight);
        if (idx < state.results.items.len) {
            state.selected = idx;
            state.focus = state.results.items[idx];
            state.org_scroll = 0;
            state.org_cursor = 0;
            state.active = .left;
            state.message = "selected result; Enter focuses it";
            state.markScreenDirty();
        }
    } else if (in_right) {
        state.active = .right;
        state.armRightClick(m.x, m.y);
        if (!state.viewer_open) {
            if (render.detailTreeRowAtIndexed(ctx, search, lay.right, state.focus, &state.relation_tree, m.y)) |row| {
                if (row < state.relation_tree.row_count) state.relation_tree.cursor = row;
            }
        }
        const text_action = render.detailTextActionAtIndexed(ctx, search, lay.right, state.focus, m.x, m.y, state.viewer_open, state.viewer_scroll, state.org_scroll);
        state.message = detailActionMessage(text_action);
        state.markScreenDirty();
    } else {
        state.clearRightHover();
        state.clearRightClick();
    }
}

fn performTreeAction(state: *ui.State, _: *const model.Context, action: tree.Action) !void {
    switch (action) {
        .select => |target| {
            try state.pushFocusBack(state.focus);
            state.selected = findResultIndex(state.results.items, target) orelse state.selected;
            state.focus = target;
            state.org_scroll = 0;
            state.org_cursor = 0;
            state.message = "opened relation target on mouse release; h goes back";
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
            state.message = "right pane focused; release on links/path buttons/tree rows to open";
            state.markScreenDirty();
        },
    }
}

fn performDetailTextAction(state: *ui.State, ctx: *const model.Context, action: render.DetailAction) !bool {
    switch (action) {
        .select => |target| {
            try state.pushFocusBack(state.focus);
            state.selected = findResultIndex(state.results.items, target) orelse state.selected;
            state.focus = target;
            state.org_scroll = 0;
            state.org_cursor = 0;
            state.message = "opened org id/link target";
            state.markScreenDirty();
            return true;
        },
        .open_file => |path| {
            try openFileSmart(state, ctx, path);
            return true;
        },
        .open_file_at => |loc| {
            try openEditorAtLine(state, ctx, loc.path, loc.line);
            return true;
        },
        .id => |id| {
            const q = try std.fmt.allocPrint(state.allocator, "?{s}", .{id});
            defer state.allocator.free(q);
            try state.quickQuery(q, "opened unresolved org id as ?id query");
            return true;
        },
        .tag => |tag| {
            const q = try std.fmt.allocPrint(state.allocator, "{s}", .{tag});
            defer state.allocator.free(q);
            try state.quickQuery(q, "opened Org tag from preview");
            return true;
        },
        .heading => |heading| {
            const q = try std.fmt.allocPrint(state.allocator, "title:{s}", .{heading});
            defer state.allocator.free(q);
            try state.quickQuery(q, "opened Org heading from preview");
            return true;
        },
        .query => |qtext| {
            try state.quickQuery(qtext, "opened org link as query");
            return true;
        },
        .viewer => {
            state.openViewer();
            return true;
        },
        .github => {
            try state.quickQuery("github README catface", "help g: GitHub/README surface");
            return true;
        },
        .none => return false,
    }
}

fn detailActionMessage(action: render.DetailAction) []const u8 {
    return switch (action) {
        .select => "armed object link; release to jump",
        .open_file => "armed file/path button; release to locate it",
        .open_file_at => "armed source location; release to open $EDITOR at line",
        .id => "armed org id link; release to query it",
        .tag => "armed Org tag button; release to search it",
        .heading => "armed Org heading; release to search this section",
        .query => "armed org query link; release to search it",
        .viewer => "armed document viewer",
        .github => "armed GitHub/README help link",
        .none => "right pane armed; drag away to cancel, release to activate tree/link row",
    };
}


fn openEditorAtLine(state: *ui.State, ctx: *const model.Context, path: []const u8, line: usize) !void {
    const clean = cleanPathTarget(path);
    const abs_path = if (std.fs.path.isAbsolute(clean))
        try state.allocator.dupe(u8, clean)
    else
        try std.fs.path.join(state.allocator, &.{ ctx.root, clean });
    defer state.allocator.free(abs_path);

    const editor = try editorCommand(state.allocator);
    defer state.allocator.free(editor);
    var argv = std.array_list.Managed([]const u8).init(state.allocator);
    defer argv.deinit();
    var toks = std.mem.tokenizeAny(u8, editor, " \t");
    while (toks.next()) |tok| try argv.append(tok);
    if (argv.items.len == 0) try argv.append("vi");
    const line_arg = try std.fmt.allocPrint(state.allocator, "+{d}", .{line});
    defer state.allocator.free(line_arg);
    try argv.append(line_arg);
    try argv.append(abs_path);

    var child = std.process.Child.init(argv.items, state.allocator);
    child.stdin_behavior = .Inherit;
    child.stdout_behavior = .Inherit;
    child.stderr_behavior = .Inherit;
    _ = child.spawnAndWait() catch |err| {
        state.message = "failed to open $EDITOR";
        state.markScreenDirty();
        return err;
    };
    state.message = "opened $EDITOR at file line";
    state.markScreenDirty();
}

fn editorCommand(allocator: std.mem.Allocator) ![]u8 {
    if (std.process.getEnvVarOwned(allocator, "EDITOR")) |value| {
        return value;
    } else |_| {}
    if (std.process.getEnvVarOwned(allocator, "VISUAL")) |value| {
        return value;
    } else |_| {}
    return allocator.dupe(u8, "vi");
}

fn openFileSmart(state: *ui.State, ctx: *const model.Context, path: []const u8) !void {
    const clean = cleanPathTarget(path);
    if (findObjectByPath(ctx, clean)) |target| {
        try state.pushFocusBack(state.focus);
        state.selected = findResultIndex(state.results.items, target) orelse state.selected;
        state.focus = target;
        state.org_scroll = 0;
        state.org_cursor = 0;
        state.message = "opened file location as category object";
        state.markScreenDirty();
        return;
    }
    const q = try std.fmt.allocPrint(state.allocator, "path:{s}", .{clean});
    defer state.allocator.free(q);
    try state.quickQuery(q, "file location not in current result set; searched path:");
}

fn cleanPathTarget(path: []const u8) []const u8 {
    var clean = std.mem.trim(u8, path, " \t\r\n");
    if (std.mem.startsWith(u8, clean, "file:")) clean = clean[5..];
    if (std.mem.indexOfScalar(u8, clean, ':')) |colon| {
        var numeric = colon + 1 < clean.len;
        var i = colon + 1;
        while (i < clean.len) : (i += 1) numeric = numeric and std.ascii.isDigit(clean[i]);
        if (numeric) clean = clean[0..colon];
    }
    return clean;
}

fn findObjectByPath(ctx: *const model.Context, path: []const u8) ?usize {
    for (ctx.objects.items, 0..) |obj, i| {
        if (std.mem.eql(u8, obj.path, path)) return i;
    }
    for (ctx.objects.items, 0..) |obj, i| {
        if (obj.path.len != 0 and (std.mem.endsWith(u8, obj.path, path) or std.mem.endsWith(u8, path, obj.path))) return i;
    }
    return null;
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

test "TAB opens completion, C-i jumps to info lane, and C-o switches pane" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "info.docs", .kind = .info, .title = "Info docs", .path = "context/info.org", .preview = "advanced docs" });
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .tab, 80, 24);
    try std.testing.expect(state.prompt_mode == .command_palette);
    state.closePalette();
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 9 }, 80, 24);
    try std.testing.expect(std.mem.eql(u8, state.query_buffer.items, "@info"));
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 15 }, 80, 24);
    try std.testing.expect(state.active == .right);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .alt = 'k' }, 80, 24);
    try std.testing.expect(std.mem.eql(u8, state.query_buffer.items, "@contracts"));
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .alt = 'y' }, 80, 24);
    try std.testing.expect(std.mem.eql(u8, state.query_buffer.items, "@quality"));
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .alt = 'a' }, 80, 24);
    try std.testing.expect(std.mem.eql(u8, state.query_buffer.items, "@metadata"));
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
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 14 }, 80, 24);
    try std.testing.expect(state.selected == 1);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 16 }, 80, 24);
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
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .char = 'h' }, 80, 24);
    try std.testing.expect(state.focus.? == 0);
}

test "right pane file target cleanup removes file prefix and line" {
    try std.testing.expect(std.mem.eql(u8, cleanPathTarget("file:context/a.org:42"), "context/a.org"));
    try std.testing.expect(std.mem.eql(u8, cleanPathTarget("context/a.org"), "context/a.org"));
}

test "RET on left enters right content mode instead of rewriting query" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "t", .kind = .test_kind, .title = "TEST", .path = "tests/t.mon", .preview = "TEST-ID: t" });
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    try state.setQuery(":Test");
    try state.refreshIndexed(&ctx, &search);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .enter, 80, 24);
    try std.testing.expect(state.active == .right);
    try std.testing.expect(!state.viewer_open);
    try std.testing.expect(std.mem.eql(u8, state.query_buffer.items, ":Test"));
}

test "C-h c describes the following key and C-c d opens directory prompt" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 8 }, 80, 24);
    try std.testing.expect(state.pending_chord == .ctrl_h);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .char = 'c' }, 80, 24);
    try std.testing.expect(state.pending_chord == .describe_key);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .tab, 80, 24);
    try std.testing.expect(state.pending_chord == .none);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 3 }, 80, 24);
    _ = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .char = 'd' }, 80, 24);
    try std.testing.expect(state.prompt_mode == .directory);
}

test "C-x C-c quits like Emacs" {
    var state = try ui.State.init(std.testing.allocator);
    defer state.deinit();
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var search = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer search.deinit();
    const first = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 24 }, 80, 24);
    try std.testing.expect(!first);
    try std.testing.expect(state.pending_chord == .ctrl_x);
    const second = try handleKey(&state, std.testing.allocator, &ctx, &search, .{ .ctrl = 3 }, 80, 24);
    try std.testing.expect(second);
}
