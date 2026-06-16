const std = @import("std");
const model = @import("model.zig");
const fuzzy = @import("fuzzy.zig");

pub const EvalOptions = struct {
    limit: usize = 80,
};

pub const Result = struct {
    object_index: usize,
    score: i32,
};

pub const ResultList = struct {
    allocator: std.mem.Allocator,
    items: []Result,

    pub fn deinit(self: *ResultList) void {
        self.allocator.free(self.items);
    }
};

const TokenKind = enum { word, kind_filter, path_filter, id_filter, op_out, op_in, op_neighborhood, op_project, op_clear, lparen, rparen };

const Token = struct {
    kind: TokenKind,
    text: []const u8,
};

pub fn evaluate(allocator: std.mem.Allocator, ctx: *const model.Context, expr: []const u8, options: EvalOptions) !ResultList {
    var tokens = try tokenize(allocator, expr);
    defer tokens.deinit();

    var set = try initAll(allocator, ctx);
    defer set.deinit();
    var terms = std.array_list.Managed([]const u8).init(allocator);
    defer terms.deinit();

    var had_word = false;
    for (tokens.items) |tok| {
        switch (tok.kind) {
            .word => {
                try applyWord(ctx, &set, tok.text);
                try terms.append(tok.text);
                had_word = true;
            },
            .kind_filter => try applyKind(ctx, &set, tok.text),
            .path_filter => try applyPath(ctx, &set, tok.text),
            .id_filter => try applyId(ctx, &set, tok.text),
            .op_out => try expandEdges(allocator, ctx, &set, .out),
            .op_in => try expandEdges(allocator, ctx, &set, .in),
            .op_neighborhood => try expandEdges(allocator, ctx, &set, .both),
            .op_project => try projectSuperset(allocator, ctx, &set),
            .op_clear => { for (set.items) |*b| b.* = true; },
            .lparen, .rparen => {},
        }
    }

    if (!had_word and tokens.items.len == 0) {
        for (set.items) |*b| b.* = true;
    }

    var ranked = std.array_list.Managed(Result).init(allocator);
    defer ranked.deinit();
    for (set.items, 0..) |keep, i| {
        if (!keep) continue;
        const obj = ctx.objects.items[i];
        const sc = rankObject(obj, terms.items);
        try ranked.append(.{ .object_index = i, .score = sc });
    }
    std.mem.sort(Result, ranked.items, ctx, lessResult);
    const n = @min(ranked.items.len, options.limit);
    const out = try allocator.dupe(Result, ranked.items[0..n]);
    return .{ .allocator = allocator, .items = out };
}

fn lessResult(ctx: *const model.Context, a: Result, b: Result) bool {
    _ = ctx;
    return a.score > b.score;
}

fn rankObject(obj: model.Object, terms: []const []const u8) i32 {
    var total: i32 = obj.weight;
    for (terms) |t| {
        var best = fuzzy.score(obj.id, t);
        const st = fuzzy.score(obj.title, t);
        if (st.value > best.value) best = st;
        const sp = fuzzy.score(obj.path, t);
        if (sp.value > best.value) best = sp;
        const sg = fuzzy.score(obj.tags, t);
        if (sg.value > best.value) best = sg;
        total += best.value;
    }
    total += switch (obj.kind) {
        .record => 15,
        .heading => 10,
        .concept => 12,
        .script => 6,
        .test_kind => 20,
        .todo => 18,
        .done => 14,
        .info => 12,
        .source => 8,
        else => 0,
    };
    return total;
}

fn initAll(allocator: std.mem.Allocator, ctx: *const model.Context) !std.array_list.Managed(bool) {
    var set = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try set.appendNTimes(true, ctx.objects.items.len);
    return set;
}

fn applyWord(ctx: *const model.Context, set: *std.array_list.Managed(bool), word: []const u8) !void {
    try tryString(word);
    for (set.items, 0..) |*keep, i| {
        if (!keep.*) continue;
        const obj = ctx.objects.items[i];
        const id = fuzzy.score(obj.id, word);
        const title = fuzzy.score(obj.title, word);
        const path = fuzzy.score(obj.path, word);
        const preview = fuzzy.score(obj.preview, word);
        const tags = fuzzy.score(obj.tags, word);
        keep.* = id.matched or title.matched or path.matched or preview.matched or tags.matched;
    }
}

fn applyKind(ctx: *const model.Context, set: *std.array_list.Managed(bool), name: []const u8) !void {
    const k = model.Context.parseObjectKind(name);
    for (set.items, 0..) |*keep, i| {
        if (keep.*) {
            keep.* = ctx.objects.items[i].kind == k;
        }
    }
}

fn applyPath(ctx: *const model.Context, set: *std.array_list.Managed(bool), path: []const u8) !void {
    for (set.items, 0..) |*keep, i| {
        if (keep.*) {
            keep.* = matchNamespace(ctx.objects.items[i], path);
        }
    }
}

