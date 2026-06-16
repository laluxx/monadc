const std = @import("std");
const model = @import("model.zig");
const text = @import("text.zig");

pub const Posting = struct {
    object_index: usize,
    weight: i32,
};

pub const TermEntry = struct {
    term: []const u8,
    postings: std.array_list.Managed(Posting),
};

pub const InvertedIndex = struct {
    allocator: std.mem.Allocator,
    terms: std.StringHashMap(*TermEntry),

    pub fn init(allocator: std.mem.Allocator) InvertedIndex {
        return .{ .allocator = allocator, .terms = std.StringHashMap(*TermEntry).init(allocator) };
    }

    pub fn deinit(self: *InvertedIndex) void {
        var it = self.terms.iterator();
        while (it.next()) |e| {
            self.allocator.free(e.key_ptr.*);
            e.value_ptr.*.postings.deinit();
            self.allocator.destroy(e.value_ptr.*);
        }
        self.terms.deinit();
    }

    pub fn build(allocator: std.mem.Allocator, ctx: *const model.Context) !InvertedIndex {
        var idx = InvertedIndex.init(allocator);
        var scratch = std.array_list.Managed(u8).init(allocator);
        defer scratch.deinit();
        for (ctx.objects.items, 0..) |obj, i| {
            try idx.addText(&scratch, i, obj.id, 5);
            try idx.addText(&scratch, i, obj.title, 8);
            try idx.addText(&scratch, i, obj.path, 3);
            try idx.addText(&scratch, i, obj.preview, 1);
            try idx.addText(&scratch, i, obj.tags, 3);
            try idx.addText(&scratch, i, model.Context.kindName(obj.kind), 4);
        }
        return idx;
    }

    fn addText(self: *InvertedIndex, scratch: *std.array_list.Managed(u8), object_index: usize, bytes: []const u8, weight: i32) !void {
        var pos: usize = 0;
        while (pos < bytes.len) {
            while (pos < bytes.len and !isTokenByte(bytes[pos])) pos += 1;
            const start = pos;
            while (pos < bytes.len and isTokenByte(bytes[pos])) pos += 1;
            if (pos <= start) continue;
            const raw = bytes[start..pos];
            if (raw.len < 2) continue;
            scratch.clearRetainingCapacity();
            const norm = try text.normalizeToken(scratch, raw);
            try self.addTerm(norm, object_index, weight);
        }
    }

    fn addTerm(self: *InvertedIndex, term: []const u8, object_index: usize, weight: i32) !void {
        if (self.terms.get(term)) |entry| {
            // Deduplicate repeated terms for the same object. A single object can
            // contain the same token in id/title/path/preview; keeping one
            // posting per object makes exact queries and mark passes cheaper.
            if (entry.postings.items.len != 0) {
                const last_i = entry.postings.items.len - 1;
                if (entry.postings.items[last_i].object_index == object_index) {
                    if (weight > entry.postings.items[last_i].weight) entry.postings.items[last_i].weight = weight;
                    return;
                }
            }
            try entry.postings.append(.{ .object_index = object_index, .weight = weight });
            return;
        }
        const owned = try self.allocator.dupe(u8, term);
        const entry = try self.allocator.create(TermEntry);
        entry.* = .{ .term = owned, .postings = std.array_list.Managed(Posting).init(self.allocator) };
        try entry.postings.append(.{ .object_index = object_index, .weight = weight });
        try self.terms.put(owned, entry);
    }

    pub fn lookup(self: *const InvertedIndex, term: []const u8) ?[]const Posting {
        if (self.terms.get(term)) |entry| return entry.postings.items;
        return null;
    }

    pub fn hasExact(self: *const InvertedIndex, term: []const u8) bool {
        return self.terms.contains(term);
    }

    pub fn postingCount(self: *const InvertedIndex) usize {
        var n: usize = 0;
        var it = self.terms.iterator();
        while (it.next()) |e| n += e.value_ptr.*.postings.items.len;
        return n;
    }

    pub fn approxTerms(self: *const InvertedIndex, allocator: std.mem.Allocator, needle: []const u8, limit: usize) !std.array_list.Managed([]const u8) {
        var out = std.array_list.Managed([]const u8).init(allocator);
        var it = self.terms.iterator();
        while (it.next()) |e| {
            if (text.containsFold(e.key_ptr.*, needle)) {
                try out.append(e.key_ptr.*);
                if (out.items.len >= limit) break;
            }
        }
        return out;
    }
};

