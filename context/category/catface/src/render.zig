const std = @import("std");
const model = @import("model.zig");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const glyphs = @import("glyphs.zig");
const math = @import("math.zig");
const version = @import("version.zig");
const tree = @import("tree.zig");
const search_index = @import("index.zig");
const perf = @import("perf.zig");

pub const ResultRowHeight: usize = 3;

pub const Layout = struct {
    header: terminal.Rect,
    left: terminal.Rect,
    right: terminal.Rect,
    footer: terminal.Rect,
};

pub fn layout(w: u16, h: u16) Layout {
    const header_h: u16 = if (h < 18) 3 else 5;
    const footer_h: u16 = 2;
    const body_h: u16 = if (h > header_h + footer_h) h - header_h - footer_h else 0;
    const left_w: u16 = if (w < 100) @divTrunc(w, 2) else @as(u16, @intCast(@divTrunc(@as(u32, w) * 40, 100)));
    return .{
        .header = .{ .x = 0, .y = 0, .w = w, .h = header_h },
        .left = .{ .x = 0, .y = header_h, .w = left_w, .h = body_h },
        .right = .{ .x = left_w, .y = header_h, .w = w - left_w, .h = body_h },
        .footer = .{ .x = 0, .y = if (h > footer_h) h - footer_h else 0, .w = w, .h = footer_h },
    };
}

pub fn resultVisibleRows(r: terminal.Rect) usize {
    const inner = r.inset(1);
    if (inner.h == 0) return 1;
    return @max(@as(usize, 1), @divTrunc(@as(usize, inner.h), ResultRowHeight));
}

pub fn detailTreeVisibleRows(r: terminal.Rect, ctx: *const model.Context, idx: ?*const search_index.SearchIndex, idx_opt: ?usize) usize {
    if (idx_opt == null) return 1;
    const inner = r.inset(1);
    if (inner.h == 0) return 1;
    const obj = ctx.objects.items[idx_opt.?];
    const graph = detailGraphRect(inner, ctx, idx, obj.id);
    if (graph.h <= 1) return 1;
    return @as(usize, graph.h - 1);
}