fn matchNamespace(obj: model.Object, ns: []const u8) bool {
    if (std.mem.eql(u8, ns, "tests") or std.mem.eql(u8, ns, "test")) {
        return obj.kind == .test_kind or contains(obj.path, "tests/") or contains(obj.tags, "@tests");
    }
    if (std.mem.eql(u8, ns, "info")) {
        return obj.kind == .info or contains(obj.tags, "@info") or contains(obj.path, "info") or contains(obj.path, "docs") or contains(obj.title, "Info");
    }
    if (std.mem.eql(u8, ns, "source") or std.mem.eql(u8, ns, "src")) {
        return obj.kind == .source or obj.kind == .script or contains(obj.tags, "@source") or contains(obj.path, "src/");
    }
    if (std.mem.eql(u8, ns, "todo")) return obj.kind == .todo or contains(obj.title, "TODO") or contains(obj.preview, "TODO");
    if (std.mem.eql(u8, ns, "done")) return obj.kind == .done or contains(obj.title, "DONE") or contains(obj.preview, "DONE");
    return contains(obj.path, ns) or contains(obj.tags, ns) or contains(obj.title, ns) or contains(obj.preview, ns);
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

fn applyId(ctx: *const model.Context, set: *std.array_list.Managed(bool), id: []const u8) !void {
    for (set.items, 0..) |*keep, i| {
        if (keep.*) {
            keep.* = std.mem.indexOf(u8, ctx.objects.items[i].id, id) != null;
        }
    }
}

const Direction = enum { in, out, both };

fn expandEdges(allocator: std.mem.Allocator, ctx: *const model.Context, set: *std.array_list.Managed(bool), dir: Direction) !void {
    var next = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try next.appendNTimes(false, ctx.objects.items.len);
    for (ctx.edges.items) |e| {
        const si = ctx.findObject(e.src) orelse continue;
        const di = ctx.findObject(e.dst) orelse continue;
        switch (dir) {
            .out => {
                if (set.items[si]) next.items[di] = true;
            },
            .in => {
                if (set.items[di]) next.items[si] = true;
            },
            .both => {
                if (set.items[si]) next.items[di] = true;
                if (set.items[di]) next.items[si] = true;
            },
        }
    }
    @memcpy(set.items, next.items);
    next.deinit();
}

fn projectSuperset(allocator: std.mem.Allocator, ctx: *const model.Context, set: *std.array_list.Managed(bool)) !void {
    // Projection: keep current objects and add their conceptual codomains.
    var next = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try next.appendSlice(set.items);
    for (ctx.edges.items) |e| {
        if (e.kind != .classifies_as and e.kind != .forgets_to and e.kind != .refines) continue;
        const si = ctx.findObject(e.src) orelse continue;
        const di = ctx.findObject(e.dst) orelse continue;
        if (set.items[si]) next.items[di] = true;
    }
    @memcpy(set.items, next.items);
    next.deinit();
}

fn tokenize(allocator: std.mem.Allocator, expr: []const u8) !std.array_list.Managed(Token) {
    var tokens = std.array_list.Managed(Token).init(allocator);
    var i: usize = 0;
    while (i < expr.len) {
        while (i < expr.len and std.ascii.isWhitespace(expr[i])) i += 1;
        if (i >= expr.len) break;
        const c = expr[i];
        if (c == '(') { try tokens.append(.{ .kind = .lparen, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (c == ')') { try tokens.append(.{ .kind = .rparen, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (c == '>') { try tokens.append(.{ .kind = .op_out, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (c == '<') { try tokens.append(.{ .kind = .op_in, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (c == '~') { try tokens.append(.{ .kind = .op_neighborhood, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (c == '!') { try tokens.append(.{ .kind = .op_clear, .text = expr[i .. i + 1] }); i += 1; continue; }
        if (startsOpWord(expr, i, "proj")) { try tokens.append(.{ .kind = .op_project, .text = expr[i .. i + "proj".len] }); i += "proj".len; continue; }
        if (startsOpWord(expr, i, "pi")) { try tokens.append(.{ .kind = .op_project, .text = expr[i .. i + "pi".len] }); i += "pi".len; continue; }
        var j = i;
        while (j < expr.len and !std.ascii.isWhitespace(expr[j]) and expr[j] != '(' and expr[j] != ')' and expr[j] != '>' and expr[j] != '<' and expr[j] != '~') j += 1;
        const raw = expr[i..j];
        if (raw.len == 0) { i += 1; continue; }
        const kind: TokenKind = if (raw[0] == ':') .kind_filter else if (raw[0] == '@') .path_filter else if (raw[0] == '?' or raw[0] == '#') .id_filter else .word;
        const text = if (kind == .word) raw else raw[1..];
        try tokens.append(.{ .kind = kind, .text = text });
        i = j;
    }
    return tokens;
}

fn startsOpWord(expr: []const u8, i: usize, word: []const u8) bool {
    if (!std.mem.startsWith(u8, expr[i..], word)) return false;
    const end = i + word.len;
    return end == expr.len or std.ascii.isWhitespace(expr[end]) or expr[end] == ')' or expr[end] == '(';
}

fn tryString(s: []const u8) !void { _ = s; }

test "query tokenizer" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var t = try tokenize(arena.allocator(), "typed :Record @wisp > proj");
    defer t.deinit();
    try std.testing.expect(t.items.len == 5);
    try std.testing.expect(t.items[1].kind == .kind_filter);
    try std.testing.expect(t.items[3].kind == .op_out);
    try std.testing.expect(t.items[4].kind == .op_project);
}
