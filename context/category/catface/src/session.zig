const std = @import("std");

pub const Bookmark = struct {
    id: []const u8,
    label: []const u8,
};

pub const Session = struct {
    allocator: std.mem.Allocator,
    last_query: []const u8 = "",
    bookmarks: std.array_list.Managed(Bookmark),

    pub fn init(allocator: std.mem.Allocator) Session {
        return .{ .allocator = allocator, .bookmarks = std.array_list.Managed(Bookmark).init(allocator) };
    }

    pub fn deinit(self: *Session) void {
        self.allocator.free(self.last_query);
        for (self.bookmarks.items) |b| {
            self.allocator.free(b.id);
            self.allocator.free(b.label);
        }
        self.bookmarks.deinit();
    }

    pub fn setQuery(self: *Session, q: []const u8) !void {
        if (self.last_query.len != 0) self.allocator.free(self.last_query);
        self.last_query = try self.allocator.dupe(u8, q);
    }

    pub fn addBookmark(self: *Session, id: []const u8, label: []const u8) !void {
        for (self.bookmarks.items) |b| {
            if (std.mem.eql(u8, b.id, id)) return;
        }
        try self.bookmarks.append(.{ .id = try self.allocator.dupe(u8, id), .label = try self.allocator.dupe(u8, label) });
    }

    pub fn save(self: *const Session, path: []const u8) !void {
        var file = try std.fs.cwd().createFile(path, .{ .truncate = true });
        defer file.close();
        var buf: [8192]u8 = undefined;
        var w = file.writer(&buf);
        try w.interface.print("last_query\t{s}\n", .{self.last_query});
        for (self.bookmarks.items) |b| {
            try w.interface.print("bookmark\t{s}\t{s}\n", .{ b.id, b.label });
        }
        try w.interface.flush();
    }

    pub fn load(allocator: std.mem.Allocator, path: []const u8) !Session {
        var s = Session.init(allocator);
        const bytes = std.fs.cwd().readFileAlloc(path, allocator, @enumFromInt(1024 * 1024)) catch return s;
        defer allocator.free(bytes);
        var lines = std.mem.splitScalar(u8, bytes, '\n');
        while (lines.next()) |line| {
            var cols = std.mem.splitScalar(u8, line, '\t');
            const kind = cols.next() orelse continue;
            if (std.mem.eql(u8, kind, "last_query")) try s.setQuery(cols.next() orelse "");
            if (std.mem.eql(u8, kind, "bookmark")) try s.addBookmark(cols.next() orelse "", cols.next() orelse "");
        }
        return s;
    }
};

test "session bookmark" {
    var s = Session.init(std.testing.allocator);
    defer s.deinit();
    try s.addBookmark("A", "alpha");
    try std.testing.expect(s.bookmarks.items.len == 1);
}
