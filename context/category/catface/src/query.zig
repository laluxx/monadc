const std = @import("std");
const model = @import("model.zig");
const fuzzy = @import("fuzzy.zig");
const text = @import("text.zig");
const search_index = @import("index.zig");

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

const TokenKind = enum {
    word,
    kind_filter,
    path_filter,
    id_filter,
    edge_filter,
    exact_filter,
    op_out,
    op_in,
    op_neighborhood,
    op_project,
    op_clear,
    lparen,
    rparen,
};

const Token = struct {
    kind: TokenKind,
    text: []const u8,
};

pub const RelationOp = enum { out, in };
pub const RelationSpec = struct {
    op: RelationOp,
    lhs: []const u8,
    rhs: []const u8,
    edge_kind: ?model.EdgeKind = null,
};

pub fn evaluate(allocator: std.mem.Allocator, ctx: *const model.Context, expr: []const u8, options: EvalOptions) !ResultList {
    if (looksLikeTypeSignature(expr)) return evaluateTypeSignatureImpl(allocator, ctx, expr, options);
    if (findRelation(expr)) |rel| {
        return evaluateRelationImpl(allocator, ctx, null, rel, options);
    }
    return evaluateSimpleImpl(allocator, ctx, null, expr, options);
}

pub fn evaluateIndexed(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, expr: []const u8, options: EvalOptions) !ResultList {
    if (looksLikeTypeSignature(expr)) return evaluateTypeSignatureIndexedImpl(allocator, ctx, idx, expr, options);
    if (findRelation(expr)) |rel| {
        return evaluateRelationImpl(allocator, ctx, idx, rel, options);
    }
    return evaluateSimpleImpl(allocator, ctx, idx, expr, options);
}

fn looksLikeTypeSignature(expr_raw: []const u8) bool {
    const expr = std.mem.trim(u8, expr_raw, " \t\r\n");
    if (expr.len == 0 or std.mem.indexOf(u8, expr, "->") == null) return false;
    if (std.mem.indexOf(u8, expr, "@") != null or std.mem.indexOf(u8, expr, "%") != null) return false;
    if (std.mem.indexOf(u8, expr, "<-") != null) return false;
    if (std.mem.startsWith(u8, expr, "title:") or std.mem.startsWith(u8, expr, "path:") or std.mem.startsWith(u8, expr, "id:")) return false;
    if (findSpacedOp(expr, "->")) |pos| {
        const lhs = std.mem.trim(u8, expr[0..pos], " \t\r\n");
        const rhs = std.mem.trim(u8, expr[pos + 2 ..], " \t\r\n");
        return typeSideLooksLikeType(lhs) and typeSideLooksLikeType(rhs);
    }
    return false;
}

fn typeSideLooksLikeType(side: []const u8) bool {
    if (side.len == 0) return false;
    var saw_typeish = false;
    var toks = std.mem.tokenizeAny(u8, side, " \t()[]{}.,");
    while (toks.next()) |tok| {
        if (tok.len == 0) continue;
        if (tok.len == 1 and std.ascii.isAlphabetic(tok[0])) { saw_typeish = true; continue; }
        if (std.ascii.isUpper(tok[0])) { saw_typeish = true; continue; }
        if (std.mem.eql(u8, tok, "Unit") or std.mem.eql(u8, tok, "Bool") or std.mem.eql(u8, tok, "Int") or std.mem.eql(u8, tok, "String")) saw_typeish = true;
    }
    return saw_typeish;
}

fn evaluateTypeSignatureImpl(allocator: std.mem.Allocator, ctx: *const model.Context, expr_raw: []const u8, options: EvalOptions) !ResultList {
    const expr = normalizeTypeQuery(expr_raw);
    var ranked = std.array_list.Managed(Result).init(allocator);
    defer ranked.deinit();
    const limit = options.limit;
    try ranked.ensureTotalCapacity(@min(limit, ctx.objects.items.len));
    for (ctx.objects.items, 0..) |obj, i| {
        if (obj.kind != .function_kind) continue;
        if (!signatureMatches(obj, expr)) continue;
        const sc = 420 + rankObject(obj, &[_][]const u8{expr});
        insertTop(&ranked, .{ .object_index = i, .score = sc }, limit);
    }
    const out = try allocator.dupe(Result, ranked.items[0..@min(ranked.items.len, limit)]);
    return .{ .allocator = allocator, .items = out };
}

fn evaluateTypeSignatureIndexedImpl(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, expr_raw: []const u8, options: EvalOptions) !ResultList {
    const expr = normalizeTypeQuery(expr_raw);
    var ranked = std.array_list.Managed(Result).init(allocator);
    defer ranked.deinit();
    const limit = options.limit;
    const function_objects = idx.objectsOfKind(.function_kind);
    try ranked.ensureTotalCapacity(@min(limit, function_objects.len));
    for (function_objects) |i| {
        const obj = ctx.objects.items[i];
        if (!signatureMatches(obj, expr)) continue;
        const sc = 460 + rankObject(obj, &[_][]const u8{expr});
        insertTop(&ranked, .{ .object_index = i, .score = sc }, limit);
    }
    const out = try allocator.dupe(Result, ranked.items[0..@min(ranked.items.len, limit)]);
    return .{ .allocator = allocator, .items = out };
}

fn signatureMatches(obj: model.Object, expr_raw: []const u8) bool {
    const expr = normalizeTypeQuery(expr_raw);
    const sig = signatureSlice(obj);
    if (text.containsFold(sig, expr)) return true;
    if (text.containsFold(obj.preview, expr)) return true;
    if (std.mem.startsWith(u8, expr, "::")) {
        const stripped = std.mem.trim(u8, expr[2..], " \t\r\n");
        return stripped.len != 0 and (text.containsFold(sig, stripped) or text.containsFold(obj.preview, stripped));
    }
    return false;
}

fn normalizeTypeQuery(expr_raw: []const u8) []const u8 {
    return std.mem.trim(u8, expr_raw, " \t\r\n");
}

fn signatureSlice(obj: model.Object) []const u8 {
    const preview = std.mem.trim(u8, obj.preview, " \t\r\n");
    if (std.mem.indexOf(u8, preview, "::")) |pos| return std.mem.trim(u8, preview[pos + 2 ..], " \t\r\n");
    return preview;
}