fn isTokenByte(c: u8) bool {
    return std.ascii.isAlphanumeric(c) or c == '_' or c == '-' or c == '.';
}


pub const KindSlotCount = 13;
pub const EdgeSlotCount = 13;
pub const LaneSlotCount = 19;
pub const MissingObjectIndex = std.math.maxInt(usize);

pub const SearchIndex = struct {
    allocator: std.mem.Allocator,
    text_index: InvertedIndex,
    src_edges: std.StringHashMap(std.array_list.Managed(usize)),
    dst_edges: std.StringHashMap(std.array_list.Managed(usize)),
    kind_objects: [KindSlotCount]std.array_list.Managed(usize),
    edge_kind_edges: [EdgeSlotCount]std.array_list.Managed(usize),
    lane_objects: [LaneSlotCount]std.array_list.Managed(usize),
    out_degree: []usize,
    in_degree: []usize,
    out_kind_counts: []usize,
    in_kind_counts: []usize,
    edge_src_object: []usize,
    edge_dst_object: []usize,
    object_count: usize = 0,
    edge_count: usize = 0,

    pub fn build(allocator: std.mem.Allocator, ctx: *const model.Context) !SearchIndex {
        var idx = SearchIndex{
            .allocator = allocator,
            .text_index = try InvertedIndex.build(allocator, ctx),
            .src_edges = std.StringHashMap(std.array_list.Managed(usize)).init(allocator),
            .dst_edges = std.StringHashMap(std.array_list.Managed(usize)).init(allocator),
            .kind_objects = undefined,
            .edge_kind_edges = undefined,
            .lane_objects = undefined,
            .out_degree = try allocator.alloc(usize, ctx.objects.items.len),
            .in_degree = try allocator.alloc(usize, ctx.objects.items.len),
            .out_kind_counts = try allocator.alloc(usize, ctx.objects.items.len * EdgeSlotCount),
            .in_kind_counts = try allocator.alloc(usize, ctx.objects.items.len * EdgeSlotCount),
            .edge_src_object = try allocator.alloc(usize, ctx.edges.items.len),
            .edge_dst_object = try allocator.alloc(usize, ctx.edges.items.len),
            .object_count = ctx.objects.items.len,
            .edge_count = ctx.edges.items.len,
        };
        @memset(idx.out_degree, 0);
        @memset(idx.in_degree, 0);
        @memset(idx.out_kind_counts, 0);
        @memset(idx.in_kind_counts, 0);
        @memset(idx.edge_src_object, MissingObjectIndex);
        @memset(idx.edge_dst_object, MissingObjectIndex);
        var ki: usize = 0;
        while (ki < KindSlotCount) : (ki += 1) idx.kind_objects[ki] = std.array_list.Managed(usize).init(allocator);
        var ei: usize = 0;
        while (ei < EdgeSlotCount) : (ei += 1) idx.edge_kind_edges[ei] = std.array_list.Managed(usize).init(allocator);
        var li: usize = 0;
        while (li < LaneSlotCount) : (li += 1) idx.lane_objects[li] = std.array_list.Managed(usize).init(allocator);

        for (ctx.objects.items, 0..) |obj, i| {
            try idx.kind_objects[kindIndex(obj.kind)].append(i);
        }
        for (ctx.edges.items, 0..) |edge, i| {
            try idx.edge_kind_edges[edgeIndex(edge.kind)].append(i);
            try idx.appendEdge(&idx.src_edges, edge.src, i);
            try idx.appendEdge(&idx.dst_edges, edge.dst, i);
            const ek = edgeIndex(edge.kind);
            if (ctx.findObject(edge.src)) |si| {
                idx.edge_src_object[i] = si;
                idx.out_degree[si] += 1;
                idx.out_kind_counts[si * EdgeSlotCount + ek] += 1;
            }
            if (ctx.findObject(edge.dst)) |di| {
                idx.edge_dst_object[i] = di;
                idx.in_degree[di] += 1;
                idx.in_kind_counts[di * EdgeSlotCount + ek] += 1;
            }
        }
        try idx.populateLanes(ctx);
        return idx;
    }

    pub fn deinit(self: *SearchIndex) void {
        self.text_index.deinit();
        deinitEdgeMap(self.allocator, &self.src_edges);
        deinitEdgeMap(self.allocator, &self.dst_edges);
        var ki: usize = 0;
        while (ki < KindSlotCount) : (ki += 1) self.kind_objects[ki].deinit();
        var ei: usize = 0;
        while (ei < EdgeSlotCount) : (ei += 1) self.edge_kind_edges[ei].deinit();
        var li: usize = 0;
        while (li < LaneSlotCount) : (li += 1) self.lane_objects[li].deinit();
        self.allocator.free(self.out_degree);
        self.allocator.free(self.in_degree);
        self.allocator.free(self.out_kind_counts);
        self.allocator.free(self.in_kind_counts);
        self.allocator.free(self.edge_src_object);
        self.allocator.free(self.edge_dst_object);
    }

    fn populateLanes(self: *SearchIndex, ctx: *const model.Context) !void {
        for (ctx.objects.items, 0..) |obj, i| {
            var li: usize = 0;
            while (li < LaneSlotCount) : (li += 1) {
                if (matchesLane(obj, li)) try self.lane_objects[li].append(i);
            }
        }
        for (self.edge_kind_edges[edgeIndex(.blocks)].items) |edge_idx| {
            if (self.edgeSrc(edge_idx)) |si| try self.lane_objects[laneIndexKnown("blocked")].append(si);
            if (self.edgeDst(edge_idx)) |di| try self.lane_objects[laneIndexKnown("blocked")].append(di);
        }
        for (ctx.objects.items, 0..) |_, i| {
            if (!self.hasIncoming(i) and self.hasOutgoing(i)) try self.lane_objects[laneIndexKnown("roots")].append(i);
            if (self.hasIncoming(i) and !self.hasOutgoing(i)) try self.lane_objects[laneIndexKnown("leaves")].append(i);
            if (self.isOrphan(i)) try self.lane_objects[laneIndexKnown("orphans")].append(i);
        }
    }

    fn appendEdge(self: *SearchIndex, map: *std.StringHashMap(std.array_list.Managed(usize)), id: []const u8, edge_index: usize) !void {
        if (map.getPtr(id)) |list| {
            try list.append(edge_index);
            return;
        }
        const owned = try self.allocator.dupe(u8, id);
        var list = std.array_list.Managed(usize).init(self.allocator);
        try list.append(edge_index);
        try map.put(owned, list);
    }

    pub fn outgoing(self: *const SearchIndex, id: []const u8) []const usize {
        if (self.src_edges.get(id)) |list| return list.items;
        return &.{};
    }

    pub fn incoming(self: *const SearchIndex, id: []const u8) []const usize {
        if (self.dst_edges.get(id)) |list| return list.items;
        return &.{};
    }

    pub fn objectsOfKind(self: *const SearchIndex, kind: model.ObjectKind) []const usize {
        return self.kind_objects[kindIndex(kind)].items;
    }

    pub fn edgesOfKind(self: *const SearchIndex, kind: model.EdgeKind) []const usize {
        return self.edge_kind_edges[edgeIndex(kind)].items;
    }

    pub fn objectsOfLane(self: *const SearchIndex, ns: []const u8) ?[]const usize {
        if (laneIndex(ns)) |li| return self.lane_objects[li].items;
        return null;
    }

    pub fn edgeSrc(self: *const SearchIndex, edge_index: usize) ?usize {
        if (edge_index >= self.edge_src_object.len) return null;
        const idx = self.edge_src_object[edge_index];
        return if (idx == MissingObjectIndex) null else idx;
    }

    pub fn edgeDst(self: *const SearchIndex, edge_index: usize) ?usize {
        if (edge_index >= self.edge_dst_object.len) return null;
        const idx = self.edge_dst_object[edge_index];
        return if (idx == MissingObjectIndex) null else idx;
    }


    pub fn outgoingKindCount(self: *const SearchIndex, object_index: usize, kind: model.EdgeKind) usize {
        if (object_index >= self.object_count) return 0;
        return self.out_kind_counts[object_index * EdgeSlotCount + edgeIndex(kind)];
    }

    pub fn incomingKindCount(self: *const SearchIndex, object_index: usize, kind: model.EdgeKind) usize {
        if (object_index >= self.object_count) return 0;
        return self.in_kind_counts[object_index * EdgeSlotCount + edgeIndex(kind)];
    }

    pub fn outgoingTotal(self: *const SearchIndex, object_index: usize) usize {
        if (object_index >= self.out_degree.len) return 0;
        return self.out_degree[object_index];
    }

    pub fn incomingTotal(self: *const SearchIndex, object_index: usize) usize {
        if (object_index >= self.in_degree.len) return 0;
        return self.in_degree[object_index];
    }

    pub fn hasOutgoing(self: *const SearchIndex, object_index: usize) bool {
        return object_index < self.out_degree.len and self.out_degree[object_index] != 0;
    }

    pub fn hasIncoming(self: *const SearchIndex, object_index: usize) bool {
        return object_index < self.in_degree.len and self.in_degree[object_index] != 0;
    }

    pub fn isOrphan(self: *const SearchIndex, object_index: usize) bool {
        return !self.hasOutgoing(object_index) and !self.hasIncoming(object_index);
    }

    pub fn markExactWordCandidates(self: *const SearchIndex, allocator: std.mem.Allocator, word: []const u8, marks: []bool) !usize {
        @memset(marks, false);
        if (word.len == 0) return 0;
        var tmp = std.array_list.Managed(u8).init(allocator);
        defer tmp.deinit();
        const norm = try text.normalizeToken(&tmp, word);
        if (norm.len == 0) return 0;
        if (self.text_index.lookup(norm)) |posts| return markPostings(posts, marks);
        return 0;
    }

    pub fn markWordCandidates(self: *const SearchIndex, allocator: std.mem.Allocator, word: []const u8, marks: []bool) !usize {
        const exact = try self.markExactWordCandidates(allocator, word, marks);
        if (exact != 0) return exact;
        var tmp = std.array_list.Managed(u8).init(allocator);
        defer tmp.deinit();
        const norm = try text.normalizeToken(&tmp, word);
        if (norm.len < 3) return 0;
        var approx = try self.text_index.approxTerms(allocator, norm, 96);
        defer approx.deinit();
        var count: usize = 0;
        for (approx.items) |term| {
            if (self.text_index.lookup(term)) |posts| count += markPostings(posts, marks);
        }
        return countMarked(marks, count);
    }

    pub fn candidateCountForWord(self: *const SearchIndex, allocator: std.mem.Allocator, word: []const u8) !usize {
        const marks = try allocator.alloc(bool, self.object_count);
        defer allocator.free(marks);
        return self.markWordCandidates(allocator, word, marks);
    }
};

