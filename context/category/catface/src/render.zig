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
const command_palette = @import("command_palette.zig");

pub const ResultRowHeight: usize = 3;

pub const Layout = struct {
    header: terminal.Rect,
    left: terminal.Rect,
    right: terminal.Rect,
    footer: terminal.Rect,
};

pub const Point = struct { x: u16, y: u16 };

pub const DetailAction = union(enum) {
    none,
    select: usize,
    open_file: []const u8,
    open_file_at: struct { path: []const u8, line: usize },
    query: []const u8,
    id: []const u8,
    tag: []const u8,
    heading: []const u8,
    viewer,
    github,
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
    t.textClipped(2, r.y + 2, if (r.w > 4) r.w - 4 else 0,
        "TAB completion · C-i/Alt-i @info · C-o pane · Alt-t @todo · Alt-n @notes · Alt-e @tests · Alt-s @source · Alt-f @functions · Alt-k @contracts · Alt-y @quality · Alt-a @metadata · Alt-l @links",
        .{ .fg = theme.accent2, .bg = theme.bg, .bold = true });

    t.fill(.{ .x = 0, .y = r.y + 3, .w = r.w, .h = 1 }, '─', .{ .fg = theme.edge, .bg = theme.bg });
    var counts_buf: [320]u8 = undefined;
    const counts = std.fmt.bufPrint(&counts_buf, "TODO {d}  TEST {d}  NOTE {d}  INFO {d}  SOURCE {d}  RECORD {d}  BUG/FAIL {d}", .{
        countKind(ctx, .todo),
        countKind(ctx, .test_kind),
        countNamespace(ctx, "notes"),
        countKind(ctx, .info),
        countKind(ctx, .source) + countKind(ctx, .script),
        countKind(ctx, .record),
        countBugs(ctx),
    }) catch "";
    t.textClipped(2, r.y + 4, if (r.w > 4) r.w - 4 else 0, counts, .{ .fg = theme.mute, .bg = theme.bg });
}

pub fn drawFooter(t: *terminal.Tty, r: terminal.Rect, msg: []const u8, stats: perf.Stats, theme: palette.Theme) void {
    t.fill(r, ' ', .{ .fg = theme.mute, .bg = theme.bg });
    if (r.h == 0) return;
    var perf_buf: [320]u8 = undefined;
    const perf_line = std.fmt.bufPrint(&perf_buf, "frame {d}ns  query {d}ns  pal {d}ns/{d}v/{d}cap  flush {d}ns  cached {d}", .{
        stats.last_frame_ns,
        stats.last_query_ns,
        stats.last_palette_ns,
        stats.last_palette_visible,
        stats.last_palette_capacity,
        stats.last_flush_ns,
        stats.cached_refreshes,
    }) catch "frame/query/palette timing unavailable";
    const left = "Type to search · TAB completion · RET opens right content mode · Int -> Int finds functions · C-h c describe key · ? help";
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
            const preview = chooseFirstNonEmpty(objectDisplayText(obj), objectOneLineMeaning(obj));
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
        .info => "INFO",
        .heading => "HEAD",
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
        if (std.mem.eql(u8, cls, "BUG")) return theme.bad;
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
    if (hasToken(obj.title, "BUG") or hasToken(obj.preview, "[BUG") or hasToken(obj.preview, "FAIL") or hasToken(obj.preview, "error:")) return "BUG";
    return "REC";
}

fn drawEmptyResults(t: *terminal.Tty, r: terminal.Rect, theme: palette.Theme) void {
    t.text(r.x + 2, r.y + 1, "No matches.", .{ .fg = theme.bad, .bg = theme.panel, .bold = true });
    t.textClipped(r.x + 2, r.y + 3, if (r.w > 4) r.w - 4 else 0, "Try @todo, @hot, @blocked, title:reader, @roots, @reader, :Record, %verifies, or a -> b.", .{ .fg = theme.mute, .bg = theme.panel });
}

pub fn drawDetail(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx_opt: ?usize, tree_state: *const tree.State, active: bool, theme: palette.Theme) void {
    drawDetailWithIndex(t, r, ctx, null, idx_opt, tree_state, &.{}, active, theme, null, false, 0, 0, 0);
}

pub fn drawDetailIndexed(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx: *const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State, focus_stack: []const usize, active: bool, theme: palette.Theme) void {
    drawDetailWithIndex(t, r, ctx, idx, idx_opt, tree_state, focus_stack, active, theme, null, false, 0, 0, 0);
}

pub fn drawDetailInteractive(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx: *const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State, focus_stack: []const usize, active: bool, theme: palette.Theme, hover: ?Point, viewer_open: bool, viewer_scroll: usize, org_scroll: usize, org_cursor: usize) void {
    drawDetailWithIndex(t, r, ctx, idx, idx_opt, tree_state, focus_stack, active, theme, hover, viewer_open, viewer_scroll, org_scroll, org_cursor);
}

fn drawDetailWithIndex(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, idx_opt: ?usize, tree_state: *const tree.State, focus_stack: []const usize, active: bool, theme: palette.Theme, hover: ?Point, viewer_open: bool, viewer_scroll: usize, org_scroll: usize, org_cursor: usize) void {
    t.box(r, detailTitle(ctx, idx_opt, active), active, theme);
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0) return;
    if (idx_opt == null) {
        drawDashboard(t, inner, ctx, theme);
        return;
    }

    const idx = idx_opt.?;
    const obj = ctx.objects.items[idx];
    if (viewer_open) {
        drawDocumentViewer(t, inner, obj, hover, viewer_scroll, theme);
        return;
    }
    const graph = detailGraphRect(inner, ctx, sidx_opt, obj.id);
    const text_bottom: u16 = if (graph.h > 0 and graph.y > inner.y) graph.y - 1 else inner.bottom();
    const text_h: u16 = if (text_bottom > inner.y) text_bottom - inner.y else 0;
    const text_rect = terminal.Rect{ .x = inner.x, .y = inner.y, .w = inner.w, .h = text_h };

    if (text_rect.h > 0) {
        var y = text_rect.y;
        y = drawIdentity(t, text_rect, y, ctx, focus_stack, idx, obj, theme, hover);
        if (y < text_rect.bottom()) {
            y += 1;
            y = drawPrimaryOrgContent(t, text_rect, y, obj, hover, org_scroll, org_cursor, theme);
        }
        if (y + 4 < text_rect.bottom()) {
            y += 1;
            _ = drawObjectText(t, text_rect, y, ctx, sidx_opt, obj, theme, hover);
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

pub fn detailTreeRowAtIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, r: terminal.Rect, idx_opt: ?usize, tree_state: *const tree.State, mouse_y: u16) ?usize {
    if (idx_opt == null) return null;
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0) return null;
    const obj = ctx.objects.items[idx_opt.?];
    const graph = detailGraphRect(inner, ctx, idx, obj.id);
    if (graph.h <= 1 or mouse_y <= graph.y or mouse_y >= graph.bottom()) return null;
    const content_y = graph.y + 1;
    return tree_state.scroll + @as(usize, mouse_y - content_y);
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
            .info => " info page + context links ",
            .heading => " heading text + context links ",
            .file, .report => " artifact text + contained objects ",
            .unknown => " object text + relation tree ",
        };
    }
    return " Catface cockpit ";
}

fn drawIdentity(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, focus_stack: []const usize, current_idx: usize, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    if (y >= r.bottom()) return y;
    const bx = drawKindBubble(t, r.x, y, obj, theme.panel, theme);
    if (bx + 2 < r.right()) t.textClipped(bx + 2, y, r.right() - bx - 2, model.Context.kindName(obj.kind), .{ .fg = theme.mute, .bg = theme.panel, .bold = true });
    if (r.w > 58) t.textClipped(r.right() - 56, y, 55, "Org: wheel/TAB/n/p · tree: h/j/k/l", .{ .fg = theme.mute.scale(78, 100), .bg = theme.panel });
    y += 1;

    y = drawFocusChain(t, r, y, ctx, focus_stack, current_idx, theme);
    const title = if (obj.title.len != 0) obj.title else obj.id;
    t.textClipped(r.x, y, r.w, title, .{ .fg = theme.ink, .bg = theme.panel, .bold = true });
    y += 1;
    y = drawMetadataChipLine(t, r, y, obj, theme, hover);
    y = drawOrgMetadataChips(t, r, y, obj, theme, hover);
    return y;
}

fn drawMetadataChipLine(t: *terminal.Tty, r: terminal.Rect, y0: u16, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    const y = y0;
    if (y >= r.bottom()) return y;
    t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.mute, .bg = theme.panel });
    var x = r.x;
    x = drawLinkChip(t, x, y, r.right(), "id", obj.id, theme.info, theme, hover);
    if (obj.path.len != 0) x = drawLinkChip(t, x + 1, y, r.right(), "file", obj.path, theme.accent2, theme, hover);
    if (obj.tags.len != 0) x = drawLinkChip(t, x + 1, y, r.right(), "tags", obj.tags, theme.record, theme, hover);
    if (x + 8 < r.right()) {
        var line_buf: [64]u8 = undefined;
        const line = std.fmt.bufPrint(&line_buf, "line:{d}", .{obj.line}) catch "line";
        _ = drawLinkChip(t, x + 1, y, r.right(), "at", line, theme.mute, theme, hover);
    }
    return y + 1;
}