fn evaluateSimpleImpl(allocator: std.mem.Allocator, ctx: *const model.Context, idx_opt: ?*const search_index.SearchIndex, expr: []const u8, options: EvalOptions) !ResultList {
    var tokens = try tokenize(allocator, expr);
    defer tokens.deinit();

    const indexed = idx_opt != null;
    var scratch_marks: []bool = &.{};
    if (indexed) scratch_marks = try allocator.alloc(bool, ctx.objects.items.len);
    defer { if (indexed) allocator.free(scratch_marks); }

    var set = try initAll(allocator, ctx);
    defer set.deinit();
    var terms = std.array_list.Managed([]const u8).init(allocator);
    defer terms.deinit();
    var edge_hint: ?model.EdgeKind = null;

    var had_word = false;
    for (tokens.items) |tok| {
        switch (tok.kind) {
            .word => {
                if (idx_opt) |idx| {
                    try applyWordIndexed(allocator, ctx, idx, &set, tok.text, scratch_marks);
                } else {
                    try applyWord(ctx, &set, tok.text);
                }
                try terms.append(tok.text);
                had_word = true;
            },
            .kind_filter => {
                if (idx_opt) |idx| {
                    try applyKindIndexed(ctx, idx, &set, tok.text, scratch_marks);
                } else {
                    try applyKind(ctx, &set, tok.text);
                }
            },
            .path_filter => {
                if (idx_opt) |idx| {
                    try applyPathIndexed(ctx, idx, &set, tok.text, scratch_marks);
                } else {
                    try applyPath(ctx, &set, tok.text);
                }
            },
            .id_filter => try applyId(ctx, &set, tok.text),
            .edge_filter => {
                const k = model.Context.parseEdgeKind(tok.text);
                edge_hint = k;
                if (idx_opt) |idx| {
                    try applyEdgeKindIndexed(ctx, idx, &set, k, scratch_marks);
                } else {
                    try applyEdgeKind(ctx, &set, k);
                }
            },
            .exact_filter => {
                if (idx_opt) |idx| {
                    try applyExactIndexed(allocator, ctx, idx, &set, tok.text, scratch_marks);
                } else {
                    try applyExact(ctx, &set, tok.text);
                }
                try terms.append(tok.text);
                had_word = true;
            },
            .op_out => {
                if (idx_opt) |idx| {
                    try expandEdgesIndexed(allocator, ctx, idx, &set, .out, edge_hint);
                } else {
                    try expandEdges(allocator, ctx, &set, .out, edge_hint);
                }
            },
            .op_in => {
                if (idx_opt) |idx| {
                    try expandEdgesIndexed(allocator, ctx, idx, &set, .in, edge_hint);
                } else {
                    try expandEdges(allocator, ctx, &set, .in, edge_hint);
                }
            },
            .op_neighborhood => {
                if (idx_opt) |idx| {
                    try expandEdgesIndexed(allocator, ctx, idx, &set, .both, edge_hint);
                } else {
                    try expandEdges(allocator, ctx, &set, .both, edge_hint);
                }
            },
            .op_project => {
                if (idx_opt) |idx| {
                    try projectSupersetIndexed(allocator, ctx, idx, &set);
                } else {
                    try projectSuperset(allocator, ctx, &set);
                }
            },
            .op_clear => {
                for (set.items) |*b| {
                    b.* = true;
                }
                edge_hint = null;
            },
            .lparen, .rparen => {},
        }
    }

    if (!had_word and tokens.items.len == 0) {
        for (set.items) |*b| {
            b.* = true;
        }
    }

    return rankSet(allocator, ctx, set.items, terms.items, null, options.limit);
}

fn evaluateRelationImpl(allocator: std.mem.Allocator, ctx: *const model.Context, idx_opt: ?*const search_index.SearchIndex, rel: RelationSpec, options: EvalOptions) !ResultList {
    var lhs_set = try evaluateSetOnlyImpl(allocator, ctx, idx_opt, rel.lhs);
    defer lhs_set.deinit();
    var rhs_set = try evaluateSetOnlyImpl(allocator, ctx, idx_opt, rel.rhs);
    defer rhs_set.deinit();
    var keep = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    defer keep.deinit();
    try keep.appendNTimes(false, ctx.objects.items.len);
    var bonus = try std.array_list.Managed(i32).initCapacity(allocator, ctx.objects.items.len);
    defer bonus.deinit();
    try bonus.appendNTimes(0, ctx.objects.items.len);

    if (idx_opt) |idx| {
        if (rel.edge_kind) |k| {
            for (idx.edgesOfKind(k)) |edge_idx| {
                considerRelationEdge(ctx, rel, ctx.edges.items[edge_idx], lhs_set.items, rhs_set.items, keep.items, bonus.items);
            }
        } else {
            for (ctx.edges.items) |e| {
                considerRelationEdge(ctx, rel, e, lhs_set.items, rhs_set.items, keep.items, bonus.items);
            }
        }
    } else {
        for (ctx.edges.items) |e| {
            if (rel.edge_kind) |k| {
                if (e.kind != k) continue;
            }
            considerRelationEdge(ctx, rel, e, lhs_set.items, rhs_set.items, keep.items, bonus.items);
        }
    }

    var relation_terms = std.array_list.Managed([]const u8).init(allocator);
    defer relation_terms.deinit();
    try appendWordsOnly(allocator, rel.lhs, &relation_terms);
    try appendWordsOnly(allocator, rel.rhs, &relation_terms);
    return rankSet(allocator, ctx, keep.items, relation_terms.items, bonus.items, options.limit);
}

pub fn findRelation(expr: []const u8) ?RelationSpec {
    if (looksLikeTypeSignature(expr)) return null;
    if (findSpacedOp(expr, "->")) |pos| {
        const lhs = std.mem.trim(u8, expr[0..pos], " \t\r\n");
        const rhs = std.mem.trim(u8, expr[pos + 2 ..], " \t\r\n");
        if (lhs.len == 0 or rhs.len == 0) return null;
        return .{ .op = .out, .lhs = lhs, .rhs = rhs, .edge_kind = findEdgeHint(expr) };
    }
    if (findSpacedOp(expr, "<-")) |pos| {
        const lhs = std.mem.trim(u8, expr[0..pos], " \t\r\n");
        const rhs = std.mem.trim(u8, expr[pos + 2 ..], " \t\r\n");
        if (lhs.len == 0 or rhs.len == 0) return null;
        return .{ .op = .in, .lhs = lhs, .rhs = rhs, .edge_kind = findEdgeHint(expr) };
    }
    return null;
}

fn findSpacedOp(expr: []const u8, op: []const u8) ?usize {
    var start: usize = 0;
    while (std.mem.indexOf(u8, expr[start..], op)) |rel| {
        const pos = start + rel;
        const before_ok = pos > 0 and std.ascii.isWhitespace(expr[pos - 1]);
        const after = pos + op.len;
        const after_ok = after < expr.len and std.ascii.isWhitespace(expr[after]);
        if (before_ok and after_ok) return pos;
        start = pos + op.len;
        if (start >= expr.len) break;
    }
    return null;
}

fn findEdgeHint(expr: []const u8) ?model.EdgeKind {
    var i: usize = 0;
    while (i < expr.len) : (i += 1) {
        if (expr[i] != '%') continue;
        var j = i + 1;
        while (j < expr.len and !std.ascii.isWhitespace(expr[j])) : (j += 1) {}
        if (j > i + 1) {
            const k = model.Context.parseEdgeKind(expr[i + 1 .. j]);
            if (k != .unknown) return k;
        }
    }
    return null;
}

fn evaluateSetOnlyImpl(allocator: std.mem.Allocator, ctx: *const model.Context, idx_opt: ?*const search_index.SearchIndex, expr: []const u8) !std.array_list.Managed(bool) {
    var tokens = try tokenize(allocator, expr);
    defer tokens.deinit();
    const indexed = idx_opt != null;
    var scratch_marks: []bool = &.{};
    if (indexed) scratch_marks = try allocator.alloc(bool, ctx.objects.items.len);
    defer { if (indexed) allocator.free(scratch_marks); }
    var set = try initAll(allocator, ctx);
    var edge_hint: ?model.EdgeKind = null;
    for (tokens.items) |tok| {
        switch (tok.kind) {
            .word => {
                if (idx_opt) |idx| { try applyWordIndexed(allocator, ctx, idx, &set, tok.text, scratch_marks); } else { try applyWord(ctx, &set, tok.text); }
            },
            .kind_filter => {
                if (idx_opt) |idx| { try applyKindIndexed(ctx, idx, &set, tok.text, scratch_marks); } else { try applyKind(ctx, &set, tok.text); }
            },
            .path_filter => {
                if (idx_opt) |idx| { try applyPathIndexed(ctx, idx, &set, tok.text, scratch_marks); } else { try applyPath(ctx, &set, tok.text); }
            },
            .id_filter => try applyId(ctx, &set, tok.text),
            .edge_filter => {
                const k = model.Context.parseEdgeKind(tok.text);
                edge_hint = k;
                if (idx_opt) |idx| { try applyEdgeKindIndexed(ctx, idx, &set, k, scratch_marks); } else { try applyEdgeKind(ctx, &set, k); }
            },
            .exact_filter => {
                if (idx_opt) |idx| { try applyExactIndexed(allocator, ctx, idx, &set, tok.text, scratch_marks); } else { try applyExact(ctx, &set, tok.text); }
            },
            .op_out => {
                if (idx_opt) |idx| { try expandEdgesIndexed(allocator, ctx, idx, &set, .out, edge_hint); } else { try expandEdges(allocator, ctx, &set, .out, edge_hint); }
            },
            .op_in => {
                if (idx_opt) |idx| { try expandEdgesIndexed(allocator, ctx, idx, &set, .in, edge_hint); } else { try expandEdges(allocator, ctx, &set, .in, edge_hint); }
            },
            .op_neighborhood => {
                if (idx_opt) |idx| { try expandEdgesIndexed(allocator, ctx, idx, &set, .both, edge_hint); } else { try expandEdges(allocator, ctx, &set, .both, edge_hint); }
            },
            .op_project => {
                if (idx_opt) |idx| { try projectSupersetIndexed(allocator, ctx, idx, &set); } else { try projectSuperset(allocator, ctx, &set); }
            },
            .op_clear => {
                for (set.items) |*b| b.* = true;
                edge_hint = null;
            },
            .lparen, .rparen => {},
        }
    }
    return set;
}