fn deinitEdgeMap(allocator: std.mem.Allocator, map: *std.StringHashMap(std.array_list.Managed(usize))) void {
    var it = map.iterator();
    while (it.next()) |e| {
        allocator.free(e.key_ptr.*);
        e.value_ptr.*.deinit();
    }
    map.deinit();
}

fn markPostings(posts: []const Posting, marks: []bool) usize {
    var added: usize = 0;
    for (posts) |p| {
        if (p.object_index >= marks.len) continue;
        if (!marks[p.object_index]) added += 1;
        marks[p.object_index] = true;
    }
    return added;
}

fn countMarked(marks: []const bool, approximate: usize) usize {
    _ = approximate;
    var n: usize = 0;
    for (marks) |m| {
        if (m) n += 1;
    }
    return n;
}

fn laneIndexKnown(name: []const u8) usize {
    return laneIndex(name) orelse 0;
}

pub fn laneIndex(ns: []const u8) ?usize {
    if (eql(ns, "todo") or eql(ns, "todos")) return 0;
    if (eql(ns, "hot") or eql(ns, "triage")) return 1;
    if (eql(ns, "notes") or eql(ns, "note") or eql(ns, "ctx") or eql(ns, "context")) return 2;
    if (eql(ns, "tests") or eql(ns, "test")) return 3;
    if (eql(ns, "info")) return 4;
    if (eql(ns, "source") or eql(ns, "src")) return 5;
    if (eql(ns, "functions") or eql(ns, "function") or eql(ns, "fn") or eql(ns, "methods") or eql(ns, "method")) return 18;
    if (eql(ns, "records") or eql(ns, "record")) return 6;
    if (eql(ns, "obs") or eql(ns, "observations")) return 7;
    if (eql(ns, "dec") or eql(ns, "decisions")) return 8;
    if (eql(ns, "inf") or eql(ns, "inferences")) return 9;
    if (eql(ns, "bugs") or eql(ns, "bug")) return 10;
    if (eql(ns, "wisp")) return 11;
    if (eql(ns, "reader")) return 12;
    if (eql(ns, "codegen")) return 13;
    if (eql(ns, "blocked") or eql(ns, "blockers")) return 14;
    if (eql(ns, "roots") or eql(ns, "root")) return 15;
    if (eql(ns, "leaves") or eql(ns, "leaf")) return 16;
    if (eql(ns, "orphans") or eql(ns, "orphan")) return 17;
    return null;
}