fn drawOrgMetadataChips(t: *terminal.Tty, r: terminal.Rect, y0: u16, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    var row: usize = 0;
    var x = r.x;
    const doc = objectDocumentText(obj);
    var in_properties = false;
    var seen_any = false;
    var it = std.mem.splitScalar(u8, doc, '\n');
    const max_y = @min(r.bottom(), y0 + 2);
    while (it.next()) |raw| {
        if (y >= max_y) break;
        const line = std.mem.trim(u8, raw, " \t\r\n");
        if (line.len == 0) {
            if (!seen_any) continue;
            break;
        }
        if (isDrawerStart(line)) {
            in_properties = true;
            seen_any = true;
            continue;
        }
        if (in_properties) {
            if (isDrawerEnd(line)) { in_properties = false; continue; }
            if (y >= r.bottom()) break;
            if (std.mem.startsWith(u8, line, ":")) {
                if (std.mem.indexOfPos(u8, line, 1, ":")) |colon| {
                    const key = std.mem.trim(u8, line[1..colon], " \t");
                    const value = std.mem.trim(u8, line[colon + 1 ..], " \t");
                    if (value.len != 0) {
                        if (row == 0) t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.mute, .bg = theme.panel });
                        const next_x = drawLinkChip(t, x, y, r.right(), key, value, theme.info, theme, hover);
                        if (next_x == x or next_x + 2 >= r.right()) { y += 1; row = 0; x = r.x; }
                        else { x = next_x + 1; row += 1; }
                    }
                }
            }
            continue;
        }
        if (isOrgMetadataLine(line)) {
            seen_any = true;
            if (y >= r.bottom()) break;
            const colon = std.mem.indexOfScalar(u8, line, ':') orelse line.len;
            if (colon > 2 and colon + 1 < line.len) {
                const key = std.mem.trim(u8, line[2..colon], " \t");
                const value = std.mem.trim(u8, line[colon + 1 ..], " \t");
                if (value.len != 0) {
                    if (row == 0) t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.mute, .bg = theme.panel });
                    const color = if (startsFold(key, "FILETAGS") or startsFold(key, "TAGS")) theme.record else if (startsFold(key, "TITLE")) theme.heading else theme.info;
                    const next_x = drawLinkChip(t, x, y, r.right(), key, value, color, theme, hover);
                    if (next_x == x or next_x + 2 >= r.right()) { y += 1; row = 0; x = r.x; }
                    else { x = next_x + 1; row += 1; }
                }
            }
            continue;
        }
        break;
    }
    if (row != 0) y += 1;
    return y;
}

fn drawLinkChip(t: *terminal.Tty, x0: u16, y: u16, right: u16, name: []const u8, value: []const u8, color: palette.Color, theme: palette.Theme, hover: ?Point) u16 {
    if (x0 >= right or value.len == 0) return x0;
    var buf: [512]u8 = undefined;
    const txt = std.fmt.bufPrint(&buf, "[{s}: {s}]", .{ name, value }) catch value;
    const w: u16 = @intCast(@min(txt.len, @as(usize, right - x0)));
    const hovered = if (hover) |pnt| pnt.y == y and pnt.x >= x0 and pnt.x < x0 + w else false;
    const bg = if (hovered) theme.panel_alt.scale(126, 100) else theme.panel_alt.scale(108, 100);
    t.textClipped(x0, y, w, txt, .{ .fg = color, .bg = bg, .bold = true, .underline = hovered });
    return x0 + w;
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

fn drawObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    return switch (obj.kind) {
        .test_kind => drawTestObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .info => drawInfoObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .function_kind => drawFunctionObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .source, .script => drawSourceObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .record => drawRecordObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .todo => drawTodoObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .done => drawDoneObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .concept => drawConceptObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        .heading => drawHeadingObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
        else => drawGenericObjectText(t, r, y0, ctx, sidx_opt, obj, theme, hover),
    };
}

fn drawPrimaryOrgContent(t: *terminal.Tty, r: terminal.Rect, y0: u16, obj: model.Object, hover: ?Point, scroll: usize, cursor_line: usize, theme: palette.Theme) u16 {
    var y = y0;
    const body = objectDocumentBody(obj);
    if (body.len == 0 or y >= r.bottom()) return y;
    y = drawSection(t, r, y, "ORG CONTENT", objectBadgeColor(obj, theme), theme);
    if (y >= r.bottom()) return y;
    return drawRichDocumentLines(t, r, y, body, hover, scroll, cursor_line, theme);
}

