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
        for (ctx.objects.items, 0..) |obj, i| {
            try idx.addText(i, obj.id, 5);
            try idx.addText(i, obj.title, 8);
            try idx.addText(i, obj.path, 3);
            try idx.addText(i, obj.preview, 1);
            try idx.addText(i, model.Context.kindName(obj.kind), 4);
        }
        return idx;
    }

    fn addText(self: *InvertedIndex, object_index: usize, bytes: []const u8, weight: i32) !void {
        var pos: usize = 0;
        while (pos < bytes.len) {
            while (pos < bytes.len and !isTokenByte(bytes[pos])) pos += 1;
            const start = pos;
            while (pos < bytes.len and isTokenByte(bytes[pos])) pos += 1;
            if (pos <= start) continue;
            const raw = bytes[start..pos];
            if (raw.len < 2) continue;
            var tmp = std.array_list.Managed(u8).init(self.allocator);
            defer tmp.deinit();
            const norm = try text.normalizeToken(&tmp, raw);
            try self.addTerm(norm, object_index, weight);
        }
    }

    fn addTerm(self: *InvertedIndex, term: []const u8, object_index: usize, weight: i32) !void {
        if (self.terms.get(term)) |entry| {
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

test "index empty" {
    var idx = InvertedIndex.init(std.testing.allocator);
    defer idx.deinit();
    try std.testing.expect(idx.lookup("x") == null);
}
