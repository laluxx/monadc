const std = @import("std");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const model = @import("model.zig");
const render = @import("render.zig");
const ui = @import("ui.zig");

pub fn run(allocator: std.mem.Allocator, ctx: *model.Context) !void {
    var tty = try terminal.Tty.init(allocator);
    defer tty.deinit();

    var state = try ui.State.init(allocator);
    defer state.deinit();
    try state.setQuery("@tests @info source");
    try state.refresh(ctx);

    while (true) {
        try draw(tty, ctx, &state);
        const key = try tty.poll(16);
        state.advanceClock(16);
        const done = try handleKey(&state, ctx, key, tty.width, tty.height);
        if (done) break;
        try state.refresh(ctx);
    }
}

fn draw(tty: *terminal.Tty, ctx: *const model.Context, state: *ui.State) !void {
    try tty.resize();
    const theme = palette.default_theme;
    tty.clear(.{ .fg = theme.ink, .bg = theme.bg });
    const lay = render.layout(tty.width, tty.height);
    render.drawHeader(tty, lay.header, state.query_buffer.items, state.cursor, state.cursorVisible(state.frame_ms), state.active == .left, ctx, theme);
    render.drawResults(tty, lay.left, ctx, state.results.items, state.selected, state.scroll, state.active == .left, theme);
    render.drawDetail(tty, lay.right, ctx, state.focus, state.active == .right, theme);
    render.drawFooter(tty, lay.footer, state.message, theme);
    if (state.show_tutorial) render.drawTutorial(tty, lay, theme);
    try tty.flush();
}

fn handleKey(state: *ui.State, ctx: *const model.Context, key: terminal.Key, width: u16, height: u16) !bool {
    switch (key) {
        .none => return false,
        .esc => return true,
        .tab => { state.active = if (state.active == .left) .right else .left; state.message = "switched pane"; },
        .up => state.move(-1),
        .down => state.move(1),
        .left => state.moveCursorLeft(),
        .right => state.moveCursorRight(),
        .home => state.beginningOfLine(),
        .end => state.endOfLine(),
        .page_up => state.move(-10),
        .page_down => state.move(10),
        .enter => try state.followFocused(ctx),
        .backspace => state.backspace(),
        .delete => state.deleteForward(),
        .mouse => |m| handleMouse(state, ctx, m, width, height) catch {},
        .alt => |c| switch (c) {
            'd', 'D' => try state.killWord(),
            else => {},
        },
        .ctrl => |c| switch (c) {
            1 => state.beginningOfLine(),       // C-a
            3 => return true,                  // C-c
            4 => state.deleteForward(),        // C-d
            5 => state.endOfLine(),            // C-e
            8 => state.backspace(),            // C-h
            11 => try state.killToEnd(),       // C-k
            14 => state.move(1),               // C-n
            16 => state.move(-1),              // C-p
            21 => { try state.setQuery(""); state.message = "cleared query"; }, // C-u
            25 => try state.yank(),            // C-y
            else => {},
        },
        .char => |cp| {
            switch (cp) {
                'q' => return true,
                'j' => state.move(1),
                'k' => state.move(-1),
                '/' => { state.active = .left; state.message = "type to query"; },
                'o' => try state.appendOp(">", "outgoing Hom(object, -)"),
                'i' => try state.appendOp("<", "incoming Hom(-, object)"),
                'n' => try state.appendOp("~", "neighborhood"),
                'p' => try state.appendOp("proj", "projection/superset"),
                'b' => try state.goBack(),
                '?' => state.toggleTutorial(),
                else => try state.appendUtf8(cp),
            }
        },
        else => {},
    }
    return false;
}

fn handleMouse(state: *ui.State, ctx: *const model.Context, m: terminal.MouseEvent, width: u16, height: u16) !void {
    _ = ctx;
    const lay = render.layout(width, height);
    if (m.y >= lay.left.y + 1 and m.y + 1 < lay.left.bottom() and m.x >= lay.left.x and m.x < lay.left.right()) {
        const row: usize = @intCast(m.y - lay.left.y - 1);
        const idx = state.scroll + row;
        if (idx < state.results.items.len) {
            state.selected = idx;
            state.focus = state.results.items[idx];
            state.active = .left;
            state.message = "mouse selected result; Enter follows";
        }
    } else if (m.x >= lay.right.x and m.x < lay.right.right() and m.y >= lay.right.y and m.y < lay.right.bottom()) {
        state.active = .right;
        state.message = "right pane focused; node buttons are keyboard-followable with Enter";
    }
}