fn drawGenericObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, objectTextTitle(obj), objectBadgeColor(obj, theme), theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "does", objectDoes(obj), theme);
    y = drawKV(t, r, y, "why", objectWhy(obj), theme);
    y = drawKV(t, r, y, "text", objectDisplayText(obj), theme);
    if (obj.title.len != 0 and !std.mem.eql(u8, obj.title, obj.preview)) y = drawKV(t, r, y, "title", obj.title, theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawTestObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "TEST CONTRACT", theme.test_color, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "id", testHeaderField(obj.preview, "TEST-ID:") orelse testIdFallback(obj), theme);
    y = drawKV(t, r, y, "context", testHeaderField(obj.preview, "TEST-CONTEXT:") orelse contextHintFromPath(obj.path), theme);
    y = drawKV(t, r, y, "purpose", testHeaderField(obj.preview, "TEST-PURPOSE:") orelse objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "atom", testHeaderField(obj.preview, "TEST-ATOM:") orelse "", theme);
    y = drawKV(t, r, y, "expect", testHeaderField(obj.preview, "TEST-EXPECT:") orelse expectationHint(obj), theme);
    y = drawKV(t, r, y, "surface", testSurfaceHint(obj), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "run", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawInfoObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "INFO PAGE", theme.info, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "read", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "topic", infoTopicHint(obj), theme);
    y = drawKV(t, r, y, "tags", obj.tags, theme);
    y = drawKV(t, r, y, "trust", objectWhy(obj), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawFunctionObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "FUNCTION TYPE", theme.function_color, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "name", if (obj.title.len != 0) obj.title else obj.id, theme);
    y = drawKV(t, r, y, "type", functionSignatureOnly(obj), theme);
    y = drawKV(t, r, y, "search", functionTypeQueryHint(obj), theme);
    y = drawActionKV(t, r, y, "where", obj.path, theme, hover);
    y = drawKV(t, r, y, "tags", obj.tags, theme);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawSourceObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, if (obj.kind == .script) "SCRIPT SURFACE" else "SOURCE SURFACE", objectBadgeColor(obj, theme), theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "role", sourceRoleHint(obj), theme);
    y = drawKV(t, r, y, "summary", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "verify", "Use incoming %verifies tests and supporting OBS records before editing.", theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawRecordObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "RECORD CLAIM", objectBadgeColor(obj, theme), theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawRecordFacts(t, r, y, obj, theme);
    y = drawKV(t, r, y, "claim", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "trust", recordTrust(recordClass(obj)), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "source", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawTodoObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "TODO WORK ITEM", theme.todo, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "next", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "why", objectWhy(obj), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawDoneObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "DONE EVIDENCE", theme.done, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "done", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "use", objectWhy(obj), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawConceptObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "CONCEPT OBJECT", theme.concept, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "meaning", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "project", "Use proj or classifies-as/refines edges to move between implementation and abstraction.", theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawHeadingObjectText(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    y = drawSection(t, r, y, "CONTEXT HEADING", theme.heading, theme);
    y = drawSignalRow(t, r, y, ctx, sidx_opt, obj, theme);
    y = drawKV(t, r, y, "section", if (obj.title.len != 0) obj.title else obj.id, theme);
    y = drawKV(t, r, y, "text", objectDisplayText(obj), theme);
    y = drawKV(t, r, y, "kind", objectWhy(obj), theme);
    if (obj.path.len != 0) y = drawActionKV(t, r, y, "open", obj.path, theme, hover);
    y = drawPreviewBlock(t, r, y, obj, theme, hover);
    return y;
}

fn drawSignalRow(t: *terminal.Tty, r: terminal.Rect, y0: u16, ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, obj: model.Object, theme: palette.Theme) u16 {
    const y = y0;
    if (y >= r.bottom()) return y;
    const out_n = if (sidx_opt) |idx| idx.outgoing(obj.id).len else countEdges(ctx, null, obj.id, .out, null);
    const in_n = if (sidx_opt) |idx| idx.incoming(obj.id).len else countEdges(ctx, null, obj.id, .in, null);
    var buf: [256]u8 = undefined;
    const signal = std.fmt.bufPrint(&buf, "kind {s}  status {s}  OUT {d}  IN {d}  weight {d}", .{ model.Context.kindName(obj.kind), objectSignalLabel(obj), out_n, in_n, obj.weight }) catch "object signals";
    t.textClipped(r.x, y, r.w, signal, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    return y + 1;
}

fn objectSignalLabel(obj: model.Object) []const u8 {
    return switch (obj.kind) {
        .test_kind => testSurfaceHint(obj),
        .info => infoTopicHint(obj),
        .function_kind => functionSignatureOnly(obj),
        .todo => if (hasToken(obj.preview, "block") or hasToken(obj.title, "FAIL")) "needs triage" else "open work",
        .done => "completed evidence",
        .record => recordClass(obj),
        .source, .script => sourceRoleHint(obj),
        .concept => "category concept",
        .heading => "context section",
        .file => "file object",
        .report => "generated report",
        .unknown => "unclassified",
    };
}

fn drawRecordFacts(t: *terminal.Tty, r: terminal.Rect, y0: u16, obj: model.Object, theme: palette.Theme) u16 {
    var y = y0;
    y = drawKV(t, r, y, "record", shortRecordClass(obj), theme);
    y = drawKV(t, r, y, "rid", recordHeaderField(obj.preview, "id") orelse "", theme);
    y = drawKV(t, r, y, "src", recordHeaderField2(obj.preview, "src", "source") orelse "", theme);
    y = drawKV(t, r, y, "conf", recordHeaderField2(obj.preview, "conf", "confidence") orelse "", theme);
    y = drawKV(t, r, y, "from", recordHeaderField(obj.preview, "from") orelse "", theme);
    y = drawKV(t, r, y, "supports", recordHeaderField(obj.preview, "supports") orelse "", theme);
    y = drawKV(t, r, y, "verifies", recordHeaderField(obj.preview, "verifies") orelse "", theme);
    y = drawKV(t, r, y, "blocks", recordHeaderField(obj.preview, "blocks") orelse "", theme);
    y = drawKV(t, r, y, "supersedes", recordHeaderField(obj.preview, "supersedes") orelse "", theme);
    return y;
}


fn testIdFallback(obj: model.Object) []const u8 {
    if (std.mem.startsWith(u8, obj.id, "test:")) return obj.id[5..];
    return obj.id;
}

fn testHeaderField(preview: []const u8, marker: []const u8) ?[]const u8 {
    const pos = std.mem.indexOf(u8, preview, marker) orelse return null;
    const rest = std.mem.trim(u8, preview[pos + marker.len ..], " \t;\r\n");
    if (rest.len == 0) return null;
    var end = rest.len;
    const markers = [_][]const u8{ "TEST-ID:", "TEST-CONTEXT:", "TEST-PURPOSE:", "TEST-ATOM:", "TEST-EXPECT:" };
    for (markers) |m| {
        if (std.mem.eql(u8, m, marker)) continue;
        if (std.mem.indexOf(u8, rest, m)) |hit| {
            if (hit != 0 and hit < end) end = hit;
        }
    }
    const field_text = std.mem.trim(u8, rest[0..end], " \t;\r\n");
    return if (field_text.len == 0) null else field_text;
}

fn contextHintFromPath(path: []const u8) []const u8 {
    if (hasToken(path, "reader")) return "reader/parser surface";
    if (hasToken(path, "wisp")) return "wisp expander surface";
    if (hasToken(path, "codegen")) return "codegen/runtime surface";
    if (hasToken(path, "infer") or hasToken(path, "types")) return "type inference/type system surface";
    if (hasToken(path, "core")) return "core library surface";
    return "test corpus";
}

fn testSurfaceHint(obj: model.Object) []const u8 {
    if (hasToken(obj.preview, "compile-fail") or hasToken(obj.preview, "error:")) return "negative compile/error regression";
    if (hasToken(obj.preview, "stdout") or hasToken(obj.path, ".stdout")) return "stdout golden behavior";
    if (hasToken(obj.preview, "desugar") or hasToken(obj.path, ".desugar")) return "desugar golden behavior";
    if (hasToken(obj.preview, "json") or hasToken(obj.preview, "AST")) return "reader/AST golden behavior";
    return contextHintFromPath(obj.path);
}

fn infoTopicHint(obj: model.Object) []const u8 {
    if (hasToken(obj.tags, "wisp") or hasToken(obj.path, "wisp")) return "Wisp reference";
    if (hasToken(obj.tags, "type") or hasToken(obj.path, "type")) return "type-system reference";
    if (hasToken(obj.tags, "runtime") or hasToken(obj.path, "runtime")) return "runtime reference";
    if (hasToken(obj.tags, "performance") or hasToken(obj.path, "performance")) return "performance reference";
    if (hasToken(obj.tags, "language") or hasToken(obj.path, "syntax")) return "language reference";
    return "project info/reference";
}

fn functionSignatureOnly(obj: model.Object) []const u8 {
    const full = functionSignatureText(obj);
    if (std.mem.indexOf(u8, full, "::")) |pos| return std.mem.trim(u8, full[pos + 2 ..], " \t\r\n");
    return full;
}

fn functionTypeQueryHint(obj: model.Object) []const u8 {
    const sig = functionSignatureOnly(obj);
    if (sig.len != 0) return sig;
    return "search plain arrows, e.g. Int -> Int or a -> a";
}

fn sourceRoleHint(obj: model.Object) []const u8 {
    if (hasToken(obj.path, "reader")) return "reader/parser implementation";
    if (hasToken(obj.path, "wisp")) return "Wisp layout/sugar implementation";
    if (hasToken(obj.path, "codegen")) return "code generator/runtime bridge";
    if (hasToken(obj.path, "infer")) return "type inference implementation";
    if (hasToken(obj.path, "core")) return "core/prelude library";
    if (obj.kind == .script) return "project tooling/script";
    return "source artifact";
}

fn recordHeaderField2(preview_raw: []const u8, a: []const u8, b: []const u8) ?[]const u8 {
    if (recordHeaderField(preview_raw, a)) |value| return value;
    return recordHeaderField(preview_raw, b);
}

fn recordHeaderField(preview_raw: []const u8, key: []const u8) ?[]const u8 {
    const preview = std.mem.trim(u8, preview_raw, " \t\r\n");
    if (preview.len == 0 or preview[0] != '[') return null;
    const end = std.mem.indexOfScalar(u8, preview, ']') orelse return null;
    const header = preview[1..end];
    var parts = std.mem.tokenizeAny(u8, header, " \t");
    while (parts.next()) |part| {
        const colon = std.mem.indexOfScalar(u8, part, ':') orelse continue;
        const name = part[0..colon];
        if (!std.ascii.eqlIgnoreCase(name, key)) continue;
        return std.mem.trim(u8, part[colon + 1 ..], " ,.;\t");
    }
    return null;
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
fn drawActionKV(t: *terminal.Tty, r: terminal.Rect, y0: u16, key: []const u8, value: []const u8, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    if (y >= r.bottom() or value.len == 0) return y;
    var label_buf: [32]u8 = undefined;
    const label = std.fmt.bufPrint(&label_buf, "{s}", .{key}) catch key;
    const label_w: u16 = if (r.w > 14) 9 else @min(r.w, @as(u16, 6));
    t.textClipped(r.x, y, label_w, label, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    if (r.w <= label_w + 1) return y + 1;
    const text_x = r.x + label_w + 1;
    const text_w = r.right() - text_x;
    var hovered = false;
    if (hover) |p| hovered = p.y == y and p.x >= text_x and p.x < r.right();
    const style = palette.Style{ .fg = theme.info, .bg = theme.panel, .bold = true, .underline = hovered };
    var buf: [768]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, "↪ {s}", .{value}) catch value;
    y = drawWrapped(t, text_x, y, text_w, r.bottom(), text, style);
    return y;
}

fn drawPreviewBlock(t: *terminal.Tty, r: terminal.Rect, y0: u16, obj: model.Object, theme: palette.Theme, hover: ?Point) u16 {
    var y = y0;
    const preview = std.mem.trim(u8, obj.preview, " \t\r\n");
    if (preview.len == 0 or y + 2 >= r.bottom()) return y;
    const display = objectDisplayText(obj);
    if (display.len == 0 or std.mem.eql(u8, display, obj.title)) return y;
    y += 1;
    y = drawSection(t, r, y, "RICH ORG PREVIEW", objectBadgeColor(obj, theme), theme);
    y = drawRichDocumentLines(t, r, y, display, hover, 0, 0, theme);
    return y;
}

fn drawDocumentViewer(t: *terminal.Tty, r: terminal.Rect, obj: model.Object, hover: ?Point, scroll: usize, theme: palette.Theme) void {
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel });
    if (r.h == 0 or r.w == 0) return;
    const title = if (obj.title.len != 0) obj.title else obj.id;
    t.textClipped(r.x, r.y, r.w, "┌ document viewer — q close, wheel/Page scroll, release opens links", .{ .fg = objectBadgeColor(obj, theme), .bg = theme.panel, .bold = true });
    if (r.h < 3) return;
    t.textClipped(r.x, r.y + 1, r.w, title, .{ .fg = theme.ink, .bg = theme.panel, .bold = true });
    if (obj.path.len != 0 and r.h > 3) {
        var hovered = false;
        if (hover) |p| hovered = p.y == r.y + 2;
        t.textClipped(r.x, r.y + 2, r.w, obj.path, .{ .fg = theme.info, .bg = theme.panel, .underline = hovered, .bold = true });
    }
    const doc_y: u16 = if (r.h > 4) r.y + 4 else r.bottom();
    if (doc_y >= r.bottom()) return;
    const body = objectDocumentBody(obj);
    _ = drawRichDocumentLines(t, .{ .x = r.x, .y = doc_y, .w = r.w, .h = r.bottom() - doc_y }, doc_y, body, hover, scroll, scroll, theme);
}

const OrgLineMode = enum { normal, source, example, quote, drawer };

fn drawRichDocumentLines(t: *terminal.Tty, r: terminal.Rect, y0: u16, text: []const u8, hover: ?Point, scroll: usize, cursor_line: usize, theme: palette.Theme) u16 {
    var y = y0;
    var visible_line_no: usize = 0;
    var mode: OrgLineMode = .normal;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |raw_line| {
        const line = std.mem.trimRight(u8, raw_line, " \t\r");
        const trimmed = std.mem.trim(u8, line, " \t");
        if (skipOrgRenderLine(trimmed, &mode)) continue;
        if (visible_line_no >= scroll and y < r.bottom()) {
            drawOrgRenderedLine(t, r, y, line, hover, visible_line_no == cursor_line, theme, &mode);
            y += 1;
        }
        visible_line_no += 1;
        if (y >= r.bottom()) break;
    }
    return y;
}

fn skipOrgRenderLine(trimmed: []const u8, mode: *OrgLineMode) bool {
    if (mode.* == .drawer) {
        if (isDrawerEnd(trimmed)) mode.* = .normal;
        return true;
    }
    if (isDrawerStart(trimmed)) { mode.* = .drawer; return true; }
    if (isOrgMetadataLine(trimmed)) return true;
    if (isBeginSrc(trimmed)) { mode.* = .source; return true; }
    if (isBeginExample(trimmed)) { mode.* = .example; return true; }
    if (isBeginQuote(trimmed)) { mode.* = .quote; return true; }
    if (isEndBlock(trimmed)) { mode.* = .normal; return true; }
    return false;
}
fn updateOrgLineMode(line: []const u8, mode: *OrgLineMode) void {
    const trimmed = std.mem.trim(u8, line, " \t");
    if (isBeginSrc(trimmed)) { mode.* = .source; return; }
    if (isBeginExample(trimmed)) { mode.* = .example; return; }
    if (isBeginQuote(trimmed)) { mode.* = .quote; return; }
    if (isDrawerStart(trimmed)) { mode.* = .drawer; return; }
    if (isEndBlock(trimmed) or isDrawerEnd(trimmed)) mode.* = .normal;
}

fn drawOrgRenderedLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, hover: ?Point, cursor: bool, theme: palette.Theme, mode: *OrgLineMode) void {
    if (y >= r.bottom()) return;
    if (cursor) t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    const trimmed_left = std.mem.trimLeft(u8, line, " \t");
    const trimmed = std.mem.trim(u8, line, " \t");

    if (isBeginSrc(trimmed)) { mode.* = .source; return; }
    if (isBeginExample(trimmed)) { mode.* = .example; return; }
    if (isBeginQuote(trimmed)) { mode.* = .quote; return; }
    if (isEndBlock(trimmed)) { mode.* = .normal; return; }
    if (isDrawerStart(trimmed)) { mode.* = .drawer; return; }
    if (isDrawerEnd(trimmed)) { mode.* = .normal; return; }

    switch (mode.*) {
        .source => { drawOrgCodeLine(t, r, y, line, theme); return; },
        .example => { drawOrgExampleLine(t, r, y, line, theme); return; },
        .quote => { drawOrgQuoteLine(t, r, y, line, hover, theme); return; },
        .drawer => { drawOrgDrawerLine(t, r, y, trimmed, theme); return; },
        .normal => {},
    }

    if (trimmed.len == 0) {
        t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.mute, .bg = theme.panel });
    } else if (isTodoKeywordLine(trimmed)) {
        drawTodoPriorityText(t, r, y, trimmed, theme, false);
    } else if (isOrgTableLine(trimmed)) {
        drawOrgTableLine(t, r, y, trimmed, theme);
    } else if (trimmed_left.len != 0 and trimmed_left[0] == '*') {
        drawOrgHeadingLine(t, r, y, trimmed_left, hover, theme);
    } else if (std.mem.startsWith(u8, trimmed, "#+")) {
        t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.mute, .bg = theme.panel });
    } else if (isPlanningLine(trimmed)) {
        t.textClipped(r.x, y, r.w, trimmed, .{ .fg = theme.warn, .bg = if (cursor) theme.panel_alt else theme.panel, .bold = true });
    } else if (isListLine(trimmed_left)) {
        drawOrgListLine(t, r, y, trimmed_left, hover, theme);
    } else if (isRecordLine(trimmed)) {
        drawOrgRecordLine(t, r, y, trimmed, hover, theme);
    } else {
        drawOrgInlineLine(t, r, y, line, hover, theme);
    }
}