pub fn drawHeader(
    t: *terminal.Tty,
    r: terminal.Rect,
    query_text: []const u8,
    cursor: usize,
    cursor_visible: bool,
    active: bool,
    ctx: *const model.Context,
    result_count: usize,
    theme: palette.Theme,
) void {
    const bg_style = palette.Style{ .fg = theme.ink, .bg = theme.bg };
    t.fill(r, ' ', bg_style);

    var stats_buf: [320]u8 = undefined;
    const stats = std.fmt.bufPrint(&stats_buf, "v{s}  {d} objects  {d} arrows  {d} matches  H={d:.2}", .{
        version.version,
        ctx.objects.items.len,
        ctx.edges.items.len,
        result_count,
        math.objectEntropy(ctx),
    }) catch "";

    t.text(2, r.y, glyphs.logo, .{ .fg = theme.accent, .bg = theme.bg, .bold = true });
    t.text(17, r.y, version.codename, .{ .fg = theme.mute, .bg = theme.bg });
    if (@as(usize, r.w) > stats.len + 4) {
        t.textClipped(r.w - @as(u16, @intCast(stats.len)) - 2, r.y, @as(u16, @intCast(stats.len)), stats, .{ .fg = theme.mute, .bg = theme.bg });
    }

    if (r.h < 3) return;
    const query_y = r.y + 1;
    t.fill(.{ .x = 1, .y = query_y, .w = if (r.w > 2) r.w - 2 else r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    t.text(3, query_y, glyphs.query_prompt, .{ .fg = theme.accent2, .bg = theme.panel_alt, .bold = true });
    const qx: u16 = 6;
    const qwidth: u16 = if (r.w > qx + 4) r.w - qx - 4 else 0;
    if (query_text.len == 0) {
        t.textClipped(qx, query_y, qwidth, "Search: @todo, @hot, title:reader, @tests -> reader, %verifies @tests -> codegen", .{ .fg = theme.mute, .bg = theme.panel_alt, .italic = true });
    } else {
        t.textClipped(qx, query_y, qwidth, query_text, .{ .fg = theme.ink, .bg = theme.panel_alt, .bold = active });
    }
    if (active and cursor_visible and qwidth > 0) {
        const cx: u16 = @intCast(@min(@as(usize, qx + qwidth - 1), @as(usize, qx) + cursor));
        const cp: u21 = if (cursor < query_text.len) @as(u21, query_text[cursor]) else ' ';
        t.set(cx, query_y, cp, .{ .fg = theme.panel_alt, .bg = theme.ink, .bold = true });
    }

    if (r.h < 5) return;
    t.text(2, r.y + 2, "Alt-t", .{ .fg = theme.todo, .bg = theme.bg, .bold = true });
    t.text(8, r.y + 2, "@todo", .{ .fg = theme.ink, .bg = theme.bg });
    t.text(17, r.y + 2, "Alt-n", .{ .fg = theme.info, .bg = theme.bg, .bold = true });
    t.text(23, r.y + 2, "@notes", .{ .fg = theme.ink, .bg = theme.bg });
    t.text(33, r.y + 2, "Alt-e", .{ .fg = theme.test_color, .bg = theme.bg, .bold = true });
    t.text(39, r.y + 2, "@tests", .{ .fg = theme.ink, .bg = theme.bg });
    t.text(49, r.y + 2, "Alt-s", .{ .fg = theme.script, .bg = theme.bg, .bold = true });
    t.text(55, r.y + 2, "@source", .{ .fg = theme.ink, .bg = theme.bg });
    t.text(67, r.y + 2, "syntax: lhs -> rhs   lhs <- rhs   %verifies", .{ .fg = theme.mute, .bg = theme.bg });

    t.fill(.{ .x = 0, .y = r.y + 3, .w = r.w, .h = 1 }, '─', .{ .fg = theme.edge, .bg = theme.bg });
    var counts_buf: [320]u8 = undefined;
    const counts = std.fmt.bufPrint(&counts_buf, "TODO {d}  TEST {d}  NOTE {d}  SOURCE {d}  RECORD {d}  BUG/FAIL {d}", .{
        countKind(ctx, .todo),
        countKind(ctx, .test_kind),
        countNamespace(ctx, "notes"),
        countKind(ctx, .source) + countKind(ctx, .script),
        countKind(ctx, .record),
        countBugs(ctx),
    }) catch "";
    t.textClipped(2, r.y + 4, if (r.w > 4) r.w - 4 else 0, counts, .{ .fg = theme.mute, .bg = theme.bg });
}

pub fn drawFooter(t: *terminal.Tty, r: terminal.Rect, msg: []const u8, stats: perf.Stats, theme: palette.Theme) void {
    t.fill(r, ' ', .{ .fg = theme.mute, .bg = theme.bg });
    if (r.h == 0) return;
    var perf_buf: [240]u8 = undefined;
    const perf_line = std.fmt.bufPrint(&perf_buf, "frame {d}ns  query {d}ns  flush {d}ns  redraws {d}  cached {d}", .{
        stats.last_frame_ns,
        stats.last_query_ns,
        stats.last_flush_ns,
        stats.redraws,
        stats.cached_refreshes,
    }) catch "frame/query timing unavailable";
    const left = "Type to search · a -> b / a <- b · %edge-kind · scroll/click tree · ? help";
    t.textClipped(2, r.y, if (r.w > 4) r.w - 4 else 0, left, .{ .fg = theme.mute, .bg = theme.bg });
    if (r.w > 72) {
        const pw: u16 = @intCast(@min(perf_line.len, @as(usize, r.w - 4)));
        if (pw + 2 < r.w) t.textClipped(r.right() - pw - 2, r.y, pw, perf_line, .{ .fg = theme.accent2, .bg = theme.bg });
    }
    if (r.h > 1) t.textClipped(2, r.y + 1, if (r.w > 4) r.w - 4 else 0, msg, .{ .fg = theme.warn, .bg = theme.bg, .bold = true });
}

pub fn drawResults(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, results: []const usize, selected: usize, scroll: usize, active: bool, theme: palette.Theme) void {
    var title_buf: [96]u8 = undefined;
    const title = std.fmt.bufPrint(&title_buf, " {d} ranked objects ", .{results.len}) catch " ranked objects ";
    t.box(r, title, active, theme);
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0) return;

    if (results.len == 0) {
        drawEmptyResults(t, inner, theme);
        return;
    }

    var y = inner.y;
    var i = scroll;
    while (i < results.len and y < inner.bottom()) : (i += 1) {
        if (y + 1 >= inner.bottom()) break;
        const idx = results[i];
        const obj = ctx.objects.items[idx];
        const is_sel = i == selected;
        const bg = if (is_sel) theme.panel_alt else theme.panel;
        const bar = if (is_sel) theme.accent else theme.edge;
        const row_h: u16 = @intCast(@min(@as(usize, ResultRowHeight), @as(usize, inner.bottom() - y)));
        t.fill(.{ .x = inner.x, .y = y, .w = inner.w, .h = row_h }, ' ', .{ .fg = theme.ink, .bg = bg });
        t.set(inner.x, y, if (is_sel) '▌' else ' ', .{ .fg = bar, .bg = bg, .bold = is_sel });

        var head_buf: [128]u8 = undefined;
        const head = std.fmt.bufPrint(&head_buf, "{s} {s}", .{ glyphs.kind(obj.kind), objectBadge(obj) }) catch objectBadge(obj);
        const head_color = objectBadgeColor(obj, theme);
        const head_w: u16 = @intCast(@min(@as(usize, 10), head.len));
        t.textClipped(inner.x + 3, y, head_w, head, .{ .fg = head_color, .bg = bg, .bold = true });
        const title_x = inner.x + 3 + head_w + 2;
        if (title_x < inner.right()) {
            const title_text = if (obj.title.len != 0) obj.title else obj.id;
            t.textClipped(title_x, y, inner.right() - title_x - 1, title_text, .{ .fg = if (is_sel) theme.ink else theme.mute, .bg = bg, .bold = is_sel and active });
        }

        var meta_buf: [768]u8 = undefined;
        const meta = std.fmt.bufPrint(&meta_buf, "{s}:{d}  {s}", .{ obj.path, obj.line, obj.id }) catch obj.id;
        if (y + 1 < inner.bottom()) t.textClipped(inner.x + 3, y + 1, if (inner.w > 5) inner.w - 5 else 0, meta, .{ .fg = theme.mute, .bg = bg });
        if (y + 2 < inner.bottom()) {
            const preview = if (obj.preview.len != 0) obj.preview else objectOneLineMeaning(obj);
            t.textClipped(inner.x + 3, y + 2, if (inner.w > 5) inner.w - 5 else 0, preview, .{ .fg = if (is_sel) theme.ink.scale(80, 100) else theme.mute.scale(82, 100), .bg = bg, .dim = !is_sel });
        }
        y += @as(u16, @intCast(ResultRowHeight));
    }
}


fn drawKindBubble(t: *terminal.Tty, x: u16, y: u16, obj: model.Object, bg: palette.Color, theme: palette.Theme) u16 {
    const label = objectBadge(obj);
    const color = objectBadgeColor(obj, theme);
    t.text(x, y, "", .{ .fg = color, .bg = bg, .bold = true });
    const label_w: u16 = @intCast(@min(label.len, 16));
    t.textClipped(x + 1, y, label_w, label, .{ .fg = theme.bg, .bg = color, .bold = true });
    t.text(x + 1 + label_w, y, "", .{ .fg = color, .bg = bg, .bold = true });
    return x + label_w + 2;
}

fn objectBadge(obj: model.Object) []const u8 {
    if (obj.kind == .record) return shortRecordClass(obj);
    return switch (obj.kind) {
        .test_kind => "TEST",
        .todo => "TODO",
        .done => "DONE",
        .source => "SRC",
        .function_kind => "FN",
        .script => "SCRIPT",
        .info, .heading => "NOTE",
        .concept => "CONCEPT",
        .file => "FILE",
        .report => "REPORT",
        .record => "REC",
        .unknown => "OBJ",
    };
}

fn objectBadgeColor(obj: model.Object, theme: palette.Theme) palette.Color {
    if (obj.kind == .record) {
        const cls = shortRecordClass(obj);
        if (std.mem.eql(u8, cls, "OBS")) return theme.obs;
        if (std.mem.eql(u8, cls, "DEC")) return theme.accent;
        if (std.mem.eql(u8, cls, "INF")) return theme.warn;
        if (std.mem.eql(u8, cls, "FIX")) return theme.bad;
        return theme.record;
    }
    if (obj.kind == .info or obj.kind == .heading) return theme.info;
    if (obj.kind == .function_kind) return theme.function_color;
    return theme.kindColor(obj.kind);
}

fn shortRecordClass(obj: model.Object) []const u8 {
    if (hasToken(obj.title, "OBS") or hasToken(obj.preview, "[OBS") or hasToken(obj.preview, "OBS")) return "OBS";
    if (hasToken(obj.title, "DEC") or hasToken(obj.preview, "[DEC") or hasToken(obj.preview, "DEC")) return "DEC";
    if (hasToken(obj.title, "INF") or hasToken(obj.preview, "[INF") or hasToken(obj.preview, "INF")) return "INF";
    if (hasToken(obj.title, "FIX") or hasToken(obj.preview, "FIX")) return "FIX";
    return "REC";
}

fn drawEmptyResults(t: *terminal.Tty, r: terminal.Rect, theme: palette.Theme) void {
    t.text(r.x + 2, r.y + 1, "No matches.", .{ .fg = theme.bad, .bg = theme.panel, .bold = true });
    t.textClipped(r.x + 2, r.y + 3, if (r.w > 4) r.w - 4 else 0, "Try @todo, @hot, @blocked, title:reader, @roots, @reader, :Record, %verifies, or a -> b.", .{ .fg = theme.mute, .bg = theme.panel });
}

pub fn drawDetail(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx_opt: ?usize, tree_state: *const tree.State, active: bool, theme: palette.Theme) void {
    drawDetailWithIndex(t, r, ctx, null, idx_opt, tree_state, &.{}, active, theme);
}

pub fn drawDetailIndexed(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx: *const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State, focus_stack: []const usize, active: bool, theme: palette.Theme) void {
    drawDetailWithIndex(t, r, ctx, idx, idx_opt, tree_state, focus_stack, active, theme);
}

fn drawDetailWithIndex(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State, focus_stack: []const usize, active: bool, theme: palette.Theme) void {
    t.box(r, detailTitle(ctx, idx_opt, active), active, theme);
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0) return;
    if (idx_opt == null) {
        drawDashboard(t, inner, ctx, theme);
        return;
    }

    const idx = idx_opt.?;
    const obj = ctx.objects.items[idx];
    const graph = detailGraphRect(inner, ctx, sidx_opt, obj.id);
    const text_bottom: u16 = if (graph.h > 0 and graph.y > inner.y) graph.y - 1 else inner.bottom();
    const text_h: u16 = if (text_bottom > inner.y) text_bottom - inner.y else 0;
    const text_rect = terminal.Rect{ .x = inner.x, .y = inner.y, .w = inner.w, .h = text_h };

    if (text_rect.h > 0) {
        var y = text_rect.y;
        y = drawIdentity(t, text_rect, y, ctx, focus_stack, idx, obj, theme);
        if (y < text_rect.bottom()) {
            y += 1;
            _ = drawObjectText(t, text_rect, y, ctx, sidx_opt, obj, theme);
        }
    }
    if (graph.h > 0) drawRelationTree(t, graph, ctx, sidx_opt, obj.id, tree_state, active, theme);
}