fn matchesLane(obj: model.Object, li: usize) bool {
    return switch (li) {
        0 => obj.kind == .todo or contains(obj.title, "TODO") or contains(obj.preview, "TODO"),
        1 => obj.kind == .todo or contains(obj.title, "TODO") or contains(obj.title, "FAIL") or contains(obj.preview, "error:") or contains(obj.preview, "regression") or contains(obj.preview, "blocks"),
        2 => obj.kind == .record or obj.kind == .heading or obj.kind == .concept or obj.kind == .info or contains(obj.tags, "@records") or contains(obj.tags, "@info") or contains(obj.path, "context/") or contains(obj.path, "category/") or contains(obj.path, "notes") or contains(obj.title, "OBS") or contains(obj.title, "DEC") or contains(obj.title, "INF") or contains(obj.preview, "[OBS") or contains(obj.preview, "[DEC") or contains(obj.preview, "[INF"),
        3 => obj.kind == .test_kind or contains(obj.path, "tests/") or contains(obj.tags, "@tests") or contains(obj.preview, "TEST-"),
        4 => obj.kind == .info or contains(obj.tags, "@info") or contains(obj.path, "info") or contains(obj.path, "docs") or contains(obj.title, "Info"),
        5 => obj.kind == .source or obj.kind == .script or contains(obj.tags, "@source") or contains(obj.path, "src/") or contains(obj.path, ".c") or contains(obj.path, ".h") or contains(obj.path, ".zig"),
        6 => obj.kind == .record or contains(obj.tags, "@records"),
        7 => contains(obj.title, "OBS") or contains(obj.preview, "[OBS") or contains(obj.preview, "OBS"),
        8 => contains(obj.title, "DEC") or contains(obj.preview, "[DEC") or contains(obj.preview, "DEC"),
        9 => contains(obj.title, "INF") or contains(obj.preview, "[INF") or contains(obj.preview, "INF"),
        10 => contains(obj.title, "BUG") or contains(obj.title, "FAIL") or contains(obj.preview, "bug") or contains(obj.preview, "FAIL") or contains(obj.preview, "error:"),
        11 => contains(obj.path, "wisp") or contains(obj.title, "wisp") or contains(obj.preview, "wisp") or contains(obj.id, "wisp"),
        12 => contains(obj.path, "reader") or contains(obj.title, "reader") or contains(obj.preview, "reader") or contains(obj.id, "reader"),
        13 => contains(obj.path, "codegen") or contains(obj.title, "codegen") or contains(obj.preview, "codegen") or contains(obj.id, "codegen"),
        18 => obj.kind == .function_kind or contains(obj.tags, "@functions") or contains(obj.tags, "@function"),
        else => false,
    };
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return text.containsFold(haystack, needle);
}