fn isOrgTableLine(line: []const u8) bool {
    const trimmed = std.mem.trim(u8, line, " \t");
    return trimmed.len >= 2 and trimmed[0] == '|';
}

fn drawOrgTableLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, theme: palette.Theme) void {
    if (y >= r.bottom()) return;
    const trimmed = std.mem.trim(u8, line, " \t");
    if (trimmed.len >= 2 and trimmed[1] == '-') {
        t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, '─', .{ .fg = theme.edge, .bg = theme.panel });
        return;
    }
    t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    var x = r.x;
    var cells = std.mem.splitScalar(u8, trimmed, '|');
    var cell_no: usize = 0;
    while (cells.next()) |cell_raw| {
        if (cell_no == 0 and cell_raw.len == 0) { cell_no += 1; continue; }
        if (x >= r.right()) break;
        t.text(x, y, "│", .{ .fg = theme.edge, .bg = theme.panel_alt });
        x += 1;
        const cell = std.mem.trim(u8, cell_raw, " \t");
        const remain = if (r.right() > x) r.right() - x else 0;
        if (remain == 0) break;
        const width: u16 = @intCast(@min(@max(cell.len + 2, 8), @as(usize, remain)));
        const is_headerish = cell_no == 1 or std.mem.indexOf(u8, cell, "TEST") != null or std.mem.indexOf(u8, cell, "key") != null;
        t.textClipped(x + 1, y, if (width > 1) width - 1 else width, cell, .{ .fg = if (is_headerish) theme.accent2 else theme.ink, .bg = theme.panel_alt, .bold = is_headerish });
        x += width;
        cell_no += 1;
    }
    if (x < r.right()) t.text(x, y, "│", .{ .fg = theme.edge, .bg = theme.panel_alt });
}

fn drawOrgHeadingLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, hover: ?Point, theme: palette.Theme) void {
    var level: usize = 0;
    while (level < line.len and line[level] == '*') : (level += 1) {}
    const rest_raw = std.mem.trimLeft(u8, line[level..], " \t");
    const tag_start = trailingOrgTagsStart(rest_raw);
    const rest = if (tag_start) |ts| std.mem.trimRight(u8, rest_raw[0..ts], " \t") else rest_raw;
    const tags = if (tag_start) |ts| rest_raw[ts..] else "";
    const color = priorityColor(rest, theme) orelse if (level <= 1) theme.accent2 else if (level == 2) theme.heading else theme.info;
    t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = color, .bg = theme.panel });
    const bullet = switch (@min(level, @as(usize, 6))) {
        0, 1 => "●",
        2 => "◆",
        3 => "◇",
        4 => "○",
        else => "·",
    };
    t.textClipped(r.x, y, @min(@as(u16, 3), r.w), bullet, .{ .fg = color, .bg = theme.panel, .bold = true });
    const text_x = r.x + @min(@as(u16, 3), r.w);
    if (text_x >= r.right()) return;
    const remain_for_tags: u16 = r.right() - text_x;
    const tag_w: u16 = @intCast(@min(tags.len + 2, @as(usize, remain_for_tags)));
    const text_right = if (tags.len != 0 and r.right() > tag_w + 1) r.right() - tag_w - 1 else r.right();
    const text_w = if (text_right > text_x) text_right - text_x else 0;
    if (std.mem.startsWith(u8, rest, "TODO") or std.mem.startsWith(u8, rest, "DONE") or std.mem.startsWith(u8, rest, "WIP")) {
        drawTodoPriorityText(t, .{ .x = text_x, .y = y, .w = text_w, .h = 1 }, y, rest, theme, true);
    } else {
        t.textClipped(text_x, y, text_w, rest, .{ .fg = color, .bg = theme.panel, .bold = true });
    }
    if (tags.len != 0 and text_right + 1 < r.right()) _ = drawOrgTagButtons(t, text_right + 1, y, r.right(), tags, theme, hover);
}

fn drawTodoPriorityText(t: *terminal.Tty, r: terminal.Rect, y: u16, text_value: []const u8, theme: palette.Theme, heading: bool) void {
    var text = text_value;
    var x = r.x;
    const bg = theme.panel;
    if (std.mem.startsWith(u8, text, "TODO")) {
        t.textClipped(x, y, @min(@as(u16, 5), r.w), "TODO", .{ .fg = theme.bg, .bg = theme.todo, .bold = true });
        x += @min(@as(u16, 5), r.w);
        text = std.mem.trimLeft(u8, text[4..], " \t");
    } else if (std.mem.startsWith(u8, text, "DONE")) {
        t.textClipped(x, y, @min(@as(u16, 5), r.w), "DONE", .{ .fg = theme.bg, .bg = theme.done, .bold = true });
        x += @min(@as(u16, 5), r.w);
        text = std.mem.trimLeft(u8, text[4..], " \t");
    } else if (std.mem.startsWith(u8, text, "WIP")) {
        t.textClipped(x, y, @min(@as(u16, 4), r.w), "WIP", .{ .fg = theme.bg, .bg = theme.warn, .bold = true });
        x += @min(@as(u16, 4), r.w);
        text = std.mem.trimLeft(u8, text[3..], " \t");
    }
    if (x >= r.right()) return;
    if (priorityChip(text)) |chip| {
        const color = priorityColor(chip, theme) orelse theme.warn;
        const w: u16 = @intCast(@min(chip.len, @as(usize, r.right() - x)));
        t.textClipped(x, y, w, chip, .{ .fg = theme.bg, .bg = color, .bold = true });
        const gap: u16 = if (x + w < r.right()) 1 else 0;
        x += w + gap;
        text = std.mem.trimLeft(u8, removeFirstPriority(text), " \t");
    }
    if (x < r.right()) t.textClipped(x, y, r.right() - x, text, .{ .fg = if (heading) theme.ink else theme.ink, .bg = bg, .bold = heading });
}

fn priorityChip(text_value: []const u8) ?[]const u8 {
    if (std.mem.indexOf(u8, text_value, "[#A]")) |i| return text_value[i .. i + 4];
    if (std.mem.indexOf(u8, text_value, "[#B]")) |i| return text_value[i .. i + 4];
    if (std.mem.indexOf(u8, text_value, "[#C]")) |i| return text_value[i .. i + 4];
    if (std.mem.indexOf(u8, text_value, "[#D]")) |i| return text_value[i .. i + 4];
    return null;
}

fn removeFirstPriority(text_value: []const u8) []const u8 {
    if (std.mem.indexOf(u8, text_value, "[#")) |i| {
        if (i + 4 <= text_value.len and text_value[i + 3] == ']') {
            if (i + 4 >= text_value.len) return "";
            return text_value[i + 4 ..];
        }
    }
    return text_value;
}

fn drawOrgKeywordLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, theme: palette.Theme) void {
    const colon = std.mem.indexOfScalar(u8, line, ':') orelse line.len;
    const key = line[0..@min(colon + 1, line.len)];
    const value = if (colon < line.len) std.mem.trimLeft(u8, line[colon + 1 ..], " \t") else "";
    t.textClipped(r.x, y, @min(@as(u16, 20), r.w), key, .{ .fg = theme.mute, .bg = theme.panel, .bold = true });
    const x = r.x + @min(@as(u16, 20), r.w);
    if (x < r.right()) t.textClipped(x, y, r.right() - x, value, .{ .fg = theme.info, .bg = theme.panel });
}

fn drawOrgListLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, hover: ?Point, theme: palette.Theme) void {
    var rest = line;
    var marker: []const u8 = "•";
    if (std.mem.startsWith(u8, rest, "- [X]") or std.mem.startsWith(u8, rest, "- [x]")) { marker = "☑"; rest = std.mem.trimLeft(u8, rest[5..], " \t"); }
    else if (std.mem.startsWith(u8, rest, "- [ ]")) { marker = "☐"; rest = std.mem.trimLeft(u8, rest[5..], " \t"); }
    else if (std.mem.startsWith(u8, rest, "- ") or std.mem.startsWith(u8, rest, "+ ")) { rest = std.mem.trimLeft(u8, rest[2..], " \t"); }
    t.text(r.x, y, marker, .{ .fg = theme.accent, .bg = theme.panel, .bold = true });
    if (r.w > 2) {
        if (isTodoKeywordLine(rest) or priorityColor(rest, theme) != null) drawTodoPriorityText(t, .{ .x = r.x + 2, .y = y, .w = r.w - 2, .h = 1 }, y, rest, theme, false)
        else drawOrgInlineLine(t, .{ .x = r.x + 2, .y = y, .w = r.w - 2, .h = 1 }, y, rest, hover, theme);
    }
}

fn drawOrgRecordLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, hover: ?Point, theme: palette.Theme) void {
    const rb = std.mem.indexOfScalar(u8, line, ']') orelse 0;
    if (rb == 0) { drawOrgInlineLine(t, r, y, line, hover, theme); return; }
    const head = line[0..rb + 1];
    const body = std.mem.trimLeft(u8, line[rb + 1 ..], " \t-:");
    const label = recordClassLabel(head);
    const color = recordClassColor(label, theme);
    t.textClipped(r.x, y, @min(@as(u16, 7), r.w), label, .{ .fg = theme.bg, .bg = color, .bold = true });
    const x = r.x + @min(@as(u16, 8), r.w);
    if (x < r.right()) drawOrgInlineLine(t, .{ .x = x, .y = y, .w = r.right() - x, .h = 1 }, y, body, hover, theme);
}