pub fn detailHitTest(ctx: *const model.Context, r: terminal.Rect, idx_opt: ?usize, tree_state: *const tree.State, mouse_y: u16) tree.Action {
    return detailHitTestWithIndex(ctx, null, r, idx_opt, tree_state, mouse_y);
}

pub fn detailHitTestIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, r: terminal.Rect, idx_opt: ?usize, tree_state: *const tree.State, mouse_y: u16) tree.Action {
    return detailHitTestWithIndex(ctx, idx, r, idx_opt, tree_state, mouse_y);
}

fn detailHitTestWithIndex(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, r: terminal.Rect, idx_opt: ?usize, tree_state: *const tree.State, mouse_y: u16) tree.Action {
    if (idx_opt == null) return .none;
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0) return .none;
    const obj = ctx.objects.items[idx_opt.?];
    const graph = detailGraphRect(inner, ctx, sidx_opt, obj.id);
    if (graph.h <= 1 or mouse_y <= graph.y or mouse_y >= graph.bottom()) return .none;
    const content = terminal.Rect{ .x = graph.x, .y = graph.y + 1, .w = graph.w, .h = graph.h - 1 };
    const target_row: usize = tree_state.scroll + @as(usize, mouse_y - content.y);
    var row: usize = 0;
    if (hitDirection(ctx, sidx_opt, obj.id, .out, tree_state, target_row, &row)) |a| return a;
    if (hitDirection(ctx, sidx_opt, obj.id, .in, tree_state, target_row, &row)) |a| return a;
    return .none;
}

pub fn detailTreeRowCountIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State) usize {
    return detailTreeRowCountWithIndex(ctx, idx, idx_opt, tree_state);
}

pub fn detailTreeActionAtCursorIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State) tree.Action {
    if (idx_opt == null) return .none;
    var row: usize = 0;
    const obj = ctx.objects.items[idx_opt.?];
    if (hitDirection(ctx, idx, obj.id, .out, tree_state, tree_state.cursor, &row)) |a| return a;
    if (hitDirection(ctx, idx, obj.id, .in, tree_state, tree_state.cursor, &row)) |a| return a;
    return .none;
}

fn detailTreeRowCountWithIndex(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State) usize {
    if (idx_opt == null) return 0;
    const id = ctx.objects.items[idx_opt.?].id;
    return directionRowCount(ctx, sidx_opt, id, .out, tree_state) + directionRowCount(ctx, sidx_opt, id, .in, tree_state);
}

fn directionRowCount(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, dir: tree.Direction, tree_state: *const tree.State) usize {
    var n: usize = 1;
    if (!tree_state.dirOpen(dir)) return n;
    const total = countEdges(ctx, sidx_opt, id, dir, null);
    if (total == 0) return n + 1;
    for (tree.edge_kinds) |kind| {
        const group_total = countEdges(ctx, sidx_opt, id, dir, kind);
        if (group_total == 0) continue;
        n += 1;
        if (tree_state.groupOpen(dir, kind)) {
            const shown = @min(group_total, 32);
            n += shown;
            if (group_total > shown) n += 1;
        }
    }
    return n;
}

fn detailTitle(ctx: *const model.Context, idx_opt: ?usize, active: bool) []const u8 {
    _ = active;
    if (idx_opt) |idx| {
        return switch (ctx.objects.items[idx].kind) {
            .test_kind => " test contract + relation tree ",
            .todo => " TODO work item + blockers/support ",
            .done => " DONE evidence + supersession links ",
            .record => " context record + trust links ",
            .source, .script => " source surface + verifying tests ",
            .function_kind => " function type + callers/links ",
            .concept => " concept object + categorical neighborhood ",
            .info, .heading => " note text + context links ",
            .file, .report => " artifact text + contained objects ",
            .unknown => " object text + relation tree ",
        };
    }
    return " Catface cockpit ";
}

fn drawIdentity(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, focus_stack: []const usize, current_idx: usize, obj: model.Object, theme: palette.Theme) u16 {
    var y = y0;
    if (y >= r.bottom()) return y;
    const bx = drawKindBubble(t, r.x, y, obj, theme.panel, theme);
    if (bx + 2 < r.right()) t.textClipped(bx + 2, y, r.right() - bx - 2, model.Context.kindName(obj.kind), .{ .fg = theme.mute, .bg = theme.panel, .bold = true });
    if (r.w > 52) t.textClipped(r.right() - 47, y, 46, "tree: n/p move · l opens · h backs", .{ .fg = theme.mute.scale(78, 100), .bg = theme.panel });
    y += 1;

    y = drawFocusChain(t, r, y, ctx, focus_stack, current_idx, theme);
    const title = if (obj.title.len != 0) obj.title else obj.id;
    t.textClipped(r.x, y, r.w, title, .{ .fg = theme.ink, .bg = theme.panel, .bold = true });
    y += 1;
    t.textClipped(r.x, y, r.w, obj.id, .{ .fg = theme.mute, .bg = theme.panel });
    y += 1;
    var meta_buf: [768]u8 = undefined;
    const meta = std.fmt.bufPrint(&meta_buf, "@{s}:{d}  {s}", .{ obj.path, obj.line, objectMetaHint(obj) }) catch "";
    t.textClipped(r.x, y, r.w, meta, .{ .fg = theme.mute.scale(82, 100), .bg = theme.panel });
    return y + 1;
}

fn drawFocusChain(t: *terminal.Tty, r: terminal.Rect, y: u16, ctx: *const model.Context, focus_stack: []const usize, current_idx: usize, theme: palette.Theme) u16 {
    if (y >= r.bottom()) return y;
    if (focus_stack.len == 0) return y;
    var buf: [1024]u8 = undefined;
    var len: usize = 0;
    const start = if (focus_stack.len > 4) focus_stack.len - 4 else 0;
    var i = start;
    while (i < focus_stack.len) : (i += 1) {
        const idx = focus_stack[i];
        if (idx >= ctx.objects.items.len) continue;
        const o = ctx.objects.items[idx];
        if (i != start) appendFixed(&buf, &len, " -> ");
        appendFixed(&buf, &len, shortObjectName(o));
    }
    appendFixed(&buf, &len, " -> ");
    appendFixed(&buf, &len, shortObjectName(ctx.objects.items[current_idx]));
    const chain = buf[0..len];
    t.textClipped(r.x, y, r.w, chain, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    return y + 1;
}

fn appendFixed(buf: []u8, len: *usize, bytes: []const u8) void {
    if (len.* >= buf.len) return;
    const n = @min(bytes.len, buf.len - len.*);
    @memcpy(buf[len.* .. len.* + n], bytes[0..n]);
    len.* += n;
}

fn shortObjectName(obj: model.Object) []const u8 {
    if (obj.title.len != 0) return obj.title;
    return obj.id;
}

fn objectMetaHint(obj: model.Object) []const u8 {
    if (obj.kind == .function_kind) return obj.preview;
    if (obj.tags.len != 0) return obj.tags;
    return "∅";
}

fn drawObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme) u16 {
    var y = y0;
    const title = objectTextTitle(obj);
    y = drawSection(t, r, y, title, theme.kindColor(obj.kind), theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "does", objectDoes(obj), theme);
    y = drawKV(t, r, y, "why", objectWhy(obj), theme);
    const display_text = objectDisplayText(obj);
    if (display_text.len != 0) y = drawKV(t, r, y, "text", display_text, theme);
    if (obj.title.len != 0 and !std.mem.eql(u8, obj.title, obj.preview)) y = drawKV(t, r, y, "title", obj.title, theme);
    if (obj.path.len != 0) y = drawKV(t, r, y, "open", obj.path, theme);
    return y;
}