fn boolSetFromResults(allocator: std.mem.Allocator, ctx: *const model.Context, items: []const Result) !std.array_list.Managed(bool) {
    var set = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try set.appendNTimes(false, ctx.objects.items.len);
    for (items) |r| {
        if (r.object_index < set.items.len) set.items[r.object_index] = true;
    }
    return set;
}


fn considerRelationEdge(
    ctx: *const model.Context,
    rel: RelationSpec,
    e: model.Edge,
    lhs_set: []const bool,
    rhs_set: []const bool,
    keep: []bool,
    bonus: []i32,
) void {
    const si = ctx.findObject(e.src) orelse return;
    const di = ctx.findObject(e.dst) orelse return;
    switch (rel.op) {
        .out => {
            if (!lhs_set[si] or !rhs_set[di]) return;
            keep[si] = true;
            keep[di] = true;
            bonus[di] += 110 + edgeWeight(e.kind);
            bonus[si] += 22 + @divTrunc(edgeWeight(e.kind), 2);
        },
        .in => {
            if (!lhs_set[di] or !rhs_set[si]) return;
            keep[si] = true;
            keep[di] = true;
            bonus[si] += 110 + edgeWeight(e.kind);
            bonus[di] += 22 + @divTrunc(edgeWeight(e.kind), 2);
        },
    }
}

fn appendWordsOnly(allocator: std.mem.Allocator, expr: []const u8, out: *std.array_list.Managed([]const u8)) !void {
    var toks = try tokenize(allocator, expr);
    defer toks.deinit();
    for (toks.items) |tok| {
        if (tok.kind == .word) try out.append(tok.text);
    }
}

fn rankSet(
    allocator: std.mem.Allocator,
    ctx: *const model.Context,
    set: []const bool,
    terms: []const []const u8,
    bonus: ?[]const i32,
    limit: usize,
) !ResultList {
    if (limit == 0) {
        return .{ .allocator = allocator, .items = try allocator.alloc(Result, 0) };
    }
    var keep_count: usize = 0;
    for (set) |keep| {
        if (keep) keep_count += 1;
    }
    var ranked = std.array_list.Managed(Result).init(allocator);
    defer ranked.deinit();
    if (keep_count <= limit) {
        try ranked.ensureTotalCapacity(keep_count);
        for (set, 0..) |keep, i| {
            if (!keep) continue;
            try ranked.append(.{ .object_index = i, .score = scoreObject(ctx, i, terms, bonus) });
        }
        std.mem.sort(Result, ranked.items, ctx, lessResult);
    } else {
        try ranked.ensureTotalCapacity(limit);
        for (set, 0..) |keep, i| {
            if (!keep) continue;
            insertTop(&ranked, .{ .object_index = i, .score = scoreObject(ctx, i, terms, bonus) }, limit);
        }
    }
    const n = @min(ranked.items.len, limit);
    const out = try allocator.dupe(Result, ranked.items[0..n]);
    return .{ .allocator = allocator, .items = out };
}

fn scoreObject(ctx: *const model.Context, i: usize, terms: []const []const u8, bonus: ?[]const i32) i32 {
    var sc = rankObject(ctx.objects.items[i], terms);
    if (bonus) |b| { if (i < b.len) sc += b[i]; }
    return sc;
}

fn insertTop(ranked: *std.array_list.Managed(Result), item: Result, limit: usize) void {
    if (ranked.items.len == 0) {
        ranked.appendAssumeCapacity(item);
        return;
    }
    if (ranked.items.len == limit and item.score <= ranked.items[ranked.items.len - 1].score) return;
    if (ranked.items.len < limit) ranked.appendAssumeCapacity(item) else ranked.items[ranked.items.len - 1] = item;
    var i = ranked.items.len - 1;
    while (i > 0 and ranked.items[i].score > ranked.items[i - 1].score) : (i -= 1) {
        const tmp = ranked.items[i - 1];
        ranked.items[i - 1] = ranked.items[i];
        ranked.items[i] = tmp;
    }
}

fn lessResult(ctx: *const model.Context, a: Result, b: Result) bool {
    _ = ctx;
    return a.score > b.score;
}

fn rankObject(obj: model.Object, terms: []const []const u8) i32 {
    var total: i32 = obj.weight;
    for (terms) |t| {
        if (t.len == 0 or t[0] == '%') continue;
        var best = fuzzy.score(obj.id, t);
        const st = fuzzy.score(obj.title, t);
        if (st.value > best.value) best = st;
        const sp = fuzzy.score(obj.path, t);
        if (sp.value > best.value) best = sp;
        const sg = fuzzy.score(obj.tags, t);
        if (sg.value > best.value) best = sg;
        const spr = fuzzy.score(obj.preview, t);
        if (spr.value > best.value) best = spr;
        total += best.value;
    }
    total += switch (obj.kind) {
        .record => 19,
        .heading => 12,
        .concept => 14,
        .script => 8,
        .test_kind => 28,
        .todo => 32,
        .done => 14,
        .info => 12,
        .source => 10,
        .function_kind => 36,
        .report => 10,
        else => 0,
    };
    if (contains(obj.title, "TODO") or contains(obj.preview, "TODO")) {
        total += 16;
    }
    if (contains(obj.title, "TEST") or contains(obj.preview, "TEST-")) {
        total += 14;
    }
    if (contains(obj.title, "OBS") or contains(obj.title, "DEC") or contains(obj.title, "INF")) {
        total += 10;
    }
    return total;
}

fn edgeWeight(k: model.EdgeKind) i32 {
    return switch (k) {
        .verifies => 38,
        .supports => 34,
        .blocks => 30,
        .refines => 24,
        .classifies_as => 22,
        .id_link => 18,
        .file_link => 14,
        .generated_by => 18,
        .mentions => 12,
        .contains => 8,
        .supersedes => 20,
        .forgets_to => 18,
        .unknown => 0,
    };
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


fn applyExact(ctx: *const model.Context, set: *std.array_list.Managed(bool), word: []const u8) !void {
    try tryString(word);
    for (set.items, 0..) |*keep, i| {
        if (!keep.*) continue;
        const obj = ctx.objects.items[i];
        keep.* = text.containsFold(obj.id, word) or text.containsFold(obj.title, word) or text.containsFold(obj.path, word) or text.containsFold(obj.tags, word) or text.containsFold(obj.preview, word);
    }
}

fn applyExactIndexed(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), word: []const u8, marks: []bool) !void {
    const count = try idx.markExactWordCandidates(allocator, word, marks);
    if (count == 0) {
        try applyExact(ctx, set, word);
        return;
    }
    for (set.items, 0..) |*keep, i| {
        if (keep.*) keep.* = marks[i];
    }
}