fn recordClassLabel(head: []const u8) []const u8 {
    if (std.mem.indexOf(u8, head, "OBS") != null) return " OBS ";
    if (std.mem.indexOf(u8, head, "DEC") != null) return " DEC ";
    if (std.mem.indexOf(u8, head, "BUG") != null or std.mem.indexOf(u8, head, "FAIL") != null) return " BUG ";
    if (std.mem.indexOf(u8, head, "FIX") != null) return " FIX ";
    if (std.mem.indexOf(u8, head, "INF") != null) return " INF ";
    return " REC ";
}

fn recordClassColor(label: []const u8, theme: palette.Theme) palette.Color {
    if (std.mem.indexOf(u8, label, "OBS") != null) return theme.obs;
    if (std.mem.indexOf(u8, label, "DEC") != null) return theme.warn;
    if (std.mem.indexOf(u8, label, "BUG") != null) return theme.bad;
    if (std.mem.indexOf(u8, label, "FIX") != null) return theme.done;
    if (std.mem.indexOf(u8, label, "INF") != null) return theme.info;
    return theme.record;
}

fn drawOrgBlockDelimiter(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, label: []const u8, theme: palette.Theme) void {
    var buf: [512]u8 = undefined;
    const text_value = std.fmt.bufPrint(&buf, "▸ {s} {s}", .{ label, line }) catch line;
    t.textClipped(r.x, y, r.w, text_value, .{ .fg = theme.script, .bg = theme.panel, .bold = true });
}

fn drawOrgCodeLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, theme: palette.Theme) void {
    t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = theme.bg });
    t.text(r.x, y, "│", .{ .fg = theme.script, .bg = theme.bg });
    if (r.w > 2) t.textClipped(r.x + 2, y, r.w - 2, line, .{ .fg = theme.ink, .bg = theme.bg });
}

fn drawOrgExampleLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, theme: palette.Theme) void {
    const bg = theme.bg.scale(82, 100);
    t.fill(.{ .x = r.x, .y = y, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = bg });
    if (r.w > 2) t.textClipped(r.x + 2, y, r.w - 2, line, .{ .fg = theme.accent2, .bg = bg });
}

fn drawOrgQuoteLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, hover: ?Point, theme: palette.Theme) void {
    t.text(r.x, y, "▌", .{ .fg = theme.edge, .bg = theme.panel });
    if (r.w > 2) drawOrgInlineLine(t, .{ .x = r.x + 2, .y = y, .w = r.w - 2, .h = 1 }, y, line, hover, theme);
}

fn drawOrgDrawerLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line: []const u8, theme: palette.Theme) void {
    t.textClipped(r.x, y, r.w, line, .{ .fg = theme.mute, .bg = theme.panel, .dim = true });
}

fn drawOrgInlineLine(t: *terminal.Tty, r: terminal.Rect, y: u16, line_raw: []const u8, hover: ?Point, theme: palette.Theme) void {
    const tag_start = trailingOrgTagsStart(line_raw);
    const line = if (tag_start) |ts| std.mem.trimRight(u8, line_raw[0..ts], " \t") else line_raw;
    const tags = if (tag_start) |ts| line_raw[ts..] else "";
    if (priorityColor(line, theme)) |pc| {
        t.textClipped(r.x, y, r.w, line, .{ .fg = pc, .bg = theme.panel, .bold = true });
        if (tags.len != 0 and r.w > line.len + 2) _ = drawOrgTagButtons(t, r.x + @as(u16, @intCast(@min(line.len + 2, @as(usize, r.w)))), y, r.right(), tags, theme, hover);
        return;
    }
    var x = r.x;
    var pos: usize = 0;
    while (pos < line.len and x < r.right()) {
        const next_link = std.mem.indexOfPos(u8, line, pos, "[[");
        const next_file = std.mem.indexOfPos(u8, line, pos, "file:");
        const next_id = std.mem.indexOfPos(u8, line, pos, "id:");
        const next = minOptional(next_link, minOptional(next_file, next_id));
        if (next) |open| {
            if (open > pos) {
                const before = line[pos..open];
                t.textClipped(x, y, r.right() - x, before, .{ .fg = theme.ink, .bg = theme.panel });
                x += @intCast(@min(before.len, @as(usize, r.right() - x)));
            }
            if (x >= r.right()) break;
            if (std.mem.startsWith(u8, line[open..], "[[")) {
                if (std.mem.indexOfPos(u8, line, open + 2, "]]")) |close| {
                    const raw = line[open..close + 2];
                    const label = orgLinkLabel(raw);
                    const w: u16 = @intCast(@min(label.len, @as(usize, r.right() - x)));
                    const hovered = if (hover) |pnt| pnt.y == y and pnt.x >= x and pnt.x < x + w else false;
                    const bg = if (hovered) theme.panel_alt.scale(120, 100) else theme.panel;
                    t.textClipped(x, y, w, label, .{ .fg = theme.info, .bg = bg, .underline = hovered, .bold = true });
                    x += w;
                    pos = close + 2;
                    continue;
                }
            }
            const prefix = if (std.mem.startsWith(u8, line[open..], "file:")) "file:" else "id:";
            const target = wordAfterPrefix(line[open..], prefix);
            const w: u16 = @intCast(@min(target.len, @as(usize, r.right() - x)));
            const hovered = if (hover) |pnt| pnt.y == y and pnt.x >= x and pnt.x < x + w else false;
            const bg = if (hovered) theme.panel_alt.scale(120, 100) else theme.panel;
            t.textClipped(x, y, w, target, .{ .fg = theme.info, .bg = bg, .underline = hovered, .bold = true });
            x += w;
            pos = open + target.len;
            continue;
        }
        t.textClipped(x, y, r.right() - x, line[pos..], .{ .fg = theme.ink, .bg = theme.panel });
        x += @intCast(@min(line.len - pos, @as(usize, r.right() - x)));
        break;
    }
    if (tags.len != 0 and x + 1 < r.right()) _ = drawOrgTagButtons(t, x + 1, y, r.right(), tags, theme, hover);
}

fn drawOrgTagButtons(t: *terminal.Tty, x0: u16, y: u16, right: u16, tags: []const u8, theme: palette.Theme, hover: ?Point) u16 {
    var x = x0;
    var pos: usize = 0;
    while (pos < tags.len and x < right) {
        while (pos < tags.len and (tags[pos] == ':' or std.ascii.isWhitespace(tags[pos]))) : (pos += 1) {}
        const start = pos;
        while (pos < tags.len and tags[pos] != ':' and !std.ascii.isWhitespace(tags[pos])) : (pos += 1) {}
        if (pos <= start) break;
        const tag = tags[start..pos];
        var buf: [96]u8 = undefined;
        const txt = std.fmt.bufPrint(&buf, "#{s}", .{tag}) catch tag;
        const w: u16 = @intCast(@min(txt.len + 2, @as(usize, right - x)));
        const hovered = if (hover) |pnt| pnt.y == y and pnt.x >= x and pnt.x < x + w else false;
        const bg = if (hovered) theme.panel_alt.scale(128, 100) else theme.panel_alt.scale(110, 100);
        t.textClipped(x, y, w, txt, .{ .fg = theme.record, .bg = bg, .bold = true, .underline = hovered });
        const gap: u16 = if (x + w < right) 1 else 0;
        x += w + gap;
    }
    return x;
}

fn trailingOrgTagsStart(line: []const u8) ?usize {
    const trimmed = std.mem.trimRight(u8, line, " \t\r");
    if (trimmed.len < 3 or trimmed[trimmed.len - 1] != ':') return null;
    var i = trimmed.len - 1;
    var saw_colon = false;
    while (i > 0) {
        i -= 1;
        const c = trimmed[i];
        if (c == ':') { saw_colon = true; continue; }
        if (std.ascii.isAlphanumeric(c) or c == '_' or c == '-' or c == '@' or c == '.') continue;
        if (std.ascii.isWhitespace(c) and saw_colon and i + 1 < trimmed.len and trimmed[i + 1] == ':') return i + 1;
        return null;
    }
    return if (saw_colon and trimmed[0] == ':') 0 else null;
}

fn priorityColor(line: []const u8, theme: palette.Theme) ?palette.Color {
    if (std.mem.indexOf(u8, line, "[#A]") != null) return theme.bad;
    if (std.mem.indexOf(u8, line, "[#B]") != null) return theme.warn;
    if (std.mem.indexOf(u8, line, "[#C]") != null) return theme.accent2;
    if (std.mem.indexOf(u8, line, "[#D]") != null) return theme.info;
    return null;
}

fn minOptional(a: ?usize, b: ?usize) ?usize {
    if (a == null) return b;
    if (b == null) return a;
    return @min(a.?, b.?);
}

fn isBeginSrc(line: []const u8) bool { return startsFold(line, "#+begin_src") or startsFold(line, "#+BEGIN_SRC"); }
fn isBeginExample(line: []const u8) bool { return startsFold(line, "#+begin_example") or startsFold(line, "#+BEGIN_EXAMPLE"); }
fn isBeginQuote(line: []const u8) bool { return startsFold(line, "#+begin_quote") or startsFold(line, "#+BEGIN_QUOTE"); }
fn isEndBlock(line: []const u8) bool { return startsFold(line, "#+end_") or startsFold(line, "#+END_"); }
fn isDrawerStart(line: []const u8) bool { return std.mem.eql(u8, line, ":PROPERTIES:") or std.mem.eql(u8, line, ":LOGBOOK:"); }
fn isDrawerEnd(line: []const u8) bool { return std.mem.eql(u8, line, ":END:"); }
fn isPlanningLine(line: []const u8) bool { return startsFold(line, "SCHEDULED:") or startsFold(line, "DEADLINE:") or startsFold(line, "CLOSED:"); }
fn isTodoKeywordLine(line: []const u8) bool { return std.mem.startsWith(u8, line, "TODO ") or std.mem.startsWith(u8, line, "DONE ") or std.mem.startsWith(u8, line, "WIP ") or std.mem.startsWith(u8, line, "WAIT ") or std.mem.startsWith(u8, line, "CANCELLED "); }
fn isRecordLine(line: []const u8) bool { return std.mem.startsWith(u8, line, "[") and std.mem.indexOf(u8, line, " id:") != null; }