fn drawSignalRow(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme) u16 {
    const y = y0;
    if (y >= r.bottom()) return y;
    const out_n = if (sidx_opt) |idx| idx.outgoing(obj.id).len else countEdges(ctx, null, obj.id, .out, null);
    const in_n = if (sidx_opt) |idx| idx.incoming(obj.id).len else countEdges(ctx, null, obj.id, .in, null);
    var buf: [256]u8 = undefined;
    const signal = std.fmt.bufPrint(&buf, "kind {s}  trust {s}  OUT {d}  IN {d}  weight {d}", .{ model.Context.kindName(obj.kind), recordClass(obj), out_n, in_n, obj.weight }) catch "object signals";
    t.textClipped(r.x, y, r.w, signal, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    return y + 1;
}

fn drawSection(t: *terminal.Tty, r: terminal.Rect, y0: u16, title: []const u8, color: palette.Color, theme: palette.Theme) u16 {
    var y = y0;
    if (y >= r.bottom()) return y;
    t.textClipped(r.x, y, r.w, title, .{ .fg = color, .bg = theme.panel, .bold = true });
    y += 1;
    if (y < r.bottom()) {
        t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, '─', .{ .fg = theme.edge, .bg = theme.panel });
        y += 1;
    }
    return y;
}

fn drawKV(t: *terminal.Tty, r: terminal.Rect, y0: u16, key: []const u8, value: []const u8, theme: palette.Theme) u16 {
    var y = y0;
    if (y >= r.bottom() or value.len == 0) return y;
    var label_buf: [32]u8 = undefined;
    const label = std.fmt.bufPrint(&label_buf, "{s}", .{key}) catch key;
    const label_w: u16 = if (r.w > 14) 9 else @min(r.w, @as(u16, 6));
    t.textClipped(r.x, y, label_w, label, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    if (r.w <= label_w + 1) return y + 1;
    const text_x = r.x + label_w + 1;
    const text_w = r.right() - text_x;
    y = drawWrapped(t, text_x, y, text_w, r.bottom(), value, .{ .fg = theme.ink, .bg = theme.panel });
    return y;
}

fn objectTextTitle(obj: model.Object) []const u8 {
    return switch (obj.kind) {
        .test_kind => "TEST: WHAT / WHY / EXPECTATION",
        .todo => "TODO: NEXT ACTION",
        .done => "DONE: COMPLETED EVIDENCE",
        .record => "RECORD: CLAIM AND TRUST LEVEL",
        .source, .script => "SOURCE: IMPLEMENTATION SURFACE",
        .function_kind => "FUNCTION: TYPE AND DEFINITION",
        .concept => "CONCEPT: CATEGORY OBJECT",
        .info, .heading => "NOTE: CONTEXT TEXT",
        .file, .report => "ARTIFACT: FILE SURFACE",
        .unknown => "OBJECT TEXT",
    };
}

fn objectDoes(obj: model.Object) []const u8 {
    const body = objectDisplayText(obj);
    return switch (obj.kind) {
        .test_kind => chooseFirstNonEmpty(body, "compiler regression/contract test"),
        .todo => chooseFirstNonEmpty(body, "open work item"),
        .done => chooseFirstNonEmpty(body, "completed invariant/evidence"),
        .record => chooseFirstNonEmpty(body, obj.title),
        .source, .script => chooseFirstNonEmpty(body, "source/tooling object"),
        .function_kind => chooseFirstNonEmpty(functionSignatureText(obj), "first-class function/type arrow"),
        .concept => chooseFirstNonEmpty(body, "semantic object in the context category"),
        .info, .heading => chooseFirstNonEmpty(body, "documentation/context note"),
        .file, .report => chooseFirstNonEmpty(body, "file/report level artifact"),
        .unknown => chooseFirstNonEmpty(body, obj.title),
    };
}

fn objectDisplayText(obj: model.Object) []const u8 {
    if (obj.kind == .function_kind) return functionSignatureText(obj);
    var text_slice = std.mem.trim(u8, obj.preview, " \t\r\n");
    if (text_slice.len == 0) return "";
    if (text_slice[0] == '[') {
        if (std.mem.indexOfScalar(u8, text_slice, ']')) |end| {
            text_slice = std.mem.trim(u8, text_slice[end + 1 ..], " \t-:;\r\n");
            if (text_slice.len != 0) return text_slice;
        }
        return obj.title;
    }
    if (std.mem.startsWith(u8, text_slice, "#+") or text_slice[0] == ':') return obj.title;
    return text_slice;
}

fn functionSignatureText(obj: model.Object) []const u8 {
    const preview = std.mem.trim(u8, obj.preview, " \t\r\n");
    if (preview.len != 0) return preview;
    if (obj.title.len != 0) return obj.title;
    return obj.id;
}

fn objectWhy(obj: model.Object) []const u8 {
    return switch (obj.kind) {
        .test_kind => expectationHint(obj),
        .todo => "Use the bottom relation tree to see blockers, supporting notes, and source targets before editing.",
        .done => "Treat as useful evidence unless a supersedes/refines edge points to newer truth.",
        .record => recordTrust(recordClass(obj)),
        .source, .script => "Find tests and records connected to this source before changing behavior.",
        .function_kind => "Search by name or type signature; connected source tells where the definition lives.",
        .concept => "Use relation queries and projection to move between concrete tests/source and this abstraction.",
        .info, .heading => "Useful project memory; trust it more when linked to tests/source/OBS records.",
        .file, .report => "Navigate contained or linked objects for precise facts.",
        .unknown => "Unclassified object; use edges and fuzzy search to discover its role.",
    };
}

fn detailGraphRect(inner: terminal.Rect, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8) terminal.Rect {
    const h = detailGraphHeight(inner, ctx, sidx_opt, id);
    if (h == 0 or inner.h <= h) return .{ .x = inner.x, .y = inner.y, .w = inner.w, .h = h };
    return .{ .x = inner.x, .y = inner.bottom() - h, .w = inner.w, .h = h };
}

fn detailGraphHeight(inner: terminal.Rect, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8) u16 {
    if (inner.h < 7) return 0;
    const rels = relationTotal(ctx, sidx_opt, id);
    var desired: u16 = @intCast(@min(rels + 6, 24));
    if (desired < 8) desired = 8;
    const cap: u16 = if (inner.h > 28) @as(u16, @intCast(@divTrunc(@as(u32, inner.h) * 45, 100))) else if (inner.h > 14) inner.h - 6 else @divTrunc(inner.h, 2);
    if (desired > cap) desired = cap;
    return desired;
}

fn relationTotal(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8) usize {
    if (sidx_opt) |idx| return idx.outgoing(id).len + idx.incoming(id).len;
    var n: usize = 0;
    for (ctx.edges.items) |e| {
        if (std.mem.eql(u8, e.src, id) or std.mem.eql(u8, e.dst, id)) n += 1;
    }
    return n;
}

fn drawRelationTree(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, tree_state: *const tree.State, active: bool, theme: palette.Theme) void {
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel });
    if (r.h == 0) return;
    var title_buf: [220]u8 = undefined;
    const title = std.fmt.bufPrint(&title_buf, "RELATION TREE  row {d}/{d}  T test S src λ fn ▣ rec I note ! todo · ✓ verify ⊢ support ⊣ block ≤ refine ⇢ link", .{ tree_state.cursor + 1, tree_state.row_count }) catch "RELATION TREE";
    t.textClipped(r.x, r.y, r.w, title, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    if (r.h <= 1) return;
    const content = terminal.Rect{ .x = r.x, .y = r.y + 1, .w = r.w, .h = r.h - 1 };
    var y = content.y;
    var row: usize = 0;
    drawDirection(t, content, &y, &row, tree_state.scroll, tree_state.cursor, active, ctx, sidx_opt, id, .out, tree_state, theme);
    drawDirection(t, content, &y, &row, tree_state.scroll, tree_state.cursor, active, ctx, sidx_opt, id, .in, tree_state, theme);
}

