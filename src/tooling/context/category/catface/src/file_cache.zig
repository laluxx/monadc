const std = @import("std");

pub const Entry = struct {
    rel: []const u8,
    size: u64 = 0,
    mtime: i128 = 0,
};

pub const Cache = struct {
    allocator: std.mem.Allocator,
    files: std.array_list.Managed(Entry),
    directories_scanned: usize = 0,
    directories_skipped: usize = 0,

    pub fn init(allocator: std.mem.Allocator) Cache {
        return .{ .allocator = allocator, .files = std.array_list.Managed(Entry).init(allocator) };
    }

    pub fn build(allocator: std.mem.Allocator, root_path: []const u8) !Cache {
        var cache = Cache.init(allocator);
        errdefer cache.deinit();
        try cache.collect(root_path, "");
        std.mem.sort(Entry, cache.files.items, {}, lessEntry);
        return cache;
    }

    pub fn deinit(self: *Cache) void {
        for (self.files.items) |entry| self.allocator.free(entry.rel);
        self.files.deinit();
    }

    fn collect(self: *Cache, abs_root: []const u8, rel_dir: []const u8) !void {
        const abs_dir = if (rel_dir.len == 0) try self.allocator.dupe(u8, abs_root) else try std.fs.path.join(self.allocator, &.{ abs_root, rel_dir });
        defer self.allocator.free(abs_dir);
        var dir = std.fs.cwd().openDir(abs_dir, .{ .iterate = true }) catch return;
        defer dir.close();
        self.directories_scanned += 1;
        var it = dir.iterate();
        while (try it.next()) |entry| {
            if (shouldSkip(entry.name)) {
                if (entry.kind == .directory) self.directories_skipped += 1;
                continue;
            }
            const child_rel = if (rel_dir.len == 0) try self.allocator.dupe(u8, entry.name) else try std.fs.path.join(self.allocator, &.{ rel_dir, entry.name });
            switch (entry.kind) {
                .directory => {
                    defer self.allocator.free(child_rel);
                    try self.collect(abs_root, child_rel);
                },
                .file => {
                    if (isTextArtifact(entry.name)) {
                        const st = dir.statFile(entry.name) catch null;
                        const fsize: u64 = if (st) |x| x.size else 0;
                        const fmtime: i128 = if (st) |x| timestampToI128(x.mtime) else 0;
                        try self.files.append(.{ .rel = child_rel, .size = fsize, .mtime = fmtime });
                    } else {
                        self.allocator.free(child_rel);
                    }
                },
                else => self.allocator.free(child_rel),
            }
        }
    }
};

fn timestampToI128(ts: anytype) i128 {
    const T = @TypeOf(ts);
    return switch (@typeInfo(T)) {
        .int, .comptime_int => @as(i128, @intCast(ts)),
        .@"struct" => blk: {
            if (@hasField(T, "seconds")) {
                const sec: i128 = @intCast(ts.seconds);
                const ns: i128 = if (@hasField(T, "nanoseconds")) @intCast(ts.nanoseconds) else 0;
                break :blk sec * 1000000000 + ns;
            }
            if (@hasField(T, "sec")) {
                const sec: i128 = @intCast(ts.sec);
                const ns: i128 = if (@hasField(T, "nsec")) @intCast(ts.nsec) else 0;
                break :blk sec * 1000000000 + ns;
            }
            if (@hasField(T, "tv_sec")) {
                const sec: i128 = @intCast(ts.tv_sec);
                const ns: i128 = if (@hasField(T, "tv_nsec")) @intCast(ts.tv_nsec) else 0;
                break :blk sec * 1000000000 + ns;
            }
            break :blk 0;
        },
        else => 0,
    };
}

fn lessEntry(_: void, a: Entry, b: Entry) bool {
    return std.mem.lessThan(u8, a.rel, b.rel);
}

pub fn shouldSkip(name: []const u8) bool {
    return std.mem.eql(u8, name, ".") or
        std.mem.eql(u8, name, "..") or
        std.mem.eql(u8, name, ".git") or
        std.mem.eql(u8, name, ".zig-cache") or
        std.mem.eql(u8, name, "zig-cache") or
        std.mem.eql(u8, name, "zig-out") or
        std.mem.eql(u8, name, "node_modules") or
        std.mem.eql(u8, name, ".cache") or
        std.mem.eql(u8, name, "__pycache__") or
        std.mem.eql(u8, name, "build") or
        std.mem.eql(u8, name, "dist") or
        std.mem.eql(u8, name, ".last-failures");
}

pub fn isTextArtifact(name: []const u8) bool {
    const exts = [_][]const u8{ ".org", ".mon", ".wisp", ".lisp", ".c", ".h", ".zig", ".py", ".sh", ".md", ".txt", ".tsv", ".json", ".yaml", ".yml", ".zon", ".scm" };
    for (exts) |ext| {
        if (std.mem.endsWith(u8, name, ext)) return true;
    }
    return false;
}

pub fn basename(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |i| return path[i + 1 ..];
    return path;
}

test "timestamp compatibility helper handles integer times" {
    try std.testing.expect(timestampToI128(@as(i64, 42)) == 42);
}

test "file cache skips heavy directories and keeps text artifacts" {
    try std.testing.expect(shouldSkip(".git"));
    try std.testing.expect(shouldSkip("node_modules"));
    try std.testing.expect(isTextArtifact("reader.c"));
    try std.testing.expect(!isTextArtifact("catface.svg"));
    try std.testing.expect(std.mem.eql(u8, basename("a/b/c.org"), "c.org"));
}

test "file cache entry ordering ignores metadata" {
    const a = Entry{ .rel = "a.org", .size = 99, .mtime = 1 };
    const b = Entry{ .rel = "b.org", .size = 1, .mtime = 2 };
    try std.testing.expect(lessEntry({}, a, b));
}