fn isListLine(line: []const u8) bool {
    if (std.mem.startsWith(u8, line, "- ") or std.mem.startsWith(u8, line, "+ ") or std.mem.startsWith(u8, line, "- [")) return true;
    if (line.len >= 3 and std.ascii.isDigit(line[0]) and line[1] == '.' and std.ascii.isWhitespace(line[2])) return true;
    return false;
}

fn startsFold(text_value: []const u8, prefix: []const u8) bool {
    if (text_value.len < prefix.len) return false;
    for (text_value[0..prefix.len], prefix) |a, b| {
        if (std.ascii.toLower(a) != std.ascii.toLower(b)) return false;
    }
    return true;
}

fn findFirstOrgTarget(line: []const u8) ?[]const u8 {
    if (std.mem.indexOf(u8, line, "[[")) |open| {
        if (std.mem.indexOfPos(u8, line, open + 2, "]]")) |close| {
            const inside = line[open + 2 .. close];
            if (std.mem.indexOf(u8, inside, "][")) |mid| return inside[0..mid];
            return inside;
        }
    }
    if (std.mem.indexOf(u8, line, "file:")) |pos| return wordAfterPrefix(line[pos..], "file:");
    if (std.mem.indexOf(u8, line, "id:")) |pos| return wordAfterPrefix(line[pos..], "id:");
    if (std.mem.indexOf(u8, line, "<<")) |open| {
        if (std.mem.indexOfPos(u8, line, open + 2, ">>")) |close| return line[open + 2 .. close];
    }
    return null;
}

fn wordAfterPrefix(text_value: []const u8, prefix: []const u8) []const u8 {
    var end = prefix.len;
    while (end < text_value.len and !std.ascii.isWhitespace(text_value[end]) and text_value[end] != ')' and text_value[end] != ']' and text_value[end] != ';') : (end += 1) {}
    return text_value[0..end];
}

fn orgLinkLabel(raw: []const u8) []const u8 {
    if (raw.len < 4) return raw;
    const inside = raw[2 .. raw.len - 2];
    if (std.mem.indexOf(u8, inside, "][")) |mid| return inside[mid + 2 ..];
    return inside;
}

fn detailBodyStartY(inner: terminal.Rect, obj: model.Object) u16 {
    const rows: u16 = @intCast(@min(@as(usize, 2), orgMetadataVisualRows(obj)));
    return inner.y + 5 + rows;
}

fn orgMetadataVisualRows(obj: model.Object) usize {
    var rows: usize = 0;
    var chips: usize = 0;
    const doc = objectDocumentText(obj);
    var in_properties = false;
    var seen_any = false;
    var it = std.mem.splitScalar(u8, doc, '\n');
    while (it.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r\n");
        if (line.len == 0) {
            if (!seen_any) continue;
            break;
        }
        if (isDrawerStart(line)) { in_properties = true; seen_any = true; continue; }
        if (in_properties) {
            if (isDrawerEnd(line)) { in_properties = false; continue; }
            if (std.mem.startsWith(u8, line, ":")) chips += 1;
            continue;
        }
        if (isOrgMetadataLine(line)) { seen_any = true; chips += 1; continue; }
        break;
    }
    if (chips != 0) rows = 1 + @divTrunc(chips - 1, 3);
    return @min(rows, 2);
}

fn isNavigableOrgLine(line_raw: []const u8) bool {
    const line = std.mem.trim(u8, line_raw, " \t\r\n");
    return findFirstOrgTarget(line) != null or firstOrgTag(line) != null or orgHeadingText(line) != null or priorityChip(line) != null or isRecordLine(line);
}

fn orgHeadingText(line: []const u8) ?[]const u8 {
    if (line.len == 0 or line[0] != '*') return null;
    var level: usize = 0;
    while (level < line.len and line[level] == '*') : (level += 1) {}
    const rest = std.mem.trim(u8, line[level..], " \t");
    if (rest.len == 0) return null;
    const tag_start = trailingOrgTagsStart(rest);
    const title = if (tag_start) |ts| std.mem.trimRight(u8, rest[0..ts], " \t") else rest;
    return if (title.len == 0) null else title;
}

fn firstTagValue(tags_raw: []const u8) ?[]const u8 {
    const tags = std.mem.trim(u8, tags_raw, " \t:,");
    if (tags.len == 0) return null;
    var end: usize = 0;
    while (end < tags.len and tags[end] != ':' and tags[end] != ',' and !std.ascii.isWhitespace(tags[end])) : (end += 1) {}
    return if (end == 0) null else tags[0..end];
}

fn firstOrgTag(line: []const u8) ?[]const u8 {
    const tag_start = trailingOrgTagsStart(line) orelse return null;
    var tags = line[tag_start..];
    while (tags.len != 0 and (tags[0] == ':' or std.ascii.isWhitespace(tags[0]))) tags = tags[1..];
    var end: usize = 0;
    while (end < tags.len and tags[end] != ':' and !std.ascii.isWhitespace(tags[end])) : (end += 1) {}
    if (end == 0) return null;
    return tags[0..end];
}

pub fn documentBodyLineCount(obj: model.Object) usize {
    const body = objectDocumentBody(obj);
    if (body.len == 0) return 0;
    var n: usize = 0;
    var mode: OrgLineMode = .normal;
    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |raw| {
        const trimmed = std.mem.trim(u8, raw, " \t\r\n");
        if (skipOrgRenderLine(trimmed, &mode)) continue;
        n += 1;
    }
    return n;
}

pub fn nextDocumentTargetLine(obj: model.Object, current: usize, dir: isize) ?usize {
    const body = objectDocumentBody(obj);
    var best_after: ?usize = null;
    var best_before: ?usize = null;
    var first: ?usize = null;
    var last: ?usize = null;
    var line_no: usize = 0;
    var mode: OrgLineMode = .normal;
    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r\n");
        if (skipOrgRenderLine(trimmed, &mode)) continue;
        if (isNavigableOrgLine(line)) {
            if (first == null) first = line_no;
            last = line_no;
            if (line_no > current and best_after == null) best_after = line_no;
            if (line_no < current) best_before = line_no;
        }
        line_no += 1;
    }
    if (dir >= 0) return best_after orelse first;
    return best_before orelse last;
}

pub fn detailTextActionAtIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, r: terminal.Rect, idx_opt: ?usize, x: u16, y: u16, viewer_open: bool, viewer_scroll: usize, org_scroll: usize) DetailAction {
    return detailTextActionAtWithIndex(ctx, idx, r, idx_opt, x, y, viewer_open, viewer_scroll, org_scroll);
}

fn detailTextActionAtWithIndex(ctx: *const model.Context, sidx_opt: ?*const search_index.SearchIndex, r: terminal.Rect, idx_opt: ?usize, x: u16, y: u16, viewer_open: bool, viewer_scroll: usize, org_scroll: usize) DetailAction {
    if (idx_opt == null) return .none;
    const inner = r.inset(1);
    if (inner.h == 0 or inner.w == 0 or y < inner.y or y >= inner.bottom() or x < inner.x or x >= inner.right()) return .none;
    const obj = ctx.objects.items[idx_opt.?];
    if (viewer_open) {
        if (obj.path.len != 0 and y == inner.y + 2) return .{ .open_file = obj.path };
        const doc_y: u16 = if (inner.h > 4) inner.y + 4 else inner.bottom();
        if (y >= doc_y) {
            const logical_line: usize = viewer_scroll + @as(usize, y - doc_y);
            const action = actionAtDocumentLine(ctx, obj, logical_line);
            if (!isDetailActionNone(action)) return action;
        }
        return .none;
    }
    if (y <= inner.y + 4) {
        if (obj.path.len != 0 and obj.line != 0 and x + 28 >= inner.right()) return .{ .open_file_at = .{ .path = obj.path, .line = obj.line } };
        if (obj.tags.len != 0 and x > inner.x + 30) return .{ .tag = firstTagValue(obj.tags) orelse obj.tags };
        if (obj.path.len != 0 and x > inner.x + 12) return .{ .open_file = obj.path };
        return .{ .id = obj.id };
    }
    const doc_start: u16 = detailBodyStartY(inner, obj);
    if (y >= doc_start) {
        const action = actionAtDocumentLine(ctx, obj, org_scroll + @as(usize, y - doc_start));
            if (!isDetailActionNone(action)) return action;
    }
    if (obj.path.len != 0) {
        const graph = detailGraphRect(inner, ctx, sidx_opt, obj.id);
        if (graph.h == 0 or y < graph.y) return .{ .open_file = obj.path };
    }
    return .none;
}

fn isDetailActionNone(action: DetailAction) bool {
    return switch (action) { .none => true, else => false };
}

fn actionAtDocumentLine(ctx: *const model.Context, obj: model.Object, line_index: usize) DetailAction {
    const body = objectDocumentBody(obj);
    var logical: usize = 0;
    var mode: OrgLineMode = .normal;
    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |line_raw| {
        const trimmed = std.mem.trim(u8, line_raw, " \t\r\n");
        if (skipOrgRenderLine(trimmed, &mode)) continue;
        if (logical == line_index) return actionFromOrgLine(ctx, obj, line_raw);
        logical += 1;
    }
    return .none;
}

fn actionFromOrgLine(ctx: *const model.Context, obj: model.Object, line_raw: []const u8) DetailAction {
    const line = std.mem.trim(u8, line_raw, " \t\r\n");
    if (findFirstOrgTarget(line)) |target| return actionFromOrgTarget(ctx, obj, target);
    if (firstOrgTag(line)) |tag| return .{ .tag = tag };
    if (orgHeadingText(line)) |heading| return .{ .heading = heading };
    if (priorityChip(line)) |chip| return .{ .query = chip };
    if (isRecordLine(line)) return .{ .query = recordClassLabel(line) };
    return .none;
}

fn actionFromOrgTarget(ctx: *const model.Context, obj: model.Object, target_raw: []const u8) DetailAction {
    _ = obj;
    const target = std.mem.trim(u8, target_raw, " \t\r\n");
    if (target.len == 0) return .none;
    if (std.mem.startsWith(u8, target, "file:")) return .{ .open_file = target[5..] };
    if (std.mem.startsWith(u8, target, "id:")) {
        const id = target[3..];
        if (ctx.findObject(id)) |idx| return .{ .select = idx };
        return .{ .id = id };
    }
    if (ctx.findObject(target)) |idx| return .{ .select = idx };
    if (looksLikePath(target)) return .{ .open_file = target };
    return .{ .query = target };
}

