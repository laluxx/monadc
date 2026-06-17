const std = @import("std");
const model = @import("model.zig");
const file_cache = @import("file_cache.zig");

const Magic = "catface-context-cache-v4-subtree";

pub const Status = enum { disabled, hit, miss, stale, saved, failed };

pub fn signature(files: *const file_cache.Cache) u64 {
    var h = std.hash.Wyhash.init(0x4341544641434532);
    for (files.files.items) |entry| {
        h.update(entry.rel);
        h.update("\n");
        var buf: [32]u8 = undefined;
        const sz = std.fmt.bufPrint(&buf, "{d}:{d}\n", .{ entry.size, entry.mtime }) catch "0:0\n";
        h.update(sz);
    }
    return h.final();
}

pub fn load(allocator: std.mem.Allocator, root_path: []const u8, sig: u64) !?model.Context {
    const path = try cachePath(allocator, root_path);
    defer allocator.free(path);
    const bytes = std.fs.cwd().readFileAlloc(path, allocator, @enumFromInt(128 * 1024 * 1024)) catch return null;
    defer allocator.free(bytes);
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    const header = lines.next() orelse return null;
    var cols = std.mem.splitScalar(u8, header, '\t');
    const magic = cols.next() orelse return null;
    if (!std.mem.eql(u8, magic, Magic)) return null;
    const sig_text = cols.next() orelse return null;
    const root_text = cols.next() orelse return null;
    const stored_sig = std.fmt.parseUnsigned(u64, sig_text, 16) catch return null;
    if (stored_sig != sig) return null;
    if (!std.mem.eql(u8, root_text, root_path)) return null;
    var ctx = try model.Context.init(allocator, root_path);
    errdefer ctx.deinit();
    while (lines.next()) |line| {
        if (line.len == 0) continue;
        var parts = std.mem.splitScalar(u8, line, '\t');
        const rec = parts.next() orelse continue;
        if (std.mem.eql(u8, rec, "O")) {
            const kind_s = parts.next() orelse continue;
            const id_s = parts.next() orelse continue;
            const title_s = parts.next() orelse "";
            const path_s = parts.next() orelse "";
            const line_s = parts.next() orelse "0";
            const tags_s = parts.next() orelse "";
            const preview_s = parts.next() orelse "";
            const weight_s = parts.next() orelse "0";
            const id = try unescape(allocator, id_s); defer allocator.free(id);
            const title = try unescape(allocator, title_s); defer allocator.free(title);
            const obj_path = try unescape(allocator, path_s); defer allocator.free(obj_path);
            const tags = try unescape(allocator, tags_s); defer allocator.free(tags);
            const preview = try unescape(allocator, preview_s); defer allocator.free(preview);
            const line_no = std.fmt.parseUnsigned(usize, line_s, 10) catch 0;
            const weight = std.fmt.parseInt(i32, weight_s, 10) catch 0;
            _ = try ctx.addObject(.{ .id = id, .kind = model.Context.parseObjectKind(kind_s), .title = title, .path = obj_path, .line = line_no, .tags = tags, .preview = preview, .weight = weight });
        } else if (std.mem.eql(u8, rec, "E")) {
            const kind_s = parts.next() orelse continue;
            const id_s = parts.next() orelse continue;
            const src_s = parts.next() orelse continue;
            const dst_s = parts.next() orelse continue;
            const label_s = parts.next() orelse "";
            const path_s = parts.next() orelse "";
            const line_s = parts.next() orelse "0";
            const id = try unescape(allocator, id_s); defer allocator.free(id);
            const src = try unescape(allocator, src_s); defer allocator.free(src);
            const dst = try unescape(allocator, dst_s); defer allocator.free(dst);
            const label = try unescape(allocator, label_s); defer allocator.free(label);
            const edge_path = try unescape(allocator, path_s); defer allocator.free(edge_path);
            const line_no = std.fmt.parseUnsigned(usize, line_s, 10) catch 0;
            _ = try ctx.addEdge(.{ .id = id, .kind = model.Context.parseEdgeKind(kind_s), .src = src, .dst = dst, .label = label, .path = edge_path, .line = line_no });
        }
    }
    return ctx;
}