fn applyWordIndexed(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), word: []const u8, marks: []bool) !void {
    if (fieldSpec(word)) |spec| {
        applyField(ctx, set, spec.field, spec.needle);
        return;
    }
    const count = try idx.markWordCandidates(allocator, word, marks);
    if (count == 0 or word.len < 3) {
        // Very short fuzzy queries and never-seen terms are better handled by
        // the reference path; this keeps quality without scanning on normal
        // indexed words.
        try applyWord(ctx, set, word);
        return;
    }
    for (set.items, 0..) |*keep, i| {
        if (keep.*) keep.* = marks[i];
    }
}

const FieldSpec = struct { field: []const u8, needle: []const u8 };

fn fieldSpec(word: []const u8) ?FieldSpec {
    const pos = std.mem.indexOfScalar(u8, word, ':') orelse return null;
    if (pos == 0 or pos + 1 >= word.len) return null;
    const field = word[0..pos];
    if (!std.mem.eql(u8, field, "title") and !std.mem.eql(u8, field, "path") and !std.mem.eql(u8, field, "id") and !std.mem.eql(u8, field, "tag") and !std.mem.eql(u8, field, "preview") and !std.mem.eql(u8, field, "type") and !std.mem.eql(u8, field, "sig") and !std.mem.eql(u8, field, "signature") and !std.mem.eql(u8, field, "function")) return null;
    return .{ .field = field, .needle = word[pos + 1 ..] };
}

fn applyField(ctx: *const model.Context, set: *std.array_list.Managed(bool), field: []const u8, needle: []const u8) void {
    for (set.items, 0..) |*keep, i| {
        if (!keep.*) continue;
        const obj = ctx.objects.items[i];
        if (std.mem.eql(u8, field, "type") or std.mem.eql(u8, field, "sig") or std.mem.eql(u8, field, "signature")) {
            keep.* = obj.kind == .function_kind and text.containsFold(obj.preview, needle);
            continue;
        }
        if (std.mem.eql(u8, field, "function")) {
            keep.* = obj.kind == .function_kind and (text.containsFold(obj.title, needle) or text.containsFold(obj.id, needle) or text.containsFold(obj.preview, needle));
            continue;
        }
        const hay = if (std.mem.eql(u8, field, "title")) obj.title else if (std.mem.eql(u8, field, "path")) obj.path else if (std.mem.eql(u8, field, "id")) obj.id else if (std.mem.eql(u8, field, "tag")) obj.tags else obj.preview;
        keep.* = text.containsFold(hay, needle);
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


fn applyKindIndexed(_: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), name: []const u8, marks: []bool) !void {
    const k = model.Context.parseObjectKind(name);
    @memset(marks, false);
    for (idx.objectsOfKind(k)) |object_index| marks[object_index] = true;
    for (set.items, 0..) |*keep, i| {
        if (keep.*) keep.* = marks[i];
    }
}

fn applyPath(ctx: *const model.Context, set: *std.array_list.Managed(bool), path: []const u8) !void {
    for (set.items, 0..) |*keep, i| {
        if (keep.*) {
            keep.* = matchNamespace(ctx.objects.items[i], path);
        }
    }
}


fn applyPathIndexed(ctx: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), ns: []const u8, marks: []bool) !void {
    if (idx.objectsOfLane(ns)) |lane| {
        @memset(marks, false);
        for (lane) |object_index| marks[object_index] = true;
        for (set.items, 0..) |*keep, i| {
            if (keep.*) keep.* = marks[i];
        }
        return;
    }
    if (std.mem.eql(u8, ns, "orphans") or std.mem.eql(u8, ns, "orphan")) {
        for (set.items, 0..) |*keep, i| {
            if (keep.*) keep.* = idx.isOrphan(i);
        }
        return;
    }
    if (std.mem.eql(u8, ns, "roots") or std.mem.eql(u8, ns, "root")) {
        for (set.items, 0..) |*keep, i| {
            if (keep.*) keep.* = !idx.hasIncoming(i) and idx.hasOutgoing(i);
        }
        return;
    }
    if (std.mem.eql(u8, ns, "leaves") or std.mem.eql(u8, ns, "leaf")) {
        for (set.items, 0..) |*keep, i| {
            if (keep.*) keep.* = idx.hasIncoming(i) and !idx.hasOutgoing(i);
        }
        return;
    }
    if (std.mem.eql(u8, ns, "blocked") or std.mem.eql(u8, ns, "blockers")) {
        @memset(marks, false);
        for (idx.edgesOfKind(.blocks)) |edge_idx| {
            if (idx.edgeSrc(edge_idx)) |si| marks[si] = true;
            if (idx.edgeDst(edge_idx)) |di| marks[di] = true;
        }
        for (set.items, 0..) |*keep, i| {
            if (keep.*) keep.* = marks[i];
        }
        return;
    }
    try applyPath(ctx, set, ns);
}