fn looksLikePath(target: []const u8) bool {
    return std.mem.indexOfScalar(u8, target, '/') != null or
        std.mem.endsWith(u8, target, ".org") or
        std.mem.endsWith(u8, target, ".zig") or
        std.mem.endsWith(u8, target, ".mon") or
        std.mem.endsWith(u8, target, ".md") or
        std.mem.endsWith(u8, target, ".catq");
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
        .info => "INFO: READABLE REFERENCE PAGE",
        .heading => "HEADING: CONTEXT SECTION",
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
        .info => chooseFirstNonEmpty(body, "documentation/reference page"),
        .heading => chooseFirstNonEmpty(body, "context heading/section"),
        .file, .report => chooseFirstNonEmpty(body, "file/report level artifact"),
        .unknown => chooseFirstNonEmpty(body, obj.title),
    };
}

fn objectDocumentText(obj: model.Object) []const u8 {
    const preview = std.mem.trim(u8, obj.preview, " \t\r\n");
    if (preview.len != 0) return preview;
    if (obj.kind == .function_kind) return functionSignatureText(obj);
    if (obj.title.len != 0) return obj.title;
    return obj.id;
}

fn objectDocumentBody(obj: model.Object) []const u8 {
    const doc = objectDocumentText(obj);
    return stripLeadingOrgMetadata(doc);
}

fn stripLeadingOrgMetadata(doc: []const u8) []const u8 {
    var pos: usize = 0;
    var in_properties = false;
    var saw_metadata = false;
    while (pos < doc.len) {
        const line_start = pos;
        var line_end = pos;
        while (line_end < doc.len and doc[line_end] != '\n') : (line_end += 1) {}
        const raw_line = doc[line_start..line_end];
        const line = std.mem.trim(u8, raw_line, " \t\r\n");
        const next_pos = if (line_end < doc.len) line_end + 1 else line_end;
        if (!saw_metadata and line.len == 0) { pos = next_pos; continue; }
        if (isDrawerStart(line)) { in_properties = true; saw_metadata = true; pos = next_pos; continue; }
        if (in_properties) {
            pos = next_pos;
            if (isDrawerEnd(line)) in_properties = false;
            continue;
        }
        if (isOrgMetadataLine(line)) { saw_metadata = true; pos = next_pos; continue; }
        break;
    }
    const body = std.mem.trim(u8, doc[pos..], " \t\r\n");
    return body;
}

fn isOrgMetadataLine(line: []const u8) bool {
    if (!std.mem.startsWith(u8, line, "#+")) return false;
    if (isBeginSrc(line) or isBeginExample(line) or isBeginQuote(line) or isEndBlock(line)) return false;
    return true;
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
        .info => "First-class reference material. Prefer pages with CONTEXT links and test/source evidence.",
        .heading => "Context section. Use connected arrows to verify whether it is current.",
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
    const title = std.fmt.bufPrint(&title_buf, "RELATION TREE  row {d}/{d}  T test S src λ fn ▣ rec I info ◆ head ! todo · ✓ verify ⊢ support ⊣ block ≤ refine ⇢ link", .{ tree_state.cursor + 1, tree_state.row_count }) catch "RELATION TREE";
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
        "Fast lanes: TAB completion, C-i/Alt-i @info, Alt-t @todo, Alt-n @notes, Alt-e @tests, Alt-f @functions",
        "Metadata lanes: Alt-k @contracts, Alt-y @quality, Alt-a @metadata, Alt-l @links, Alt-u @bugs, Alt-r :Record",
        "Natural search: wisp define reader layout, or type arrows like Int -> Int and a -> a",
        "Namespace search: @todo @hot @blocked @bugs @failures @regressions @performance @coverage @docs",
        "More lanes: @examples @tutorials @api @cli @cache @diagnostics @design @links @tables",
        "Kind search: :Test :Function :Record :Source :Concept :Todo :Info",
        "Edge search: %verifies %supports %blocks %refines",
        "Category relation: lhs -> rhs, lhs <- rhs; C-o/Shift-Tab switches panes",
        "Examples: Int -> Int, @tests -> reader, %verifies @tests -> codegen, @contracts -> @source",
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
    if (std.mem.indexOf(u8, line, "@info") != null) return .{ .fg = theme.info, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "notes") != null or std.mem.indexOf(u8, line, "@obs") != null) return .{ .fg = theme.record, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, line, "->") != null or std.mem.indexOf(u8, line, "%") != null) return .{ .fg = theme.accent2, .bg = theme.panel, .bold = true };
    return .{ .fg = theme.ink, .bg = theme.panel };
}

fn highlightStyle(obj: model.Object, theme: palette.Theme, bg: palette.Color, active: bool) palette.Style {
    if (priorityColor(obj.title, theme) orelse priorityColor(obj.preview, theme)) |pc| return .{ .fg = pc, .bg = bg, .bold = true };
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
            if (obj.kind == .record or obj.kind == .heading or obj.kind == .concept) {
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
    t.textClipped(xx, yy, if (r.w > 4) r.w - 4 else 0, "󰄛 Catface help — calm map, live right pane", .{ .fg = theme.ink, .bg = theme.panel_alt, .bold = true });
    yy += 2;
    xx = r.x + 2;
    xx = drawHelpChip(t, xx, yy, "SEARCH", theme.accent2, theme);
    xx = drawHelpChip(t, xx + 1, yy, "INFO", theme.info, theme);
    xx = drawHelpChip(t, xx + 1, yy, "RIGHT-PANE", theme.accent, theme);
    xx = drawHelpChip(t, xx + 1, yy, "LINKS", theme.info, theme);
    _ = drawHelpChip(t, xx + 1, yy, "q/ESC", theme.mute, theme);
    yy += 2;

    const lines = [_][]const u8{
        "Typing is always search. Printable keys insert into the query line.",
        "Move results: ↑/↓ or C-n/C-p. RET focuses the right pane; wheel scrolls Org body, h/j/k/l drives the relation tree.",
        "Fast lanes: TAB palette, C-i/Alt-i @info, Alt-t @todo, Alt-n @notes, Alt-e @tests, Alt-s @source.",
        "More lanes: Alt-u @bugs, Alt-f @functions, Alt-k @contracts, Alt-y @quality, Alt-a @metadata, Alt-l @links, Alt-r :Record.",
        "Language lanes: Alt-w @wisp, Alt-m @reader, Alt-c @codegen. Pane switch: C-o or Shift-Tab.",
        "Objects: :Test :Function :Record :Source :Concept :Todo :Info. IDs: ?id or #id.",
        "Edges: Alt-v %verifies, Alt-x %blocks, plus %supports %refines %mentions %generated-by.",
        "Type search: Int -> Int or a -> a searches Function signatures before relation parsing.",
        "Category syntax: lhs -> rhs and lhs <- rhs ask for arrows between object sets.",
        "Examples: Int -> Int · @tests -> reader · reader <- @tests · %verifies @tests -> codegen.",
        "Graph ops: > outgoing, < incoming, ~ neighborhood, proj conceptual projection.",
        "Symbol algebra: T test, S source, λ function, ▣ record, I info, ◆ heading, ! TODO; arrows ✓ verify, ⊢ support, ⊣ block, ≤ refine, ⇢ link.",
        "Right pane: n/p/TAB jump Org targets; v opens the mini document viewer; q closes it. Links/buttons activate on release.",
        "Mouse: press arms, hover/drag underlines, release activates. Drag away before release cancels.",
        "Org text renders semantically: top metadata chips, colored bullets, TODO/DONE/priority chips, blocks, records, links, and tables.",
        "Try @links, @tables, @docs, @failures, @performance, @coverage, @examples, @diagnostics.",
        "Help-only keys: q closes help, g searches GitHub/README surface, i @info, f @functions.",
        "Wheel scrolls results or the right Org body/viewer. Footer shows frame/query/flush nanosecond timings.",
        "TAB opens the flat Vertico/orderless completion palette. C-c d changes root. C-h c describes the next key. Alt-b goes back.",
        "C-l/C-u clear. Esc closes help/viewer/quits. Palette prompt follows the selected command class.",
    };
    for (lines) |tutorial_text| {
        if (yy >= r.bottom() - 1) break;
        t.textClipped(r.x + 2, yy, if (r.w > 4) r.w - 4 else 0, tutorial_text, tutorialLineStyle(tutorial_text, theme, theme.panel_alt));
        yy += 1;
    }
}

fn drawHelpChip(t: *terminal.Tty, x: u16, y: u16, label: []const u8, color: palette.Color, theme: palette.Theme) u16 {
    var buf: [48]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, " {s} ", .{label}) catch label;
    const label_w: u16 = @intCast(@min(text.len, 30));
    t.text(x, y, "", .{ .fg = color.scale(70, 100), .bg = theme.panel_alt, .bold = true });
    t.textClipped(x + 1, y, label_w, text, .{ .fg = theme.ink, .bg = color.scale(25, 100), .bold = true });
    t.text(x + 1 + label_w, y, "", .{ .fg = color.scale(70, 100), .bg = theme.panel_alt, .bold = true });
    return x + label_w + 2;
}

fn tutorialLineStyle(tutorial_text: []const u8, theme: palette.Theme, bg: palette.Color) palette.Style {
    if (std.mem.indexOf(u8, tutorial_text, "Right pane") != null or std.mem.indexOf(u8, tutorial_text, "Mouse:") != null or std.mem.indexOf(u8, tutorial_text, "Org text") != null) return .{ .fg = theme.info, .bg = bg, .bold = true };
    if (std.mem.indexOf(u8, tutorial_text, "->") != null or std.mem.indexOf(u8, tutorial_text, "%") != null) return .{ .fg = theme.accent2, .bg = bg, .bold = true };
    if (std.mem.indexOf(u8, tutorial_text, "Alt-") != null or std.mem.indexOf(u8, tutorial_text, "C-") != null) return .{ .fg = theme.ink, .bg = bg, .bold = false };
    return .{ .fg = theme.ink, .bg = bg };
}