fn drawDirection(t: *terminal.Tty, r: terminal.Rect, y: *u16, row: *usize, scroll: usize, cursor: usize, active: bool, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, dir: tree.Direction, tree_state: *const tree.State, theme: palette.Theme) void {
    const total = countEdges(ctx, sidx_opt, id, dir, null);
    var buf: [160]u8 = undefined;
    const chev = if (tree_state.dirOpen(dir)) "▾" else "▸";
    const line = std.fmt.bufPrint(&buf, "{s} {s}  {s}  {d} arrows", .{ chev, tree.directionName(dir), tree.directionSignature(dir), total }) catch tree.directionName(dir);
    drawLogicalRow(t, r, y, row, scroll, cursor, active, 0, line, .{ .fg = theme.accent, .bg = theme.panel, .bold = true }, theme);
    if (!tree_state.dirOpen(dir)) return;
    if (total == 0) {
        drawLogicalRow(t, r, y, row, scroll, cursor, active, 2, "∅ no arrows in this direction", .{ .fg = theme.edge, .bg = theme.panel }, theme);
        return;
    }
    for (tree.edge_kinds) |kind| {
        const group_total = countEdges(ctx, sidx_opt, id, dir, kind);
        if (group_total == 0) continue;
        drawGroup(t, r, y, row, scroll, cursor, active, ctx, sidx_opt, id, dir, kind, group_total, tree_state, theme);
    }
}

fn drawGroup(t: *terminal.Tty, r: terminal.Rect, y: *u16, row: *usize, scroll: usize, cursor: usize, active: bool, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, dir: tree.Direction, kind: model.EdgeKind, total: usize, tree_state: *const tree.State, theme: palette.Theme) void {
    var buf: [160]u8 = undefined;
    const open = tree_state.groupOpen(dir, kind);
    const chev = if (open) "▾" else "▸";
    const line = std.fmt.bufPrint(&buf, "  {s} {s} [{s}] {s}  {d}", .{ chev, glyphs.edge(kind), edgeBadge(kind), model.Context.edgeName(kind), total }) catch model.Context.edgeName(kind);
    drawLogicalRow(t, r, y, row, scroll, cursor, active, 0, line, .{ .fg = edgeColor(kind, theme), .bg = theme.panel, .bold = true }, theme);
    if (!open) return;
    var shown: usize = 0;
    if (sidx_opt) |idx| {
        for (edgeIndicesForDir(idx, id, dir)) |edge_idx| {
            const e = ctx.edges.items[edge_idx];
            if (e.kind != kind) continue;
            if (shown >= 32) break;
            drawRelationTargetRow(t, r, y, row, scroll, cursor, active, ctx, dir, e, theme);
            shown += 1;
        }
    } else {
        for (ctx.edges.items) |e| {
            if (!edgeMatches(e, id, dir, kind)) continue;
            if (shown >= 32) break;
            drawRelationTargetRow(t, r, y, row, scroll, cursor, active, ctx, dir, e, theme);
            shown += 1;
        }
    }
    if (total > shown) {
        var more_buf: [96]u8 = undefined;
        const more = std.fmt.bufPrint(&more_buf, "  │  └─ +{d} more; refine with %{s} or scroll", .{ total - shown, model.Context.edgeName(kind) }) catch "  │  └─ more";
        drawLogicalRow(t, r, y, row, scroll, cursor, active, 0, more, .{ .fg = theme.mute, .bg = theme.panel }, theme);
    }
}


fn drawRelationTargetRow(t: *terminal.Tty, r: terminal.Rect, y: *u16, row: *usize, scroll: usize, cursor: usize, active: bool, ctx: *const model.Context, dir: tree.Direction, e: model.Edge, theme: palette.Theme) void {
    const other_id = switch (dir) { .out => e.dst, .in => e.src };
    var other_title: []const u8 = other_id;
    var other_kind: model.ObjectKind = .unknown;
    if (ctx.findObject(other_id)) |oi| {
        const o = ctx.objects.items[oi];
        other_title = if (o.title.len != 0) o.title else o.id;
        other_kind = o.kind;
    }
    var line_buf: [1024]u8 = undefined;
    const target_line = std.fmt.bufPrint(&line_buf, "  │  ├─ {s} {s} {s}  {s}", .{ glyphs.edge(e.kind), glyphs.kind(other_kind), relationTargetBadge(ctx, other_id, e.kind), other_title }) catch other_id;
    drawLogicalRow(t, r, y, row, scroll, cursor, active, 0, target_line, .{ .fg = theme.mute, .bg = theme.panel, .dim = !active }, theme);
}

fn drawLogicalRow(t: *terminal.Tty, r: terminal.Rect, y: *u16, row: *usize, scroll: usize, cursor: usize, active: bool, indent: u16, row_text: []const u8, style: palette.Style, theme: palette.Theme) void {
    if (row.* >= scroll and y.* < r.bottom()) {
        const selected = row.* == cursor;
        const bg = if (selected) theme.panel_alt else theme.panel;
        var row_style = style;
        row_style.bg = bg;
        row_style.bold = row_style.bold or (selected and active);
        t.fill(.{ .x = r.x, .y = y.*, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = bg });
        if (selected) t.set(r.x, y.*, if (active) '▌' else '│', .{ .fg = theme.accent, .bg = bg, .bold = true });
        const x = r.x + @min(indent + 1, r.w);
        const w = if (r.right() > x) r.right() - x else 0;
        t.textClipped(x, y.*, w, row_text, row_style);
        y.* += 1;
    }
    row.* += 1;
}

fn hitDirection(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, dir: tree.Direction, tree_state: *const tree.State, target_row: usize, row: *usize) ?tree.Action {
    _ = countEdges(ctx, sidx_opt, id, dir, null);
    if (row.* == target_row) return tree.Action{ .toggle_dir = dir };
    row.* += 1;
    if (!tree_state.dirOpen(dir)) return null;
    if (countEdges(ctx, sidx_opt, id, dir, null) == 0) {
        row.* += 1;
        return null;
    }
    for (tree.edge_kinds) |kind| {
        const group_total = countEdges(ctx, sidx_opt, id, dir, kind);
        if (group_total == 0) continue;
        if (row.* == target_row) return tree.Action{ .toggle_group = .{ .dir = dir, .kind = kind } };
        row.* += 1;
        if (!tree_state.groupOpen(dir, kind)) continue;
        var shown: usize = 0;
        if (sidx_opt) |idx| {
            for (edgeIndicesForDir(idx, id, dir)) |edge_idx| {
                const e = ctx.edges.items[edge_idx];
                if (e.kind != kind) continue;
                if (shown >= 32) break;
                const other_id = switch (dir) { .out => e.dst, .in => e.src };
                if (row.* == target_row) {
                    if (ctx.findObject(other_id)) |target| return tree.Action{ .select = target };
                    return .none;
                }
                row.* += 1;
                shown += 1;
            }
        } else {
            for (ctx.edges.items) |e| {
                if (!edgeMatches(e, id, dir, kind)) continue;
                if (shown >= 32) break;
                const other_id = switch (dir) { .out => e.dst, .in => e.src };
                if (row.* == target_row) {
                    if (ctx.findObject(other_id)) |target| return tree.Action{ .select = target };
                    return .none;
                }
                row.* += 1;
                shown += 1;
            }
        }
        if (group_total > shown) row.* += 1;
    }
    return null;
}