pub fn save(allocator: std.mem.Allocator, root_path: []const u8, sig: u64, ctx: *const model.Context) !void {
    const path = try cachePath(allocator, root_path);
    defer allocator.free(path);
    const dir_path = try cacheDir(allocator);
    defer allocator.free(dir_path);
    std.fs.cwd().makePath(dir_path) catch {};
    var file = try std.fs.cwd().createFile(path, .{ .truncate = true });
    defer file.close();
    var buf: [32768]u8 = undefined;
    var writer = file.writer(&buf);
    const out = &writer.interface;
    try out.print("{s}\t{x}\t{s}\t{d}\t{d}\n", .{ Magic, sig, root_path, ctx.objects.items.len, ctx.edges.items.len });
    for (ctx.objects.items) |obj| {
        try out.writeAll("O\t");
        try out.writeAll(model.Context.kindName(obj.kind));
        try out.writeAll("\t");
        try writeEscaped(out, obj.id); try out.writeAll("\t");
        try writeEscaped(out, obj.title); try out.writeAll("\t");
        try writeEscaped(out, obj.path); try out.print("\t{d}\t", .{obj.line});
        try writeEscaped(out, obj.tags); try out.writeAll("\t");
        try writeEscaped(out, obj.preview); try out.print("\t{d}\n", .{obj.weight});
    }
    for (ctx.edges.items) |edge| {
        try out.writeAll("E\t");
        try out.writeAll(model.Context.edgeName(edge.kind)); try out.writeAll("\t");
        try writeEscaped(out, edge.id); try out.writeAll("\t");
        try writeEscaped(out, edge.src); try out.writeAll("\t");
        try writeEscaped(out, edge.dst); try out.writeAll("\t");
        try writeEscaped(out, edge.label); try out.writeAll("\t");
        try writeEscaped(out, edge.path); try out.print("\t{d}\n", .{edge.line});
    }
    try out.flush();
}

pub fn cachePath(allocator: std.mem.Allocator, root_path: []const u8) ![]u8 {
    const dir = try cacheDir(allocator);
    defer allocator.free(dir);
    const h = rootHash(root_path);
    return std.fmt.allocPrint(allocator, "{s}/context-{x}.tsv", .{ dir, h });
}

fn cacheDir(allocator: std.mem.Allocator) ![]u8 {
    if (std.process.getEnvVarOwned(allocator, "CATFACE_CACHE_DIR")) |dir| {
        return dir;
    } else |_| {}
    if (std.process.getEnvVarOwned(allocator, "XDG_CACHE_HOME")) |xdg| {
        defer allocator.free(xdg);
        return std.fmt.allocPrint(allocator, "{s}/catface", .{xdg});
    } else |_| {}
    if (std.process.getEnvVarOwned(allocator, "HOME")) |home| {
        defer allocator.free(home);
        return std.fmt.allocPrint(allocator, "{s}/.config/catface/cache", .{home});
    } else |_| {}
    return allocator.dupe(u8, ".catface-cache");
}

fn rootHash(root_path: []const u8) u64 {
    var h = std.hash.Wyhash.init(0x4341544641434552);
    h.update(root_path);
    h.update("\nsubtree-only-v4");
    return h.final();
}

fn writeEscaped(out: anytype, s: []const u8) !void {
    for (s) |c| switch (c) {
        '\\' => try out.writeAll("\\\\"),
        '\t' => try out.writeAll("\\t"),
        '\n' => try out.writeAll("\\n"),
        '\r' => try out.writeAll("\\r"),
        else => try out.writeAll(&[_]u8{c}),
    };
}

fn unescape(allocator: std.mem.Allocator, s: []const u8) ![]u8 {
    var out = std.array_list.Managed(u8).init(allocator);
    defer out.deinit();
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        if (s[i] != '\\' or i + 1 >= s.len) {
            try out.append(s[i]);
            continue;
        }
        i += 1;
        switch (s[i]) {
            't' => try out.append('\t'),
            'n' => try out.append('\n'),
            'r' => try out.append('\r'),
            '\\' => try out.append('\\'),
            else => { try out.append('\\'); try out.append(s[i]); },
        }
    }
    return allocator.dupe(u8, out.items);
}


test "context cache root hash is subtree scoped" {
    const a = rootHash("/repo/context");
    const b = rootHash("/repo/context/category/catface");
    try std.testing.expect(a != b);
}

test "context cache unescapes fields" {
    const decoded = try unescape(std.testing.allocator, "a\tb\\c\n");
    defer std.testing.allocator.free(decoded);
    try std.testing.expect(std.mem.eql(u8, decoded, "a\tb\\c\n"));
}