pub fn drawCommandPalette(t: *terminal.Tty, lay: Layout, ctx: *const model.Context, filter: []const u8, cursor: usize, matches: []const command_palette.Match, selected: usize, cursor_visible: bool, theme: palette.Theme) void {
    const rows: usize = 10;
    const h: u16 = @min(@as(u16, 12), lay.footer.y);
    if (h < 3 or lay.header.w == 0) return;
    const y: u16 = lay.footer.y - h;
    const r = terminal.Rect{ .x = 0, .y = y, .w = lay.header.w, .h = h };
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    if (r.w < 8) return;

    const selected_match: ?command_palette.Match = if (matches.len != 0 and selected < matches.len) matches[selected] else null;
    const selected_item: ?command_palette.CommandItem = if (selected_match) |m| if (m.kind == .command) command_palette.commands[m.index] else null else null;
    const prompt_name = command_palette.promptForMatch(ctx, selected_match);
    const prompt_x = r.x + 1;
    t.textClipped(prompt_x, r.y, @min(@as(u16, 22), r.w - 1), prompt_name, .{ .fg = theme.accent2, .bg = theme.panel_alt, .bold = true });
    const edit_x = prompt_x + @as(u16, @intCast(@min(prompt_name.len + 1, @as(usize, 24))));
    if (edit_x < r.right()) {
        t.textClipped(edit_x, r.y, r.right() - edit_x, filter, .{ .fg = theme.ink, .bg = theme.panel_alt, .bold = true });
        if (cursor_visible) {
            const cx: u16 = @intCast(@min(@as(usize, r.right() - 1), @as(usize, edit_x) + @min(cursor, filter.len)));
            const cp: u21 = if (cursor < filter.len) @as(u21, filter[cursor]) else ' ';
            t.set(cx, r.y, cp, .{ .fg = theme.panel_alt, .bg = theme.ink, .bold = true });
        }
    }

    if (r.h > 1) {
        const doc = if (selected_item) |item| item.description else "orderless completion · C-n/C-p move · C-c p/n bounds · RET accept · Esc/C-g cancel";
        t.textClipped(r.x + 1, r.y + 1, if (r.w > 2) r.w - 2 else r.w, doc, .{ .fg = theme.mute, .bg = theme.panel_alt });
    }

    var yy = r.y + 2;
    var row: usize = 0;
    while (row < matches.len and row < rows and yy < r.bottom()) : (row += 1) {
        const m = matches[row];
        const is_sel = row == selected;
        const item_name = command_palette.matchName(ctx, m);
        const item_desc = command_palette.matchDescription(ctx, m);
        const item_insert = command_palette.matchInsert(ctx, m);
        const bg = if (is_sel) theme.panel else theme.panel_alt;
        const fg = if (is_sel) theme.ink else theme.mute;
        t.fill(.{ .x = r.x, .y = yy, .w = r.w, .h = 1 }, ' ', .{ .fg = fg, .bg = bg });
        const marker = if (is_sel) "›" else " ";
        t.text(r.x + 1, yy, marker, .{ .fg = theme.accent, .bg = bg, .bold = is_sel });
        t.textClipped(r.x + 3, yy, @min(@as(u16, 24), if (r.w > 4) r.w - 4 else 0), item_name, .{ .fg = if (is_sel) theme.accent2 else fg, .bg = bg, .bold = is_sel });
        const query_x = r.x + 29;
        if (query_x < r.right()) {
            t.textClipped(query_x, yy, @min(@as(u16, 20), r.right() - query_x), item_insert, .{ .fg = theme.info, .bg = bg, .bold = is_sel });
        }
        const desc_x = r.x + 52;
        if (desc_x < r.right()) {
            t.textClipped(desc_x, yy, r.right() - desc_x, item_desc, .{ .fg = fg, .bg = bg, .dim = !is_sel });
        }
        yy += 1;
    }
    if (matches.len == 0 and yy < r.bottom()) {
        t.textClipped(r.x + 1, yy, if (r.w > 2) r.w - 2 else r.w, "No candidates. Try bugs, failures, perf, coverage, examples, docs, functions, directory.", .{ .fg = theme.warn, .bg = theme.panel_alt });
    }
}

pub fn drawDirectoryPrompt(t: *terminal.Tty, lay: Layout, current_root: []const u8, text_value: []const u8, cursor: usize, cursor_visible: bool, theme: palette.Theme) void {
    if (lay.footer.y < 2) return;
    const r = terminal.Rect{ .x = 0, .y = lay.footer.y - 2, .w = lay.header.w, .h = 2 };
    t.fill(r, ' ', .{ .fg = theme.ink, .bg = theme.panel_alt });
    if (r.w < 8) return;
    const label = "directory root:";
    t.textClipped(r.x + 1, r.y, @min(@as(u16, label.len), r.w - 1), label, .{ .fg = theme.accent2, .bg = theme.panel_alt, .bold = true });
    const edit_x = r.x + 1 + @as(u16, @intCast(label.len + 1));
    if (edit_x < r.right()) {
        t.textClipped(edit_x, r.y, r.right() - edit_x, text_value, .{ .fg = theme.ink, .bg = theme.panel_alt, .bold = true });
        if (cursor_visible) {
            const cx: u16 = @intCast(@min(@as(usize, r.right() - 1), @as(usize, edit_x) + @min(cursor, text_value.len)));
            const cp: u21 = if (cursor < text_value.len) @as(u21, text_value[cursor]) else ' ';
            t.set(cx, r.y, cp, .{ .fg = theme.panel_alt, .bg = theme.ink, .bold = true });
        }
    }
    var help_buf: [1024]u8 = undefined;
    const help = std.fmt.bufPrint(&help_buf, "current {s} · recursive scan starts here · RET reloads · C-a/C-e/Home/End · Esc cancels", .{current_root}) catch "RET reloads · Esc cancels";
    t.textClipped(r.x + 1, r.y + 1, if (r.w > 2) r.w - 2 else r.w, help, .{ .fg = theme.mute, .bg = theme.panel_alt });
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
        .info => "first-class info/reference page",
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



test "org renderer skips metadata drawers and block delimiters" {
    const obj = model.Object{
        .id = "doc.example",
        .kind = .info,
        .title = "Doc",
        .preview = "#+TITLE: Doc\n:PROPERTIES:\n:ID: doc.example\n:END:\n#+BEGIN_EXAMPLE\nvalue\n#+END_EXAMPLE\n* Heading",
    };
    const body = objectDocumentBody(obj);
    try std.testing.expect(std.mem.indexOf(u8, body, "#+TITLE") == null);
    try std.testing.expect(documentBodyLineCount(obj) == 2);
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

test "record header fields are rendered as structured metadata" {
    const obj = model.Object{ .id = "r", .kind = .record, .title = "OBS", .preview = "[OBS id:x src:file.org conf:high supports:y] Reader layout is stable." };
    try std.testing.expect(std.mem.eql(u8, recordHeaderField(obj.preview, "id").?, "x"));
    try std.testing.expect(std.mem.eql(u8, recordHeaderField(obj.preview, "src").?, "file.org"));
    try std.testing.expect(std.mem.eql(u8, recordHeaderField(obj.preview, "supports").?, "y"));
}

test "org links and tables are recognized for living right pane" {
    try std.testing.expect(isOrgTableLine("| key | value |"));
    try std.testing.expect(std.mem.eql(u8, orgLinkLabel("[[id:monadc.context][context]]"), "context"));
    try std.testing.expect(std.mem.eql(u8, findFirstOrgTarget("see [[file:README.md][readme]]").?, "file:README.md"));
}

test "detail text action maps org file and id targets" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "info.a", .kind = .info, .title = "A", .path = "context/a.org", .preview = "see [[file:README.md][readme]]" });
    _ = try ctx.addObject(.{ .id = "target.id", .kind = .record, .title = "Target", .path = "context/t.org", .preview = "target" });
    const action_file = actionFromOrgTarget(&ctx, ctx.objects.items[0], "file:README.md");
    switch (action_file) {
        .open_file => |path| try std.testing.expect(std.mem.eql(u8, path, "README.md")),
        else => return error.ExpectedOpenFile,
    }
    const action_id = actionFromOrgTarget(&ctx, ctx.objects.items[0], "id:target.id");
    switch (action_id) {
        .select => |idx| try std.testing.expect(std.mem.eql(u8, ctx.objects.items[idx].id, "target.id")),
        else => return error.ExpectedSelect,
    }
}

test "org body strips top metadata and target navigation sees body links" {
    const obj = model.Object{
        .id = "doc",
        .kind = .info,
        .title = "Doc",
        .path = "context/doc.org",
        .preview = "#+TITLE: Doc\n#+FILETAGS: :catface:org:\n:PROPERTIES:\n:ID: doc\n:END:\n* TODO [#A] Readable heading\nSee [[id:next][next]].",
    };
    const body = objectDocumentBody(obj);
    try std.testing.expect(std.mem.indexOf(u8, body, "#+TITLE") == null);
    try std.testing.expect(std.mem.indexOf(u8, body, ":PROPERTIES:") == null);
    try std.testing.expect(std.mem.indexOf(u8, body, "* TODO") != null);
    try std.testing.expect(nextDocumentTargetLine(obj, 0, 1) != null);
}

test "org headings and trailing tags are navigable buttons" {
    const obj = model.Object{
        .id = "doc.tags",
        .kind = .info,
        .title = "Doc tags",
        .preview = "#+TITLE: Doc\n* TODO Heading :reader:wisp:\nPlain body\n** DONE Child :done:",
    };
    try std.testing.expect(documentBodyLineCount(obj) == 3);
    try std.testing.expect(nextDocumentTargetLine(obj, 0, 1) != null);
    const body = objectDocumentBody(obj);
    try std.testing.expect(std.mem.indexOf(u8, body, "#+TITLE") == null);
    try std.testing.expect(firstOrgTag("* TODO Heading :reader:wisp:").?.len > 0);
}

test "org line action maps heading and tag to semantic actions" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    const obj = model.Object{ .id = "doc", .kind = .info, .title = "Doc", .preview = "* TODO Heading :reader:" };
    const tag_action = actionFromOrgLine(&ctx, obj, "* TODO Heading :reader:");
    switch (tag_action) {
        .tag => |tag| try std.testing.expect(std.mem.eql(u8, tag, "reader")),
        else => return error.ExpectedOrgTagAction,
    }
    const heading_action = actionFromOrgLine(&ctx, obj, "* TODO Heading");
    switch (heading_action) {
        .heading => |heading| try std.testing.expect(std.mem.indexOf(u8, heading, "Heading") != null),
        else => return error.ExpectedOrgHeadingAction,
    }
}