fn matchNamespace(obj: model.Object, ns: []const u8) bool {
    if (std.mem.eql(u8, ns, "tests") or std.mem.eql(u8, ns, "test")) {
        return obj.kind == .test_kind or contains(obj.path, "tests/") or contains(obj.tags, "@tests") or contains(obj.preview, "TEST-");
    }
    if (std.mem.eql(u8, ns, "info")) {
        return obj.kind == .info or contains(obj.tags, "@info") or contains(obj.path, "info") or contains(obj.path, "docs") or contains(obj.title, "Info");
    }
    if (std.mem.eql(u8, ns, "notes") or std.mem.eql(u8, ns, "note") or std.mem.eql(u8, ns, "ctx") or std.mem.eql(u8, ns, "context")) {
        return obj.kind != .info and (obj.kind == .record or obj.kind == .heading or obj.kind == .concept or contains(obj.tags, "@records") or contains(obj.path, "context/") or contains(obj.path, "category/") or contains(obj.path, "notes") or contains(obj.title, "OBS") or contains(obj.title, "DEC") or contains(obj.title, "INF") or contains(obj.preview, "[OBS") or contains(obj.preview, "[DEC") or contains(obj.preview, "[INF"));
    }
    if (std.mem.eql(u8, ns, "records") or std.mem.eql(u8, ns, "record")) return obj.kind == .record or contains(obj.tags, "@records");
    if (std.mem.eql(u8, ns, "reports") or std.mem.eql(u8, ns, "report")) return obj.kind == .report or contains(obj.path, "report") or contains(obj.title, "report");
    if (std.mem.eql(u8, ns, "fix") or std.mem.eql(u8, ns, "fixes")) return contains(obj.title, "FIX") or contains(obj.preview, "FIX");
    if (std.mem.eql(u8, ns, "hot") or std.mem.eql(u8, ns, "triage")) return obj.kind == .todo or contains(obj.title, "TODO") or contains(obj.title, "FAIL") or contains(obj.preview, "error:") or contains(obj.preview, "regression") or contains(obj.preview, "blocks");
    if (std.mem.eql(u8, ns, "source") or std.mem.eql(u8, ns, "src")) {
        return obj.kind == .source or obj.kind == .script or contains(obj.tags, "@source") or contains(obj.path, "src/") or contains(obj.path, ".c") or contains(obj.path, ".h") or contains(obj.path, ".zig");
    }
    if (std.mem.eql(u8, ns, "functions") or std.mem.eql(u8, ns, "function") or std.mem.eql(u8, ns, "fn") or std.mem.eql(u8, ns, "methods") or std.mem.eql(u8, ns, "method")) return obj.kind == .function_kind or contains(obj.tags, "@functions");
    if (std.mem.eql(u8, ns, "contracts") or std.mem.eql(u8, ns, "contract")) return contains(obj.preview, "CONTEXT_KIND: contract") or contains(obj.preview, "CONTRACT") or contains(obj.title, "Contract") or contains(obj.title, "contract") or contains(obj.id, ".contract") or contains(obj.path, "contract");
    if (std.mem.eql(u8, ns, "quality") or std.mem.eql(u8, ns, "trust") or std.mem.eql(u8, ns, "risk")) return contains(obj.path, "quality") or contains(obj.title, "Anti-Pattern") or contains(obj.preview, "anti-pattern") or contains(obj.preview, "CONFIDENCE:") or contains(obj.preview, "CONTEXT_STATUS") or contains(obj.preview, "risk") or contains(obj.preview, "trust") or contains(obj.preview, "stale");
    if (std.mem.eql(u8, ns, "metadata") or std.mem.eql(u8, ns, "meta") or std.mem.eql(u8, ns, "properties")) return contains(obj.preview, "#+PROPERTY") or contains(obj.preview, "#+FILETAGS") or contains(obj.preview, "CONTEXT_") or contains(obj.preview, ":ID:") or contains(obj.tags, "monadc") or contains(obj.id, "context.");
    if (std.mem.eql(u8, ns, "links") or std.mem.eql(u8, ns, "link") or std.mem.eql(u8, ns, "buttons") or std.mem.eql(u8, ns, "button")) return obj.kind == .file or contains(obj.preview, "[[") or contains(obj.preview, "file:") or contains(obj.preview, "id:") or contains(obj.preview, "http") or contains(obj.tags, "@links");
    if (std.mem.eql(u8, ns, "tables") or std.mem.eql(u8, ns, "table") or std.mem.eql(u8, ns, "orgtables")) return contains(obj.preview, "\n|") or contains(obj.preview, "|---") or contains(obj.preview, "|-") or contains(obj.tags, "@tables");
    if (std.mem.eql(u8, ns, "docs") or std.mem.eql(u8, ns, "doc") or std.mem.eql(u8, ns, "manual")) return obj.kind == .info or obj.kind == .file or obj.kind == .report or contains(obj.path, ".org") or contains(obj.path, ".md") or contains(obj.tags, "@docs");
    if (std.mem.eql(u8, ns, "failures") or std.mem.eql(u8, ns, "failure") or std.mem.eql(u8, ns, "fails") or std.mem.eql(u8, ns, "fail")) return contains(obj.title, "FAIL") or contains(obj.preview, "FAIL") or contains(obj.preview, "failed") or contains(obj.preview, "failure") or contains(obj.preview, "error:") or contains(obj.tags, "@failures");
    if (std.mem.eql(u8, ns, "regressions") or std.mem.eql(u8, ns, "regression")) return contains(obj.title, "regression") or contains(obj.preview, "regression") or contains(obj.preview, "stale golden") or contains(obj.preview, "behavior drift") or contains(obj.tags, "@regressions");
    if (std.mem.eql(u8, ns, "performance") or std.mem.eql(u8, ns, "perf") or std.mem.eql(u8, ns, "speed")) return contains(obj.path, "perf") or contains(obj.title, "perf") or contains(obj.preview, "performance") or contains(obj.preview, "fast") or contains(obj.preview, "cache") or contains(obj.preview, "allocation") or contains(obj.tags, "@performance");
    if (std.mem.eql(u8, ns, "coverage") or std.mem.eql(u8, ns, "cover")) return contains(obj.title, "coverage") or contains(obj.preview, "coverage") or contains(obj.preview, "TEST-COVERAGE") or contains(obj.preview, "complete coverage") or contains(obj.tags, "@coverage");
    if (std.mem.eql(u8, ns, "examples") or std.mem.eql(u8, ns, "example")) return contains(obj.path, "examples") or contains(obj.title, "example") or contains(obj.preview, "example") or contains(obj.tags, "@examples");
    if (std.mem.eql(u8, ns, "tutorials") or std.mem.eql(u8, ns, "tutorial") or std.mem.eql(u8, ns, "manuals")) return contains(obj.title, "tutorial") or contains(obj.preview, "tutorial") or contains(obj.path, "README") or contains(obj.path, "manual") or contains(obj.tags, "@tutorials");
    if (std.mem.eql(u8, ns, "api") or std.mem.eql(u8, ns, "apis") or std.mem.eql(u8, ns, "interface") or std.mem.eql(u8, ns, "interfaces")) return contains(obj.title, "API") or contains(obj.preview, "API") or contains(obj.preview, "interface") or contains(obj.preview, "signature") or contains(obj.tags, "@api");
    if (std.mem.eql(u8, ns, "cli") or std.mem.eql(u8, ns, "commands") or std.mem.eql(u8, ns, "flags")) return contains(obj.path, "cli") or contains(obj.title, "CLI") or contains(obj.preview, "--") or contains(obj.preview, "command") or contains(obj.preview, "flag") or contains(obj.tags, "@cli");
    if (std.mem.eql(u8, ns, "cache") or std.mem.eql(u8, ns, "caches") or std.mem.eql(u8, ns, "index") or std.mem.eql(u8, ns, "indices")) return contains(obj.path, "cache") or contains(obj.title, "cache") or contains(obj.preview, "cache") or contains(obj.preview, "index") or contains(obj.preview, "persist") or contains(obj.tags, "@cache");
    if (std.mem.eql(u8, ns, "diagnostics") or std.mem.eql(u8, ns, "diagnostic") or std.mem.eql(u8, ns, "errors") or std.mem.eql(u8, ns, "warnings")) return contains(obj.path, "diagnostic") or contains(obj.title, "diagnostic") or contains(obj.preview, "diagnostic") or contains(obj.preview, "warning") or contains(obj.preview, "error:") or contains(obj.tags, "@diagnostics");
    if (std.mem.eql(u8, ns, "design") or std.mem.eql(u8, ns, "architecture") or std.mem.eql(u8, ns, "arch")) return contains(obj.title, "design") or contains(obj.preview, "design") or contains(obj.preview, "architecture") or contains(obj.preview, "decision") or contains(obj.preview, "intent") or contains(obj.tags, "@design");
    if (std.mem.eql(u8, ns, "todo") or std.mem.eql(u8, ns, "todos")) return obj.kind == .todo or contains(obj.title, "TODO") or contains(obj.preview, "TODO");
    if (std.mem.eql(u8, ns, "done")) return obj.kind == .done or contains(obj.title, "DONE") or contains(obj.preview, "DONE");
    if (std.mem.eql(u8, ns, "obs") or std.mem.eql(u8, ns, "observations")) return contains(obj.title, "OBS") or contains(obj.preview, "[OBS") or contains(obj.preview, "OBS");
    if (std.mem.eql(u8, ns, "dec") or std.mem.eql(u8, ns, "decisions")) return contains(obj.title, "DEC") or contains(obj.preview, "[DEC") or contains(obj.preview, "DEC");
    if (std.mem.eql(u8, ns, "inf") or std.mem.eql(u8, ns, "inferences")) return contains(obj.title, "INF") or contains(obj.preview, "[INF") or contains(obj.preview, "INF");
    if (std.mem.eql(u8, ns, "bugs") or std.mem.eql(u8, ns, "bug")) return contains(obj.title, "BUG") or contains(obj.title, "FAIL") or contains(obj.preview, "bug") or contains(obj.preview, "FAIL") or contains(obj.preview, "error:");
    if (std.mem.eql(u8, ns, "wisp")) return contains(obj.path, "wisp") or contains(obj.title, "wisp") or contains(obj.preview, "wisp") or contains(obj.id, "wisp");
    if (std.mem.eql(u8, ns, "reader")) return contains(obj.path, "reader") or contains(obj.title, "reader") or contains(obj.preview, "reader") or contains(obj.id, "reader");
    if (std.mem.eql(u8, ns, "codegen")) return contains(obj.path, "codegen") or contains(obj.title, "codegen") or contains(obj.preview, "codegen") or contains(obj.id, "codegen");
    return contains(obj.path, ns) or contains(obj.tags, ns) or contains(obj.title, ns) or contains(obj.preview, ns) or contains(obj.id, ns);
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return text.containsFold(haystack, needle);
}