fn eql(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}


pub fn kindIndex(kind: model.ObjectKind) usize {
    return switch (kind) {
        .file => 0,
        .heading => 1,
        .record => 2,
        .script => 3,
        .report => 4,
        .concept => 5,
        .test_kind => 6,
        .source => 7,
        .function_kind => 8,
        .info => 9,
        .todo => 10,
        .done => 11,
        .unknown => 12,
    };
}

pub fn edgeIndex(kind: model.EdgeKind) usize {
    return switch (kind) {
        .contains => 0,
        .file_link => 1,
        .id_link => 2,
        .supports => 3,
        .supersedes => 4,
        .verifies => 5,
        .blocks => 6,
        .refines => 7,
        .classifies_as => 8,
        .forgets_to => 9,
        .generated_by => 10,
        .mentions => 11,
        .unknown => 12,
    };
}

test "index empty" {
    var idx = InvertedIndex.init(std.testing.allocator);
    defer idx.deinit();
    try std.testing.expect(idx.lookup("x") == null);
}


test "search index builds adjacency, kinds, and term candidates" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "src.reader", .kind = .source, .title = "reader source", .path = "src/reader.c", .preview = "rareneedle parser implementation" });
    _ = try ctx.addObject(.{ .id = "test.reader", .kind = .test_kind, .title = "reader test", .path = "tests/reader.mon", .preview = "verifies rareneedle" });
    _ = try ctx.addObject(.{ .id = "todo.codegen", .kind = .todo, .title = "TODO codegen", .path = "context/todo.org", .preview = "unrelated" });
    _ = try ctx.addEdge(.{ .id = "e.verify", .kind = .verifies, .src = "test.reader", .dst = "src.reader" });

    var idx = try SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    try std.testing.expect(idx.objectsOfKind(.source).len == 1);
    try std.testing.expect(idx.edgesOfKind(.verifies).len == 1);
    try std.testing.expect(idx.outgoing("test.reader").len == 1);
    try std.testing.expect(idx.incoming("src.reader").len == 1);
    try std.testing.expect(idx.outgoingKindCount(1, .verifies) == 1);
    try std.testing.expect(idx.incomingKindCount(0, .verifies) == 1);
    try std.testing.expect(idx.edgeSrc(0).? == 1);
    try std.testing.expect(idx.edgeDst(0).? == 0);
    try std.testing.expect(!idx.isOrphan(0));
    try std.testing.expect(idx.objectsOfLane("tests").?.len == 1);
    try std.testing.expect(idx.objectsOfLane("reader").?.len >= 2);
    const rare_count = try idx.candidateCountForWord(std.testing.allocator, "rareneedle");
    try std.testing.expect(rare_count == 2);
}