fn countEdges(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, id: []const u8, dir: tree.Direction, kind_opt: ?model.EdgeKind) usize {
    if (sidx_opt) |idx| {
        if (ctx.findObject(id)) |object_index| {
            if (kind_opt) |k| {
                return switch (dir) {
                    .out => idx.outgoingKindCount(object_index, k),
                    .in => idx.incomingKindCount(object_index, k),
                };
            }
            return switch (dir) {
                .out => idx.outgoingTotal(object_index),
                .in => idx.incomingTotal(object_index),
            };
        }
        return 0;
    }
    var n: usize = 0;
    for (ctx.edges.items) |e| {
        if (kind_opt) |k| {
            if (e.kind != k) continue;
        }
        if (edgeMatchesDirection(e, id, dir)) n += 1;
    }
    return n;
}

fn edgeIndicesForDir(idx: *const search_index.SearchIndex, id: []const u8, dir: tree.Direction) []const usize {
    return switch (dir) {
        .out => idx.outgoing(id),
        .in => idx.incoming(id),
    };
}

fn edgeMatches(e: model.Edge, id: []const u8, dir: tree.Direction, kind: model.EdgeKind) bool {
    return e.kind == kind and edgeMatchesDirection(e, id, dir);
}

fn edgeMatchesDirection(e: model.Edge, id: []const u8, dir: tree.Direction) bool {
    return switch (dir) {
        .out => std.mem.eql(u8, e.src, id),
        .in => std.mem.eql(u8, e.dst, id),
    };
}

fn drawDashboard(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, theme: palette.Theme) void {
    var y = r.y + 1;
    t.text(r.x + 2, y, "Catface v" ++ version.version ++ " — category cockpit", .{ .fg = theme.accent, .bg = theme.panel, .bold = true });
    y += 2;
    const lines = [_][]const u8{
        "Fast lanes: Alt-t TODO, Alt-n notes, Alt-e tests, Alt-s source, Alt-i info, Alt-f functions",
        "Natural search: wisp define reader layout",
        "Namespace search: @todo @hot @blocked @bugs @notes @functions @obs @dec @inf @reader @wisp @codegen",
        "Kind search: :Test :Record :Source :Concept :Todo",
        "Edge search: %verifies %supports %blocks %refines",
        "Category relation: lhs -> rhs, lhs <- rhs",
        "Examples: @tests -> reader, %verifies @tests -> codegen, TODO <- source",
        "Graph ops: > outgoing, < incoming, ~ neighborhood, proj conceptual projection",
    };
    for (lines) |line| {
        if (y >= r.bottom()) break;
        t.textClipped(r.x + 2, y, if (r.w > 4) r.w - 4 else 0, line, dashboardStyle(line, theme));
        y += 1;
    }
    if (y + 3 < r.bottom()) {
        y += 1;
        var buf: [256]u8 = undefined;
        const txt = std.fmt.bufPrint(&buf, "Loaded {d} objects and {d} arrows. The right pane keeps object text above a clickable color-coded relation forest.", .{ ctx.objects.items.len, ctx.edges.items.len }) catch "";
        t.textClipped(r.x + 2, y, if (r.w > 4) r.w - 4 else 0, txt, .{ .fg = theme.mute, .bg = theme.panel });
    }
}

fn dashboardStyle(line: []const u8, theme: palette.Theme) palette.Style {
    if (std.mem.indexOf(u8, line, "TODO") != null or std.mem.indexOf(u8, line, "@todo") != null) return .{ .fg = theme.todo, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "test") != null or std.mem.indexOf(u8, line, ":Test") != null) return .{ .fg = theme.test_color, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "source") != null or std.mem.indexOf(u8, line, "codegen") != null) return .{ .fg = theme.script, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "notes") != null or std.mem.indexOf(u8, line, "@obs") != null) return .{ .fg = theme.info, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "->") != null or std.mem.indexOf(u8, line, "%") != null) return .{ .fg = theme.accent2, .bg = theme.panel, .bold = true };
    return .{ .fg = theme.ink, .bg = theme.panel };
}

fn highlightStyle(obj: model.Object, theme: palette.Theme, bg: palette.Color, active: bool) palette.Style {
    if (obj.kind == .test_kind or hasToken(obj.title, "TEST") or hasToken(obj.preview, "TEST")) return .{ .fg = theme.test_color, .bg = bg, .bold = true };
    if (obj.kind == .todo or hasToken(obj.title, "TODO") or hasToken(obj.preview, "TODO")) return .{ .fg = theme.todo, .bg = bg, .bold = true };
    if (obj.kind == .done or hasToken(obj.title, "DONE") or hasToken(obj.preview, "DONE")) return .{ .fg = theme.done, .bg = bg, .bold = true };
    if (obj.kind == .record or hasToken(obj.title, "OBS") or hasToken(obj.title, "DEC") or hasToken(obj.title, "INF")) return .{ .fg = theme.record, .bg = bg, .bold = true };
    if (obj.kind == .function_kind or hasToken(obj.tags, "@functions")) return .{ .fg = theme.function_color, .bg = bg, .bold = true };
    if (obj.kind == .info or hasToken(obj.tags, "@info")) return .{ .fg = theme.info, .bg = bg, .bold = active };
    return .{ .fg = theme.ink, .bg = bg, .bold = active };
}