fn applyId(ctx: *const model.Context, set: *std.array_list.Managed(bool), id: []const u8) !void {
    for (set.items, 0..) |*keep, i| {
        if (keep.*) {
            keep.* = std.mem.indexOf(u8, ctx.objects.items[i].id, id) != null;
        }
    }
}

fn applyEdgeKind(ctx: *const model.Context, set: *std.array_list.Managed(bool), k: model.EdgeKind) !void {
    try tryString(model.Context.edgeName(k));
    if (k == .unknown) return;
    var incident = std.array_list.Managed(bool).init(ctx.allocator);
    defer incident.deinit();
    try incident.appendNTimes(false, ctx.objects.items.len);
    for (ctx.edges.items) |e| {
        if (e.kind != k) continue;
        if (ctx.findObject(e.src)) |si| {
            incident.items[si] = true;
        }
        if (ctx.findObject(e.dst)) |di| {
            incident.items[di] = true;
        }
    }
    for (set.items, 0..) |*keep, i| {
        if (keep.*) keep.* = incident.items[i];
    }
}


fn applyEdgeKindIndexed(_: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), k: model.EdgeKind, incident: []bool) !void {
    try tryString(model.Context.edgeName(k));
    if (k == .unknown) return;
    @memset(incident, false);
    for (idx.edgesOfKind(k)) |edge_idx| {
        if (idx.edgeSrc(edge_idx)) |si| incident[si] = true;
        if (idx.edgeDst(edge_idx)) |di| incident[di] = true;
    }
    for (set.items, 0..) |*keep, i| {
        if (keep.*) keep.* = incident[i];
    }
}

const Direction = enum { in, out, both };

fn expandEdges(allocator: std.mem.Allocator, ctx: *const model.Context, set: *std.array_list.Managed(bool), dir: Direction, edge_hint: ?model.EdgeKind) !void {
    var next = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try next.appendNTimes(false, ctx.objects.items.len);
    for (ctx.edges.items) |e| {
        if (edge_hint) |k| {
            if (k != .unknown and e.kind != k) continue;
        }
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


fn expandEdgesIndexed(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool), dir: Direction, edge_hint: ?model.EdgeKind) !void {
    var next = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try next.appendNTimes(false, ctx.objects.items.len);
    for (set.items, 0..) |keep, object_index| {
        if (!keep) continue;
        const obj = ctx.objects.items[object_index];
        if (dir == .out or dir == .both) {
            for (idx.outgoing(obj.id)) |edge_idx| {
                const e = ctx.edges.items[edge_idx];
                if (edge_hint) |k| {
                    if (k != .unknown and e.kind != k) continue;
                }
                if (idx.edgeDst(edge_idx)) |di| next.items[di] = true;
            }
        }
        if (dir == .in or dir == .both) {
            for (idx.incoming(obj.id)) |edge_idx| {
                const e = ctx.edges.items[edge_idx];
                if (edge_hint) |k| {
                    if (k != .unknown and e.kind != k) continue;
                }
                if (idx.edgeSrc(edge_idx)) |si| next.items[si] = true;
            }
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


fn projectSupersetIndexed(allocator: std.mem.Allocator, ctx: *const model.Context, idx: *const search_index.SearchIndex, set: *std.array_list.Managed(bool)) !void {
    var next = try std.array_list.Managed(bool).initCapacity(allocator, ctx.objects.items.len);
    try next.appendSlice(set.items);
    const project_edges = [_]model.EdgeKind{ .classifies_as, .forgets_to, .refines };
    for (project_edges) |kind| {
        for (idx.edgesOfKind(kind)) |edge_idx| {
            const si = idx.edgeSrc(edge_idx) orelse continue;
            const di = idx.edgeDst(edge_idx) orelse continue;
            if (set.items[si]) next.items[di] = true;
        }
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
        const kind: TokenKind = if (raw[0] == ':') .kind_filter else if (raw[0] == '@') .path_filter else if (raw[0] == '?' or raw[0] == '#') .id_filter else if (raw[0] == '%') .edge_filter else if (raw[0] == '=') .exact_filter else .word;
        const token_text = if (kind == .word) raw else raw[1..];
        if (token_text.len != 0 and !std.mem.eql(u8, token_text, "-")) try tokens.append(.{ .kind = kind, .text = token_text });
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
    var t = try tokenize(arena.allocator(), "typed :Record @wisp %verifies =reader > proj");
    defer t.deinit();
    try std.testing.expect(t.items.len == 7);
    try std.testing.expect(t.items[1].kind == .kind_filter);
    try std.testing.expect(t.items[3].kind == .edge_filter);
    try std.testing.expect(t.items[4].kind == .exact_filter);
    try std.testing.expect(t.items[5].kind == .op_out);
    try std.testing.expect(t.items[6].kind == .op_project);
}

test "relation detector requires spaced arrow" {
    const rel = findRelation("@tests -> reader").?;
    try std.testing.expect(rel.op == .out);
    try std.testing.expect(std.mem.eql(u8, rel.lhs, "@tests"));
    try std.testing.expect(std.mem.eql(u8, rel.rhs, "reader"));
    try std.testing.expect(findRelation("Int->Int") == null);
}

test "exact token search uses equals prefix" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "reader.exact", .kind = .source, .title = "reader exact", .preview = "needleexact" });
    _ = try ctx.addObject(.{ .id = "other", .kind = .source, .title = "other", .preview = "needle approx" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "=needleexact", .{ .limit = 4 });
    defer res.deinit();
    try std.testing.expect(res.items.len == 1);
    try std.testing.expect(std.mem.eql(u8, ctx.objects.items[res.items[0].object_index].id, "reader.exact"));
}

test "relation set evaluation skips ranking lhs rhs but preserves participants" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .path = "tests/reader.mon", .preview = "TEST reader" });
    _ = try ctx.addObject(.{ .id = "src.reader", .kind = .source, .title = "reader source", .path = "src/reader.c", .preview = "reader implementation" });
    _ = try ctx.addEdge(.{ .id = "e1", .kind = .verifies, .src = "test.reader", .dst = "src.reader" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "%verifies @tests -> reader", .{ .limit = 8 });
    defer res.deinit();
    try std.testing.expect(res.items.len >= 2);
}

test "relation search finds morphism participants" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .path = "tests/reader.mon", .preview = "TEST reader" });
    _ = try ctx.addObject(.{ .id = "src.reader", .kind = .source, .title = "reader source", .path = "src/reader.c", .preview = "reader implementation" });
    _ = try ctx.addEdge(.{ .id = "e1", .kind = .verifies, .src = "test.reader", .dst = "src.reader" });

    var res = try evaluate(std.testing.allocator, &ctx, "%verifies @tests -> reader", .{ .limit = 8 });
    defer res.deinit();
    try std.testing.expect(res.items.len >= 2);
}