test "indexed candidate path stays selective on synthetic corpus" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    var i: usize = 0;
    while (i < 320) : (i += 1) {
        var id_buf: [64]u8 = undefined;
        var title_buf: [96]u8 = undefined;
        const id = try std.fmt.bufPrint(&id_buf, "obj.{d}", .{i});
        const title = try std.fmt.bufPrint(&title_buf, "ordinary object {d}", .{i});
        const preview = if (i % 64 == 0) "needlefast indexed benchmark marker" else "ordinary context text";
        _ = try ctx.addObject(.{ .id = id, .kind = .record, .title = title, .path = "context/synth.org", .preview = preview });
    }
    var idx = try SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const candidates = try idx.candidateCountForWord(std.testing.allocator, "needlefast");
    try std.testing.expect(candidates == 5);
    try std.testing.expect(candidates < ctx.objects.items.len / 8);
}

test "exact candidate marking reuses caller scratch buffer" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "a", .kind = .record, .title = "alpha", .preview = "needle" });
    _ = try ctx.addObject(.{ .id = "b", .kind = .record, .title = "beta", .preview = "hay" });
    var idx = try SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    const marks = try std.testing.allocator.alloc(bool, ctx.objects.items.len);
    defer std.testing.allocator.free(marks);
    const n = try idx.markExactWordCandidates(std.testing.allocator, "needle", marks);
    try std.testing.expect(n == 1);
    try std.testing.expect(marks[0]);
    try std.testing.expect(!marks[1]);
}

test "inverted index stores one posting per object per term" {
    var ctx = try model.Context.init(std.testing.allocator, ".");
    defer ctx.deinit();
    _ = try ctx.addObject(.{ .id = "reader.reader", .kind = .source, .title = "reader reader", .preview = "reader" });
    var idx = try SearchIndex.build(std.testing.allocator, &ctx);
    defer idx.deinit();
    if (idx.text_index.lookup("reader")) |posts| {
        try std.testing.expect(posts.len == 1);
    } else return error.MissingReaderPosting;
}