fn hasToken(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

const Dir = enum { in, out };

fn drawEdgeTree(t: *terminal.Tty, x: u16, y0: u16, w: u16, bottom: u16, ctx: *const model.Context, id: []const u8, dir: Dir, label: []const u8, theme: palette.Theme) u16 {
    var y = y0;
    if (y >= bottom) return y;
    t.text(x, y, label, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    y += 1;
    var shown: usize = 0;
    var total: usize = 0;
    for (ctx.edges.items) |e| {
        const keep = switch (dir) { .out => std.mem.eql(u8, e.src, id), .in => std.mem.eql(u8, e.dst, id) };
        if (!keep) continue;
        total += 1;
        if (y >= bottom or shown >= 10) continue;
        const other = switch (dir) { .out => e.dst, .in => e.src };
        var buf: [1024]u8 = undefined;
        var other_title: []const u8 = other;
        var other_kind: model.ObjectKind = .unknown;
        if (ctx.findObject(other)) |oi| {
            const o = ctx.objects.items[oi];
            other_title = if (o.title.len != 0) o.title else o.id;
            other_kind = o.kind;
        }
        const branch = "├─";
        const txt = std.fmt.bufPrint(&buf, "{s} {s} {s} {s}  {s}", .{ branch, glyphs.edge(e.kind), glyphs.kind(other_kind), model.Context.edgeName(e.kind), other_title }) catch other;
        t.textClipped(x + 2, y, if (w > 4) w - 4 else 0, txt, .{ .fg = edgeColor(e.kind, theme), .bg = theme.panel });
        shown += 1;
        y += 1;
    }
    if (total == 0 and y < bottom) {
        t.text(x + 2, y, "∅", .{ .fg = theme.edge, .bg = theme.panel });
        y += 1;
    } else if (total > shown and y < bottom) {
        var more_buf: [64]u8 = undefined;
        const more = std.fmt.bufPrint(&more_buf, "└─ +{d} more arrows", .{total - shown}) catch "└─ more";
        t.textClipped(x + 2, y, if (w > 4) w - 4 else 0, more, .{ .fg = theme.mute, .bg = theme.panel });
        y += 1;
    }
    return y;
}

fn edgeBadge(k: model.EdgeKind) []const u8 {
    return switch (k) {
        .contains => "HAS",
        .file_link => "FILE",
        .id_link => "LINK",
        .supports => "SUP",
        .supersedes => "NEW",
        .verifies => "VERIFY",
        .blocks => "BLOCK",
        .refines => "REFINE",
        .classifies_as => "CLASS",
        .forgets_to => "FORGET",
        .generated_by => "GEN",
        .mentions => "MENTION",
        .unknown => "EDGE",
    };
}

fn relationTargetBadge(ctx: *const model.Context, id: []const u8, edge_kind: model.EdgeKind) []const u8 {
    if (ctx.findObject(id)) |idx| {
        const obj = ctx.objects.items[idx];
        if (obj.kind == .record) return shortRecordClass(obj);
        return objectBadge(obj);
    }
    return edgeBadge(edge_kind);
}

fn edgeColor(k: model.EdgeKind, theme: palette.Theme) palette.Color {
    return switch (k) {
        .contains => theme.edge,
        .id_link => theme.accent,
        .supports, .verifies => theme.good,
        .blocks => theme.bad,
        .refines, .classifies_as, .forgets_to => theme.accent2,
        .generated_by => theme.warn,
        .mentions, .file_link => theme.info,
        .supersedes => theme.todo,
        .unknown => theme.mute,
    };
}

fn drawWrapped(t: *terminal.Tty, x: u16, y0: u16, w: u16, bottom: u16, text: []const u8, style: palette.Style) u16 {
    var y = y0;
    if (w == 0) return y;
    var start: usize = 0;
    while (start < text.len and y < bottom) {
        var end = @min(text.len, start + @as(usize, w));
        if (end < text.len) {
            if (std.mem.lastIndexOfScalar(u8, text[start..end], ' ')) |sp| {
                if (sp > 8) end = start + sp;
            }
        }
        if (end <= start) end = @min(text.len, start + @as(usize, w));
        t.textClipped(x, y, w, std.mem.trim(u8, text[start..end], " \t\r\n"), style);
        start = end;
        while (start < text.len and std.ascii.isWhitespace(text[start])) start += 1;
        y += 1;
    }
    return y;
}

fn countKind(ctx: *const model.Context, k: model.ObjectKind) usize {
    var n: usize = 0;
    for (ctx.objects.items) |obj| {
        if (obj.kind == k) {
            n += 1;
        }
    }
    return n;
}

fn countNamespace(ctx: *const model.Context, ns: []const u8) usize {
    var n: usize = 0;
    for (ctx.objects.items) |obj| {
        if (std.mem.eql(u8, ns, "notes")) {
            if (obj.kind == .record or obj.kind == .heading or obj.kind == .concept or obj.kind == .info) {
                n += 1;
            }
        }
    }
    return n;
}

fn countBugs(ctx: *const model.Context) usize {
    var n: usize = 0;
    for (ctx.objects.items) |obj| {
        if (hasToken(obj.title, "BUG") or hasToken(obj.title, "FAIL") or hasToken(obj.preview, "FAIL") or hasToken(obj.preview, "error:")) {
            n += 1;
        }
    }
    return n;
}

pub fn drawTutorial(t: *terminal.Tty, lay: Layout, theme: palette.Theme) void {
    const w: u16 = if (lay.header.w > 112) 106 else if (lay.header.w > 10) lay.header.w - 6 else lay.header.w;
    const h: u16 = if (lay.left.h > 25) 23 else if (lay.left.h > 8) lay.left.h - 2 else lay.left.h;
    const x: u16 = if (lay.header.w > w) @divTrunc(lay.header.w - w, 2) else 0;
    const y: u16 = if (lay.left.h > h) lay.left.y + @divTrunc(lay.left.h - h, 2) else lay.left.y;
    const r = terminal.Rect{ .x = x, .y = y, .w = w, .h = h };
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    if (r.h == 0 or r.w == 0) return;

    var yy = r.y + 1;
    var xx = r.x + 2;
    t.textClipped(xx, yy, if (r.w > 4) r.w - 4 else 0, "󰄛 Catface help", .{ .fg = theme.accent, .bg = theme.panel_alt, .bold = true });
    yy += 2;
    xx = r.x + 2;
    xx = drawHelpChip(t, xx, yy, "SEARCH", theme.accent2, theme);
    xx = drawHelpChip(t, xx + 1, yy, "TODO", theme.todo, theme);
    xx = drawHelpChip(t, xx + 1, yy, "TEST", theme.test_color, theme);
    xx = drawHelpChip(t, xx + 1, yy, "GRAPH", theme.accent, theme);
    _ = drawHelpChip(t, xx + 1, yy, "ESC", theme.warn, theme);
    yy += 2;

    const lines = [_][]const u8{
        "Typing is always search. Printable keys insert into the query line.",
        "Move results: ↑/↓ or C-n/C-p. Right tree: n/p/j/k, RET/l opens, h backs up.",
        "Fast lanes: Alt-t @todo, Alt-n @notes, Alt-e @tests, Alt-s @source. Try @hot, @blocked, title:reader.",
        "More lanes: Alt-u @bugs, Alt-f @functions, Alt-w @wisp, Alt-m @reader, Alt-c @codegen, Alt-r :Record.",
        "Objects: :Test :Function :Record :Source :Concept :Todo :Info.  IDs: ?id or #id.",
        "Edges: %verifies %supports %blocks %refines %mentions %generated-by.",
        "Category syntax: lhs -> rhs and lhs <- rhs ask for arrows between object sets.",
        "Examples: @tests -> reader  ·  reader <- @tests  ·  %verifies @tests -> codegen.",
        "Graph ops: > outgoing, < incoming, ~ neighborhood, proj conceptual projection.",
        "Symbol algebra: T test, S source, λ function, ▣ record, I note, ! TODO; arrows ✓ verify, ⊢ support, ⊣ block, ≤ refine, ⇢ link.",
        "Right pane: top is selected object text; bottom is a collapsible color-coded relation tree.",
        "Click ▸/▾ OUT/IN or edge headings to collapse. Click object rows to select targets.",
        "Wheel scrolls results/tree. Footer shows frame/query/flush nanosecond timings.",
        "Alt-b goes back.  C-l/C-u clear.  Esc closes help/quits.",
    };
    for (lines) |tutorial_text| {
        if (yy >= r.bottom() - 1) break;
        t.textClipped(r.x + 2, yy, if (r.w > 4) r.w - 4 else 0, tutorial_text, tutorialLineStyle(tutorial_text, theme, theme.panel_alt));
        yy += 1;
    }
}

fn drawHelpChip(t: *terminal.Tty, x: u16, y: u16, label: []const u8, color: palette.Color, theme: palette.Theme) u16 {
    var buf: [40]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, " {s} ", .{label}) catch label;
    const width: u16 = @intCast(@min(text.len + 1, 32));
    t.textClipped(x, y, width, text, .{ .fg = theme.bg, .bg = color, .bold = true });
    return x + width;
}

fn tutorialLineStyle(tutorial_text: []const u8, theme: palette.Theme, bg: palette.Color) palette.Style {
    if (std.mem.indexOf(u8, tutorial_text, "->") != null or std.mem.indexOf(u8, tutorial_text, "%") != null) return .{ .fg = theme.accent2, .bg = bg, .bold = true };
    if (std.mem.indexOf(u8, tutorial_text, "Alt-") != null or std.mem.indexOf(u8, tutorial_text, "C-") != null) return .{ .fg = theme.accent2, .bg = bg, .bold = true };
    if (std.mem.indexOf(u8, tutorial_text, "TODO") != null or std.mem.indexOf(u8, tutorial_text, "TEST") != null) return .{ .fg = theme.todo, .bg = bg, .bold = true };
    return .{ .fg = theme.ink, .bg = bg };
}

pub fn writeObjectCard(buf: *std.array_list.Managed(u8), ctx: *const model.Context, idx: usize, width: usize) !void {
    const obj = ctx.objects.items[idx];
    try boxLine(buf, width, '╭', '─', '╮');
    try centered(buf, width, obj.id);
    try buf.appendSlice("\n");
    try field(buf, width, "kind", model.Context.kindName(obj.kind));
    try field(buf, width, "path", obj.path);
    try field(buf, width, "meaning", objectOneLineMeaning(obj));
    if (obj.title.len != 0) try field(buf, width, "title", obj.title);
    if (obj.preview.len != 0) try field(buf, width, "preview", obj.preview);
    try field(buf, width, "out", "Hom(object, -)");
    var out_count: usize = 0;
    for (ctx.edges.items) |e| {
        if (std.mem.eql(u8, e.src, obj.id) and out_count < 8) {
            try field(buf, width, model.Context.edgeName(e.kind), e.dst);
            out_count += 1;
        }
    }
    try field(buf, width, "in", "Hom(-, object)");
    var in_count: usize = 0;
    for (ctx.edges.items) |e| {
        if (std.mem.eql(u8, e.dst, obj.id) and in_count < 8) {
            try field(buf, width, model.Context.edgeName(e.kind), e.src);
            in_count += 1;
        }
    }
    try boxLine(buf, width, '╰', '─', '╯');
}

fn boxLine(buf: *std.array_list.Managed(u8), width: usize, l: u21, fill: u21, r: u21) !void {
    try appendCp(buf, l);
    var i: usize = 0;
    while (i + 2 < width) : (i += 1) try appendCp(buf, fill);
    try appendCp(buf, r);
    try buf.append('\n');
}

fn centered(buf: *std.array_list.Managed(u8), width: usize, s: []const u8) !void {
    try appendCp(buf, '│');
    const inner = if (width > 2) width - 2 else 0;
    const cut = @min(inner, s.len);
    try buf.appendSlice(s[0..cut]);
    var i = cut;
    while (i < inner) : (i += 1) try buf.append(' ');
    try appendCp(buf, '│');
}

fn field(buf: *std.array_list.Managed(u8), width: usize, name: []const u8, value: []const u8) !void {
    try appendCp(buf, '│');
    try buf.appendSlice(" ");
    const inner = if (width > 4) width - 4 else 0;
    var used: usize = 0;
    const ncut = @min(name.len, inner);
    try buf.appendSlice(name[0..ncut]);
    used += ncut;
    if (used < inner) {
        try buf.appendSlice(": ");
        used += 2;
    }
    const remaining = inner - @min(used, inner);
    const vcut = @min(value.len, remaining);
    try buf.appendSlice(value[0..vcut]);
    used += vcut;
    while (used < inner) : (used += 1) try buf.append(' ');
    try buf.appendSlice(" ");
    try appendCp(buf, '│');
    try buf.append('\n');
}

fn appendCp(buf: *std.array_list.Managed(u8), cp: u21) !void {
    var tmp: [4]u8 = undefined;
    const n = try std.unicode.utf8Encode(cp, &tmp);
    try buf.appendSlice(tmp[0..n]);
}

fn chooseFirstNonEmpty(a: []const u8, b: []const u8) []const u8 {
    return if (a.len != 0) a else if (b.len != 0) b else "∅";
}

fn expectationHint(obj: model.Object) []const u8 {
    if (hasToken(obj.preview, "compile-fail") or hasToken(obj.preview, "error:")) return "negative compile/runtime expectation";
    if (hasToken(obj.preview, "stdout") or hasToken(obj.preview, "show")) return "observable output / golden behavior";
    if (hasToken(obj.preview, "json") or hasToken(obj.preview, "AST")) return "reader/desugar/AST contract";
    return "see TEST-EXPECT / preview / connected record";
}

fn recordClass(obj: model.Object) []const u8 {
    if (hasToken(obj.title, "OBS") or hasToken(obj.preview, "[OBS") or hasToken(obj.preview, "OBS")) return "OBS observation";
    if (hasToken(obj.title, "DEC") or hasToken(obj.preview, "[DEC") or hasToken(obj.preview, "DEC")) return "DEC decision";
    if (hasToken(obj.title, "INF") or hasToken(obj.preview, "[INF") or hasToken(obj.preview, "INF")) return "INF inference";
    if (hasToken(obj.title, "FIX") or hasToken(obj.preview, "FIX")) return "FIX repair note";
    return "record";
}

fn recordTrust(class: []const u8) []const u8 {
    if (hasToken(class, "OBS")) return "high when grounded in source/test output";
    if (hasToken(class, "DEC")) return "authoritative project intent until superseded";
    if (hasToken(class, "INF")) return "useful but verify against source/tests before editing";
    if (hasToken(class, "FIX")) return "repair note; check connected regression tests";
    return "read together with incoming/outgoing arrows";
}

fn objectOneLineMeaning(obj: model.Object) []const u8 {
    return switch (obj.kind) {
        .test_kind => "compiler behavior test / regression contract",
        .function_kind => "first-class function with searchable type signature",
        .todo => "open work item",
        .done => "completed work / evidence marker",
        .record => "context record: observation, decision, inference, or fix",
        .source => "compiler source object",
        .script => "tooling or script object",
        .concept => "semantic concept in the category",
        .info => "info/doc object",
        .heading => "org heading / context section",
        .file => "file-level object",
        .report => "generated report object",
        .unknown => "unclassified object",
    };
}

fn lineAsText(line: usize) []const u8 {
    _ = line;
    return "see path:line above";
}

test "detail relation tree hit test selects connected object" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "a", .kind = .test_kind, .title = "TEST A", .path = "tests/a.mon", .preview = "TEST A verifies B" });
    _ = try ctx.addObject(.{ .id = "b", .kind = .record, .title = "[OBS] B", .path = "context/b.org", .preview = "[OBS] linked fact" });
    _ = try ctx.addEdge(.{ .id = "ab", .kind = .verifies, .src = "a", .dst = "b" });
    const idx = ctx.findObject("a").?;
    const rect = terminal.Rect{ .x = 0, .y = 0, .w = 80, .h = 24 };
    const graph = detailGraphRect(rect.inset(1), &ctx, null, "a");
    var tree_state = tree.State.init();
    const hit_y = graph.y + 3;
    const action = detailHitTest(&ctx, rect, idx, &tree_state, hit_y);
    switch (action) {
        .select => |target| try std.testing.expect(std.mem.eql(u8, ctx.objects.items[target].id, "b")),
        else => return error.ExpectedRelationTreeSelection,
    }
    const toggle = detailHitTest(&ctx, rect, idx, &tree_state, graph.y + 1);
    switch (toggle) {
        .toggle_dir => |dir| try std.testing.expect(dir == .out),
        else => return error.ExpectedRelationTreeToggle,
    }
}