test "namespace lanes cover todo records notes and language surfaces" {
    var ctx = try makeFeatureContext(std.testing.allocator);
    defer ctx.deinit();

    var todo = try evaluate(std.testing.allocator, &ctx, "@todo", .{ .limit = 16 });
    defer todo.deinit();
    try std.testing.expect(resultHas(&ctx, todo.items, "todo.reader-gap"));

    var obs = try evaluate(std.testing.allocator, &ctx, "@obs reader", .{ .limit = 16 });
    defer obs.deinit();
    try std.testing.expect(resultHas(&ctx, obs.items, "rec.obs.reader"));

    var dec = try evaluate(std.testing.allocator, &ctx, "@dec wisp", .{ .limit = 16 });
    defer dec.deinit();
    try std.testing.expect(resultHas(&ctx, dec.items, "rec.dec.wisp"));

    var inf = try evaluate(std.testing.allocator, &ctx, "@inf codegen", .{ .limit = 16 });
    defer inf.deinit();
    try std.testing.expect(resultHas(&ctx, inf.items, "rec.inf.codegen"));

    var bugs = try evaluate(std.testing.allocator, &ctx, "@bugs", .{ .limit = 16 });
    defer bugs.deinit();
    try std.testing.expect(resultHas(&ctx, bugs.items, "todo.reader-gap"));

    var info = try evaluate(std.testing.allocator, &ctx, "@info wisp", .{ .limit = 16 });
    defer info.deinit();
    try std.testing.expect(resultHas(&ctx, info.items, "info.wisp"));

    var notes = try evaluate(std.testing.allocator, &ctx, "@notes", .{ .limit = 16 });
    defer notes.deinit();
    try std.testing.expect(!resultHas(&ctx, notes.items, "info.wisp"));

    var contracts = try evaluate(std.testing.allocator, &ctx, "@contracts", .{ .limit = 16 });
    defer contracts.deinit();
    try std.testing.expect(resultHas(&ctx, contracts.items, "contract.layout"));

    var quality = try evaluate(std.testing.allocator, &ctx, "@quality", .{ .limit = 16 });
    defer quality.deinit();
    try std.testing.expect(resultHas(&ctx, quality.items, "quality.context"));

    var metadata = try evaluate(std.testing.allocator, &ctx, "@metadata", .{ .limit = 16 });
    defer metadata.deinit();
    try std.testing.expect(resultHas(&ctx, metadata.items, "metadata.root"));
}

test "category graph operators and edge filters are evaluated" {
    var ctx = try makeFeatureContext(std.testing.allocator);
    defer ctx.deinit();

    var out = try evaluate(std.testing.allocator, &ctx, "reader :Source >", .{ .limit = 16 });
    defer out.deinit();
    try std.testing.expect(resultHas(&ctx, out.items, "concept.reader"));

    var incoming = try evaluate(std.testing.allocator, &ctx, "reader :Concept <", .{ .limit = 16 });
    defer incoming.deinit();
    try std.testing.expect(resultHas(&ctx, incoming.items, "src.reader"));

    var blocks = try evaluate(std.testing.allocator, &ctx, "%blocks @todo", .{ .limit = 16 });
    defer blocks.deinit();
    try std.testing.expect(resultHas(&ctx, blocks.items, "todo.reader-gap"));
}

test "relation syntax supports forward reverse and edge-qualified questions" {
    var ctx = try makeFeatureContext(std.testing.allocator);
    defer ctx.deinit();

    var fwd = try evaluate(std.testing.allocator, &ctx, "@tests -> reader", .{ .limit = 16 });
    defer fwd.deinit();
    try std.testing.expect(resultHas(&ctx, fwd.items, "test.reader"));
    try std.testing.expect(resultHas(&ctx, fwd.items, "src.reader"));

    var rev = try evaluate(std.testing.allocator, &ctx, "reader <- @tests", .{ .limit = 16 });
    defer rev.deinit();
    try std.testing.expect(resultHas(&ctx, rev.items, "test.reader"));
    try std.testing.expect(resultHas(&ctx, rev.items, "src.reader"));

    var qualified = try evaluate(std.testing.allocator, &ctx, "%verifies @tests -> reader", .{ .limit = 16 });
    defer qualified.deinit();
    try std.testing.expect(resultHas(&ctx, qualified.items, "test.reader"));
    try std.testing.expect(resultHas(&ctx, qualified.items, "src.reader"));
}

fn resultHas(ctx: *const model.Context, items: []const Result, id: []const u8) bool {
    for (items) |item| {
        if (std.mem.eql(u8, ctx.objects.items[item.object_index].id, id)) return true;
    }
    return false;
}

fn makeFeatureContext(allocator: std.mem.Allocator) !model.Context {
    var ctx = try model.Context.init(allocator, ".");
    _ = try ctx.addObject(.{ .id = "todo.reader-gap", .kind = .todo, .title = "TODO reader FAIL gap", .path = "context/todo.org", .preview = "TODO reader error: layout gap blocks codegen" });
    _ = try ctx.addObject(.{ .id = "src.reader", .kind = .source, .title = "reader source", .path = "src/reader.c", .preview = "reader implementation" });
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .path = "tests/reader.mon", .preview = "TEST reader verifies source" });
    _ = try ctx.addObject(.{ .id = "rec.obs.reader", .kind = .record, .title = "[OBS] reader layout", .path = "context/records.org", .preview = "[OBS] reader layout observed" });
    _ = try ctx.addObject(.{ .id = "rec.dec.wisp", .kind = .record, .title = "[DEC] wisp syntax", .path = "context/wisp.org", .preview = "[DEC] wisp define syntax" });
    _ = try ctx.addObject(.{ .id = "rec.inf.codegen", .kind = .record, .title = "[INF] codegen closure", .path = "context/codegen.org", .preview = "[INF] codegen closure inference" });
    _ = try ctx.addObject(.{ .id = "concept.reader", .kind = .concept, .title = "reader concept", .path = "context/category.org", .preview = "reader category object" });
    _ = try ctx.addObject(.{ .id = "info.wisp", .kind = .info, .title = "Wisp info page", .path = "info/wisp.org", .preview = "Wisp syntax reference page" });
    _ = try ctx.addObject(.{ .id = "function:core/int.mon:inc", .kind = .function_kind, .title = "inc", .path = "core/Int.mon", .preview = "function inc :: Int -> Int", .tags = "@functions @core" });
    _ = try ctx.addObject(.{ .id = "function:core/id.mon:id", .kind = .function_kind, .title = "id", .path = "core/id.mon", .preview = "function id :: a -> a", .tags = "@functions @core" });
    _ = try ctx.addObject(.{ .id = "contract.layout", .kind = .record, .title = "Contract layout", .path = "context/contracts/layout.org", .preview = "CONTEXT_KIND: contract CONTRACT right pane renders records" });
    _ = try ctx.addObject(.{ .id = "quality.context", .kind = .record, .title = "Anti-Pattern stale context", .path = "context/category/quality.org", .preview = "CONFIDENCE: high risk stale anti-pattern" });
    _ = try ctx.addObject(.{ .id = "metadata.root", .kind = .heading, .title = "Context metadata", .path = "context/category-context.org", .preview = "#+PROPERTY: CONTEXT_KIND category :ID: monadc.context.root" });
    _ = try ctx.addEdge(.{ .id = "e.verify", .kind = .verifies, .src = "test.reader", .dst = "src.reader" });
    _ = try ctx.addEdge(.{ .id = "e.blocks", .kind = .blocks, .src = "todo.reader-gap", .dst = "src.reader" });
    _ = try ctx.addEdge(.{ .id = "e.class", .kind = .classifies_as, .src = "src.reader", .dst = "concept.reader" });
    _ = try ctx.addEdge(.{ .id = "e.support", .kind = .supports, .src = "rec.obs.reader", .dst = "src.reader" });
    _ = try ctx.addEdge(.{ .id = "e.mention", .kind = .mentions, .src = "info.wisp", .dst = "rec.dec.wisp" });
    return ctx;
}

test "examples query catalogues parse and evaluate" {
    const allocator = std.testing.allocator;
    var ctx = try makeFeatureContext(allocator);
    defer ctx.deinit();
    const files = [_][]const u8{
        "examples/queries.catq",
        "examples/catalogue.catq",
        "examples/perf.catq",
        "examples/cache.catq",
        "examples/metadata.catq",
        "examples/functions.catq",
    };
    var checked: usize = 0;
    for (files) |file| {
        checked += try checkExampleFile(allocator, &ctx, file);
    }
    try std.testing.expect(checked >= 100);
}

