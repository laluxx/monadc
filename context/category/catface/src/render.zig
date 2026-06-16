const std = @import("std");
const model = @import("model.zig");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const glyphs = @import("glyphs.zig");
const math = @import("math.zig");

pub const Layout = struct {
    header: terminal.Rect,
    left: terminal.Rect,
    right: terminal.Rect,
    footer: terminal.Rect,
};

pub fn layout(w: u16, h: u16) Layout {
    const header_h: u16 = 3;
    const footer_h: u16 = 2;
    const body_h: u16 = if (h > header_h + footer_h) h - header_h - footer_h else 0;
    const left_w: u16 = if (w < 100) w / 2 else (w * 46) / 100;
    return .{
        .header = .{ .x = 0, .y = 0, .w = w, .h = header_h },
        .left = .{ .x = 0, .y = header_h, .w = left_w, .h = body_h },
        .right = .{ .x = left_w, .y = header_h, .w = w - left_w, .h = body_h },
        .footer = .{ .x = 0, .y = h - footer_h, .w = w, .h = footer_h },
    };
}

pub fn drawHeader(t: *terminal.Tty, r: terminal.Rect, query_text: []const u8, cursor: usize, cursor_visible: bool, active: bool, ctx: *const model.Context, theme: palette.Theme) void {
    const style = palette.Style{ .fg = theme.ink, .bg = theme.bg };
    t.fill(r, ' ', style);
    t.text(2, 0, glyphs.logo, .{ .fg = theme.accent, .bg = theme.bg, .bold = true });
    var stats_buf: [256]u8 = undefined;
    const stats = std.fmt.bufPrint(&stats_buf, "objects {d}  arrows {d}  H(kind) {d:.2} bits", .{ ctx.objects.items.len, ctx.edges.items.len, math.objectEntropy(ctx) }) catch "";
    if (r.w > stats.len + 4) t.textClipped(r.w - @as(u16, @intCast(stats.len)) - 2, 0, @as(u16, @intCast(stats.len)), stats, .{ .fg = theme.mute, .bg = theme.bg });
    t.fill(.{ .x = 0, .y = 1, .w = r.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = theme.panel });
    t.text(2, 1, glyphs.query_prompt, .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
    t.textClipped(4, 1, if (r.w > 6) r.w - 6 else 0, query_text, .{ .fg = theme.ink, .bg = theme.panel, .bold = active });
    if (active and cursor_visible and r.w > 6) {
        const cx: u16 = @intCast(@min(@as(usize, r.w - 2), 4 + cursor));
        const cp: u21 = if (cursor < query_text.len) @as(u21, query_text[cursor]) else ' ';
        t.set(cx, 1, cp, .{ .fg = theme.panel, .bg = theme.ink, .bold = true });
    }
    t.text(2, 2, "fuzzy words  :Kind  @tests/@info/@source  ?id/#id  > out  < in  ~ neigh  proj", .{ .fg = theme.mute, .bg = theme.bg });
}
pub fn drawFooter(t: *terminal.Tty, r: terminal.Rect, msg: []const u8, theme: palette.Theme) void {
    t.fill(r, ' ', .{ .fg = theme.mute, .bg = theme.bg });
    t.textClipped(2, r.y, r.w - 4, "Tab focus  Enter follow  o outgoing  i incoming  n neighborhood  b back  / query  q quit", .{ .fg = theme.mute, .bg = theme.bg });
    t.textClipped(2, r.y + 1, r.w - 4, msg, .{ .fg = theme.warn, .bg = theme.bg });
}

pub fn drawResults(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, results: []const usize, selected: usize, scroll: usize, active: bool, theme: palette.Theme) void {
    t.box(r, if (active) "* search / universal results" else "  search / universal results", active, theme);
    const inner = r.inset(1);
    if (inner.h == 0) return;
    var y = inner.y;
    var i = scroll;
    while (i < results.len and y < inner.bottom()) : ({ i += 1; y += 1; }) {
        const idx = results[i];
        const obj = ctx.objects.items[idx];
        const is_sel = i == selected;
        const bg = if (is_sel) theme.panel_alt else theme.panel;
        const st = palette.Style{ .fg = theme.kindColor(obj.kind), .bg = bg, .bold = is_sel and active };
        t.fill(.{ .x = inner.x, .y = y, .w = inner.w, .h = 1 }, ' ', .{ .fg = theme.ink, .bg = bg });
        t.text(inner.x, y, if (is_sel) "›" else " ", .{ .fg = theme.accent, .bg = bg, .bold = is_sel });
        t.text(inner.x + 2, y, glyphs.kind(obj.kind), st);
        var label_buf: [1024]u8 = undefined;
        const label = std.fmt.bufPrint(&label_buf, " {s}", .{if (obj.title.len != 0) obj.title else obj.id}) catch obj.id;
        const label_style = highlightStyle(obj, theme, bg, is_sel and active);
        t.textClipped(inner.x + 4, y, if (inner.w > 6) inner.w - 6 else 0, label, label_style);
    }
    if (results.len == 0) {
        t.text(inner.x + 2, inner.y + 1, "No objects match. Try @tests, @info, @source, or loosen fuzzy terms.", .{ .fg = theme.bad, .bg = theme.panel });
    }
}

pub fn drawDetail(t: *terminal.Tty, r: terminal.Rect, ctx: *const model.Context, idx_opt: ?usize, active: bool, theme: palette.Theme) void {
    t.box(r, if (active) "* focus / Hom" else "  focus / Hom", active, theme);
    const inner = r.inset(1);
    if (idx_opt == null) {
        t.text(inner.x + 2, inner.y + 2, "Select an object and press Enter.", .{ .fg = theme.mute, .bg = theme.panel });
        return;
    }
    const idx = idx_opt.?;
    const obj = ctx.objects.items[idx];
    var y = inner.y;
    t.text(inner.x, y, glyphs.kind(obj.kind), .{ .fg = theme.kindColor(obj.kind), .bg = theme.panel, .bold = true });
    t.textClipped(inner.x + 2, y, inner.w - 2, obj.id, .{ .fg = theme.accent, .bg = theme.panel, .bold = true });
    y += 1;
    if (obj.title.len != 0) { t.textClipped(inner.x, y, inner.w, obj.title, highlightStyle(obj, theme, theme.panel, true)); y += 1; }
    var meta_buf: [512]u8 = undefined;
    const meta = std.fmt.bufPrint(&meta_buf, "{s}  @{s}:{d}", .{ model.Context.kindName(obj.kind), obj.path, obj.line }) catch "";
    t.textClipped(inner.x, y, inner.w, meta, .{ .fg = theme.mute, .bg = theme.panel });
    y += 2;
    if (obj.preview.len != 0 and y < inner.bottom()) {
        y = drawWrapped(t, inner.x, y, inner.w, inner.bottom(), obj.preview, highlightStyle(obj, theme, theme.panel, false));
        y += 1;
    }
    if (y < inner.bottom()) {
        t.text(inner.x, y, "Hom(object, -)", .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
        y += 1;
        y = drawEdges(t, inner.x, y, inner.w, inner.bottom(), ctx, obj.id, .out, theme);
    }
    if (y + 2 < inner.bottom()) {
        y += 1;
        t.text(inner.x, y, "Hom(-, object)", .{ .fg = theme.accent2, .bg = theme.panel, .bold = true });
        y += 1;
        _ = drawEdges(t, inner.x, y, inner.w, inner.bottom(), ctx, obj.id, .in, theme);
    }
}


fn highlightStyle(obj: model.Object, theme: palette.Theme, bg: palette.Color, active: bool) palette.Style {
    if (obj.kind == .test_kind or hasToken(obj.title, "TEST") or hasToken(obj.preview, "TEST")) return .{ .fg = theme.test_color, .bg = bg, .bold = true };
    if (obj.kind == .todo or hasToken(obj.title, "TODO") or hasToken(obj.preview, "TODO")) return .{ .fg = theme.todo, .bg = bg, .bold = true };
    if (obj.kind == .done or hasToken(obj.title, "DONE") or hasToken(obj.preview, "DONE")) return .{ .fg = theme.done, .bg = bg, .bold = true };
    if (obj.kind == .info or hasToken(obj.tags, "@info")) return .{ .fg = theme.info, .bg = bg, .bold = active };
    return .{ .fg = theme.ink, .bg = bg, .bold = active };
}

fn hasToken(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}
const Dir = enum { in, out };

fn drawEdges(t: *terminal.Tty, x: u16, y0: u16, w: u16, bottom: u16, ctx: *const model.Context, id: []const u8, dir: Dir, theme: palette.Theme) u16 {
    var y = y0;
    var shown: usize = 0;
    for (ctx.edges.items) |e| {
        if (y >= bottom or shown >= 8) break;
        const keep = switch (dir) { .out => std.mem.eql(u8, e.src, id), .in => std.mem.eql(u8, e.dst, id) };
        if (!keep) continue;
        const other = switch (dir) { .out => e.dst, .in => e.src };
        var buf: [1024]u8 = undefined;
        const txt = std.fmt.bufPrint(&buf, "[{s}] {s} -> {s}", .{ model.Context.edgeName(e.kind), glyphs.edge(e.kind), other }) catch other;
        t.textClipped(x, y, w, txt, .{ .fg = theme.mute, .bg = theme.panel });
        shown += 1;
        y += 1;
    }
    if (shown == 0 and y < bottom) {
        t.text(x, y, "∅", .{ .fg = theme.edge, .bg = theme.panel });
        y += 1;
    }
    return y;
}

fn drawWrapped(t: *terminal.Tty, x: u16, y0: u16, w: u16, bottom: u16, text: []const u8, style: palette.Style) u16 {
    var y = y0;
    var start: usize = 0;
    while (start < text.len and y < bottom) {
        var end = @min(text.len, start + w);
        if (end < text.len) {
            if (std.mem.lastIndexOfScalar(u8, text[start..end], ' ')) |sp| end = start + sp;
        }
        t.textClipped(x, y, w, std.mem.trim(u8, text[start..end], " \t"), style);
        start = @min(text.len, end + 1);
        y += 1;
    }
    return y;
}


pub fn drawTutorial(t: *terminal.Tty, lay: Layout, theme: palette.Theme) void {
    const w: u16 = if (lay.header.w > 92) 88 else if (lay.header.w > 10) lay.header.w - 6 else lay.header.w;
    const h: u16 = if (lay.left.h > 20) 18 else if (lay.left.h > 6) lay.left.h - 2 else lay.left.h;
    const x: u16 = if (lay.header.w > w) (lay.header.w - w) / 2 else 0;
    const y: u16 = if (lay.left.h > h) lay.left.y + (lay.left.h - h) / 2 else lay.left.y;
    const r = terminal.Rect{ .x = x, .y = y, .w = w, .h = h };
    t.box(r, "Catface tutorial  (?)", true, theme);
    const inner = r.inset(1);
    var yy = inner.y;
    const lines = [_][]const u8{
        "Goal: one universal search over context, tests, source, scripts, TODO, DONE, and info.",
        "Type words to fuzzy-search. Use namespaces: @tests @info @source @todo @done.",
        "Filters: :Record :Test :Source :Info :Todo :Done. Identity: ?id or #id.",
        "Category ops: > outgoing Hom(object,-), < incoming Hom(-,object), ~ neighborhood, proj concept closure.",
        "Navigation: C-n/C-p or j/k move, Enter focuses/follows, Tab switches panes, mouse selects rows.",
        "Editing: C-a start, C-e end, C-d delete, C-k kill line, M-d kill word, C-y yank.",
        "Kill ring appends when consecutive kill commands are used, like Emacs.",
        "The block cursor blinks with 0.5s delay and 0.5s interval; typing resets it.",
        "Right pane shows object source, preview, and clickable-looking node buttons [id].",
        "q/Esc quits. ? toggles this overlay.",
    };
    for (lines) |tutorial_text| {
        if (yy >= inner.bottom()) break;
        t.textClipped(inner.x + 1, yy, inner.w - 2, tutorial_text, tutorialLineStyle(tutorial_text, theme));
        yy += 1;
    }
}

fn tutorialLineStyle(tutorial_text: []const u8, theme: palette.Theme) palette.Style {
    if (std.mem.indexOf(u8, tutorial_text, "C-") != null or std.mem.indexOf(u8, tutorial_text, "M-") != null) return .{ .fg = theme.accent2, .bg = theme.panel, .bold = true };
    if (std.mem.indexOf(u8, tutorial_text, "@tests") != null or std.mem.indexOf(u8, tutorial_text, "@info") != null) return .{ .fg = theme.test_color, .bg = theme.panel, .bold = true };
    return .{ .fg = theme.ink, .bg = theme.panel };
}

pub fn writeObjectCard(buf: *std.array_list.Managed(u8), ctx: *const model.Context, idx: usize, width: usize) !void {
    const obj = ctx.objects.items[idx];
    try boxLine(buf, width, '╭', '─', '╮');
    try centered(buf, width, obj.id);
    try buf.appendSlice("\n");
    try field(buf, width, "kind", model.Context.kindName(obj.kind));
    try field(buf, width, "path", obj.path);
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
    try buf.appendSlice(name[0..ncut]); used += ncut;
    if (used < inner) { try buf.appendSlice(": "); used += 2; }
    const vcut = @min(value.len, inner - @min(used, inner));
    try buf.appendSlice(value[0..vcut]); used += vcut;
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