test "detail tree row count and cursor action follow visible rows" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "a", .kind = .test_kind, .title = "TEST A", .path = "tests/a.mon", .preview = "TEST A verifies B" });
    _ = try ctx.addObject(.{ .id = "b", .kind = .record, .title = "[OBS] B", .path = "context/b.org", .preview = "[OBS] linked fact" });
    _ = try ctx.addEdge(.{ .id = "ab", .kind = .verifies, .src = "a", .dst = "b" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const focus = ctx.findObject("a").?;
    var state = tree.State.init();
    const rows = detailTreeRowCountIndexed(&ctx, &idx, focus, &state);
    try std.testing.expect(rows >= 3);
    state.sync(rows, 4);
    state.cursor = 2;
    const action = detailTreeActionAtCursorIndexed(&ctx, &idx, focus, &state);
    switch (action) {
        .select => |target| try std.testing.expect(std.mem.eql(u8, ctx.objects.items[target].id, "b")),
        else => return error.ExpectedCursorSelection,
    }
}


test "record display text hides machine directive header" {
    const obj = model.Object{ .id = "r", .kind = .record, .title = "OBS", .preview = "[OBS id:x supports:y] Reader layout is stable." };
    try std.testing.expect(std.mem.eql(u8, objectDisplayText(obj), "Reader layout is stable."));
}