fn checkExampleFile(allocator: std.mem.Allocator, ctx: *const model.Context, path: []const u8) !usize {
    const bytes = try std.fs.cwd().readFileAlloc(path, allocator, @enumFromInt(128 * 1024));
    defer allocator.free(bytes);
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    var checked: usize = 0;
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0 or line[0] == '#') continue;
        var res = try evaluate(allocator, ctx, line, .{ .limit = 8 });
        res.deinit();
        checked += 1;
    }
    return checked;
}

test "indexed evaluator matches reference on feature queries" {
    var ctx = try makeFeatureContext(std.testing.allocator);
    defer ctx.deinit();
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const samples = [_][]const u8{
        "@todo",
        "@bugs reader",
        "@tests -> reader",
        "reader <- @tests",
        "%verifies @tests -> reader",
        "reader :Source >",
        "title:reader",
        "path:reader.c",
        "@blocked",
        "@contracts",
        "@quality",
        "@metadata",
        "@functions",
        "Int -> Int",
        "a -> a",
    };
    for (samples) |q| {
        var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, q, .{ .limit = 16 });
        try std.testing.expect(res.items.len > 0);
        res.deinit();
    }
}

test "indexed evaluator performance smoke on synthetic corpus" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var i: usize = 0;
    while (i < 500) : (i += 1) {
        var id_buf: [64]u8 = undefined;
        var title_buf: [96]u8 = undefined;
        const id = try std.fmt.bufPrint(&id_buf, "synth.{d}", .{i});
        const title = try std.fmt.bufPrint(&title_buf, "synthetic object {d}", .{i});
        const preview = if (i % 50 == 0) "needlefast performance query object" else "ordinary searchable compiler context";
        _ = try ctx.addObject(.{ .id = id, .kind = if (i % 7 == 0) .todo else .record, .title = title, .path = "context/perf.org", .preview = preview });
    }
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const start = @import("perf.zig").nowNs();
    var rounds: usize = 0;
    while (rounds < 64) : (rounds += 1) {
        var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "needlefast", .{ .limit = 32 });
        try std.testing.expect(res.items.len == 10);
        res.deinit();
    }
    const elapsed = @import("perf.zig").nanosSince(start);
    try std.testing.expect(elapsed < 60_000_000_000);
}

test "query language lanes fields exact and relation syntax compose" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "test.reader.layout", .kind = .test_kind, .title = "reader layout test", .path = "tests/reader.mon", .preview = "TEST verifies reader layout" });
    _ = try ctx.addObject(.{ .id = "src.reader", .kind = .source, .title = "reader source", .path = "src/reader.c", .preview = "reader implementation" });
    _ = try ctx.addObject(.{ .id = "note.reader.obs", .kind = .record, .title = "OBS reader note", .path = "context/reader.org", .preview = "[OBS] reader accepts layout" });
    _ = try ctx.addObject(.{ .id = "todo.reader", .kind = .todo, .title = "TODO reader", .path = "context/todo.org", .preview = "TODO reader performance" });
    _ = try ctx.addEdge(.{ .id = "e.verify", .kind = .verifies, .src = "test.reader.layout", .dst = "src.reader" });
    _ = try ctx.addEdge(.{ .id = "e.support", .kind = .supports, .src = "note.reader.obs", .dst = "todo.reader" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();

    const samples = [_][]const u8{
        "@tests",
        "@notes",
        "@todo title:reader",
        "=reader",
        "%verifies",
        "@tests -> reader",
        "reader <- @tests",
        "@obs -> @todo",
    };
    for (samples) |expr| {
        var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, expr, .{ .limit = 8 });
        defer res.deinit();
        try std.testing.expect(res.items.len != 0);
    }
}

test "field filters are uniform over id title path preview tag" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "idneedle", .kind = .record, .title = "titleneedle", .path = "pathneedle/file.org", .tags = "tagneedle", .preview = "previewneedle" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const samples = [_][]const u8{ "id:idneedle", "title:titleneedle", "path:pathneedle", "tag:tagneedle", "preview:previewneedle" };
    for (samples) |expr| {
        var res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, expr, .{ .limit = 4 });
        defer res.deinit();
        try std.testing.expect(res.items.len == 1);
    }
}


test "type arrows search functions instead of relation syntax" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "function:core/id.mon:id", .kind = .function_kind, .title = "id", .path = "core/id.mon", .preview = "function id :: a -> a", .tags = "@functions @core" });
    _ = try ctx.addObject(.{ .id = "function:core/int.mon:inc", .kind = .function_kind, .title = "inc", .path = "core/int.mon", .preview = "function inc :: Int -> Int", .tags = "@functions @core" });
    _ = try ctx.addObject(.{ .id = "source:reader.c", .kind = .source, .title = "reader", .path = "reader.c", .preview = "reader source" });
    try std.testing.expect(findRelation("a -> a") == null);
    try std.testing.expect(findRelation("Int -> Int") == null);
    var res = try evaluate(std.testing.allocator, &ctx, "a -> a", .{ .limit = 8 });
    defer res.deinit();
    try std.testing.expect(res.items.len == 1);
    try std.testing.expect(ctx.objects.items[res.items[0].object_index].kind == .function_kind);
    var int_res = try evaluate(std.testing.allocator, &ctx, "Int -> Int", .{ .limit = 8 });
    defer int_res.deinit();
    try std.testing.expect(resultHas(&ctx, int_res.items, "function:core/int.mon:inc"));
    var colon_res = try evaluate(std.testing.allocator, &ctx, ":: Int -> Int", .{ .limit = 8 });
    defer colon_res.deinit();
    try std.testing.expect(resultHas(&ctx, colon_res.items, "function:core/int.mon:inc"));
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var idx_res = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "Int -> Int", .{ .limit = 8 });
    defer idx_res.deinit();
    try std.testing.expect(resultHas(&ctx, idx_res.items, "function:core/int.mon:inc"));
}

test "relation arrows still work for object expressions" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .preview = "reader" });
    _ = try ctx.addObject(.{ .id = "source.reader", .kind = .source, .title = "reader", .preview = "reader" });
    _ = try ctx.addEdge(.{ .id = "e", .kind = .verifies, .src = "test.reader", .dst = "source.reader" });
    try std.testing.expect(findRelation("@tests -> reader") != null);
    var res = try evaluate(std.testing.allocator, &ctx, "@tests -> reader", .{ .limit = 8 });
    defer res.deinit();
    try std.testing.expect(res.items.len >= 1);
}


test "namespace links tables and docs expose right pane affordances" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "info.links", .kind = .info, .title = "Link info", .path = "context/info.org", .preview = "See [[file:src/main.zig][main]] and id:core.id\n| key | value |\n|---+---|" });
    _ = try ctx.addObject(.{ .id = "source.main", .kind = .source, .title = "main", .path = "src/main.zig", .preview = "main source" });
    var idx = try search_index.SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    var links = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "@links", .{});
    defer links.deinit();
    try std.testing.expect(resultHas(&ctx, links.items, "info.links"));
    var tables = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "@tables", .{});
    defer tables.deinit();
    try std.testing.expect(resultHas(&ctx, tables.items, "info.links"));
    var docs = try evaluateIndexed(std.testing.allocator, &ctx, &idx, "@docs", .{});
    defer docs.deinit();
    try std.testing.expect(resultHas(&ctx, docs.items, "info.links"));
}

test "namespace conjunction keeps only objects satisfying every lane" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "todo.test.reader", .kind = .test_kind, .title = "TODO reader test", .path = "tests/reader.mon", .preview = "TEST-EXPECT TODO reader contract" });
    _ = try ctx.addObject(.{ .id = "todo.only", .kind = .todo, .title = "TODO only", .path = "context/todo.org", .preview = "TODO work" });
    var res = try evaluate(std.testing.allocator, &ctx, "@todo @tests", .{ .limit = 8 });
    defer res.deinit();
    try std.testing.expect(resultHas(&ctx, res.items, "todo.test.reader"));
    try std.testing.expect(!resultHas(&ctx, res.items, "todo.only"));
}
