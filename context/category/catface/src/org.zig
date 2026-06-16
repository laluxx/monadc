const std = @import("std");
const model = @import("model.zig");
const file_cache = @import("file_cache.zig");
const context_cache = @import("context_cache.zig");

const PendingLink = struct {
    src: []const u8,
    dst: []const u8,
    kind: model.EdgeKind,
    path: []const u8,
    line: usize,
};

pub fn loadContext(allocator: std.mem.Allocator, root_path: []const u8) !model.Context {
    var files = try file_cache.Cache.build(allocator, root_path);
    defer files.deinit();
    const sig = context_cache.signature(&files);
    if (try context_cache.load(allocator, root_path, sig)) |cached| {
        return cached;
    }

    var ctx = try model.Context.init(allocator, root_path);
    errdefer ctx.deinit();
    var pending = std.array_list.Managed(PendingLink).init(allocator);
    defer {
        for (pending.items) |p| {
            allocator.free(p.src);
            allocator.free(p.dst);
            allocator.free(p.path);
        }
        pending.deinit();
    }

    for (files.files.items) |entry| {
        try parseUniversalFile(allocator, &ctx, &pending, root_path, entry.rel, file_cache.basename(entry.rel));
    }
    try loadCategoryTables(allocator, &ctx, root_path);
    try resolvePending(&ctx, &pending);
    context_cache.save(allocator, root_path, sig, &ctx) catch {};
    return ctx;
}

fn walk(allocator: std.mem.Allocator, ctx: *model.Context, pending: *std.array_list.Managed(PendingLink), abs_root: []const u8, rel_dir: []const u8) !void {
    const abs_dir = if (rel_dir.len == 0) try allocator.dupe(u8, abs_root) else try std.fs.path.join(allocator, &.{ abs_root, rel_dir });
    defer allocator.free(abs_dir);
    var dir = std.fs.cwd().openDir(abs_dir, .{ .iterate = true }) catch return;
    defer dir.close();
    var it = dir.iterate();
    while (try it.next()) |entry| {
        if (shouldSkip(entry.name)) continue;
        const child_rel = if (rel_dir.len == 0) try allocator.dupe(u8, entry.name) else try std.fs.path.join(allocator, &.{ rel_dir, entry.name });
        defer allocator.free(child_rel);
        switch (entry.kind) {
            .directory => try walk(allocator, ctx, pending, abs_root, child_rel),
            .file => try parseUniversalFile(allocator, ctx, pending, abs_root, child_rel, entry.name),
            else => {},
        }
    }
}

fn shouldSkip(name: []const u8) bool {
    return std.mem.eql(u8, name, ".git") or
        std.mem.eql(u8, name, ".zig-cache") or
        std.mem.eql(u8, name, "zig-cache") or
        std.mem.eql(u8, name, "zig-out") or
        std.mem.eql(u8, name, "node_modules") or
        std.mem.eql(u8, name, "__pycache__") or
        std.mem.eql(u8, name, ".last-failures");
}

fn parseUniversalFile(allocator: std.mem.Allocator, ctx: *model.Context, pending: *std.array_list.Managed(PendingLink), abs_root: []const u8, rel: []const u8, name: []const u8) !void {
    if (!isTextArtifact(name)) return;
    if (std.mem.endsWith(u8, name, ".org")) {
        try parseOrgFile(allocator, ctx, pending, abs_root, rel);
        return;
    }
    if (isTestArtifact(rel, name)) {
        try parseTestFile(allocator, ctx, abs_root, rel);
        return;
    }
    if (isScript(name)) {
        try addSourceFile(allocator, ctx, abs_root, rel, .script, "source artifact / script");
        return;
    }
    if (isCodeArtifact(name)) {
        try addSourceFile(allocator, ctx, abs_root, rel, .source, "compiler/runtime source artifact");
        if (isMonadFunctionArtifact(name)) try parseFunctionFile(allocator, ctx, abs_root, rel);
        return;
    }
    if (isInfoArtifact(rel, name)) {
        try addSourceFile(allocator, ctx, abs_root, rel, .info, "information artifact");
        return;
    }
    try addSourceFile(allocator, ctx, abs_root, rel, .file, "text artifact");
}

fn isTextArtifact(name: []const u8) bool {
    return file_cache.isTextArtifact(name);
}

fn isScript(name: []const u8) bool {
    return std.mem.endsWith(u8, name, ".py") or std.mem.endsWith(u8, name, ".zig") or std.mem.endsWith(u8, name, ".sh");
}

fn isCodeArtifact(name: []const u8) bool {
    return std.mem.endsWith(u8, name, ".c") or std.mem.endsWith(u8, name, ".h") or std.mem.endsWith(u8, name, ".zig") or std.mem.endsWith(u8, name, ".mon") or std.mem.endsWith(u8, name, ".wisp") or std.mem.endsWith(u8, name, ".lisp");
}

fn isMonadFunctionArtifact(name: []const u8) bool {
    return std.mem.endsWith(u8, name, ".mon") or std.mem.endsWith(u8, name, ".wisp") or std.mem.endsWith(u8, name, ".lisp");
}

fn isTestArtifact(rel: []const u8, name: []const u8) bool {
    return std.mem.indexOf(u8, rel, "tests/") != null or std.mem.indexOf(u8, rel, "/test") != null or std.mem.startsWith(u8, name, "test_") or std.mem.endsWith(u8, name, ".stdout") or std.mem.endsWith(u8, name, ".desugar");
}

fn isInfoArtifact(rel: []const u8, name: []const u8) bool {
    return std.mem.indexOf(u8, rel, "info") != null or std.mem.indexOf(u8, rel, "docs") != null or std.mem.eql(u8, name, "README.org") or std.mem.eql(u8, name, "README.md");
}

fn addSourceFile(allocator: std.mem.Allocator, ctx: *model.Context, abs_root: []const u8, rel: []const u8, kind: model.ObjectKind, default_preview: []const u8) !void {
    var id_buf: [4096]u8 = undefined;
    const prefix = switch (kind) { .script => "script", .source => "source", .info => "info", .test_kind => "test", else => "file" };
    const id = try std.fmt.bufPrint(&id_buf, "{s}:{s}", .{ prefix, rel });
    const abs = try std.fs.path.join(allocator, &.{ abs_root, rel });
    defer allocator.free(abs);
    const preview = try readPreview(allocator, abs, default_preview);
    defer allocator.free(preview);
    _ = try ctx.addObject(.{ .id = id, .kind = kind, .title = model.basename(rel), .path = rel, .line = 1, .tags = namespaceTags(rel, kind), .preview = preview });
}

fn parseTestFile(allocator: std.mem.Allocator, ctx: *model.Context, abs_root: []const u8, rel: []const u8) !void {
    const abs = try std.fs.path.join(allocator, &.{ abs_root, rel });
    defer allocator.free(abs);
    const bytes = std.fs.cwd().readFileAlloc(abs, allocator, @enumFromInt(1024 * 1024)) catch {
        try addSourceFile(allocator, ctx, abs_root, rel, .test_kind, "test artifact");
        return;
    };
    defer allocator.free(bytes);
    var id: ?[]const u8 = null;
    var title: ?[]const u8 = null;
    var line_no: usize = 0;
    var preview = std.array_list.Managed(u8).init(allocator);
    defer preview.deinit();
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw| {
        line_no += 1;
        const line = std.mem.trimRight(u8, raw, "\r");
        const trimmed = std.mem.trim(u8, line, " \t;");
        if (std.mem.startsWith(u8, trimmed, "TEST-ID:")) id = std.mem.trim(u8, trimmed[8..], " \t");
        if (std.mem.startsWith(u8, trimmed, "TEST-PURPOSE:")) title = std.mem.trim(u8, trimmed[13..], " \t");
        if (preview.items.len < 420 and line.len != 0) {
            if (preview.items.len != 0) try preview.append(' ');
            try preview.appendSlice(std.mem.trim(u8, line, " \t;"));
        }
        if (line_no > 40) break;
    }
    var id_buf: [4096]u8 = undefined;
    const oid = if (id) |tid| try std.fmt.bufPrint(&id_buf, "test:{s}", .{tid}) else try std.fmt.bufPrint(&id_buf, "test:{s}", .{rel});
    _ = try ctx.addObject(.{ .id = oid, .kind = .test_kind, .title = title orelse model.basename(rel), .path = rel, .line = 1, .tags = "@tests", .preview = preview.items });
}

const FunctionDef = struct {
    name: []const u8,
    signature: []const u8,
};

fn parseFunctionFile(allocator: std.mem.Allocator, ctx: *model.Context, abs_root: []const u8, rel: []const u8) !void {
    const abs = try std.fs.path.join(allocator, &.{ abs_root, rel });
    defer allocator.free(abs);
    const bytes = std.fs.cwd().readFileAlloc(abs, allocator, @enumFromInt(4 * 1024 * 1024)) catch return;
    defer allocator.free(bytes);
    var source_id_buf: [4096]u8 = undefined;
    const source_id = try std.fmt.bufPrint(&source_id_buf, "source:{s}", .{rel});
    var line_no: usize = 0;
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw| {
        line_no += 1;
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0 or std.mem.startsWith(u8, line, ";;") or std.mem.startsWith(u8, line, "#")) continue;
        const def = parseFunctionDefineLine(line) orelse continue;
        if (def.name.len == 0 or def.signature.len == 0) continue;
        var id_buf: [4096]u8 = undefined;
        const oid = try std.fmt.bufPrint(&id_buf, "function:{s}:{s}", .{ rel, def.name });
        var preview_buf: [1024]u8 = undefined;
        const preview = try std.fmt.bufPrint(&preview_buf, "function {s} :: {s}", .{ def.name, def.signature });
        const tags = if (std.mem.indexOf(u8, rel, "core/") != null or std.mem.indexOf(u8, rel, "prelude/") != null) "@functions @core" else "@functions";
        _ = try ctx.addObject(.{ .id = oid, .kind = .function_kind, .title = def.name, .path = rel, .line = line_no, .tags = tags, .preview = preview, .weight = 32 });
        var edge_id_buf: [8192]u8 = undefined;
        const eid = try std.fmt.bufPrint(&edge_id_buf, "contains:{s}:{s}", .{ source_id, oid });
        _ = try ctx.addEdge(.{ .id = eid, .kind = .contains, .src = source_id, .dst = oid, .label = "contains function", .path = rel, .line = line_no });
    }
}

fn parseFunctionDefineLine(line: []const u8) ?FunctionDef {
    const trimmed = std.mem.trim(u8, line, " \t\r");
    if (std.mem.startsWith(u8, trimmed, "(define ")) return parseLispDefine(trimmed[8..]);
    if (std.mem.startsWith(u8, trimmed, "define ")) return parseWispDefine(trimmed[7..]);
    return null;
}

fn parseLispDefine(rest_raw: []const u8) ?FunctionDef {
    const rest = std.mem.trim(u8, rest_raw, " \t");
    if (rest.len == 0) return null;
    if (rest[0] == '(') {
        const end = std.mem.indexOfScalar(u8, rest, ')') orelse rest.len;
        const inner = std.mem.trim(u8, rest[1..end], " \t");
        if (std.mem.indexOf(u8, inner, "->") == null) return null;
        const name_end = firstSpace(inner) orelse return null;
        return .{ .name = std.mem.trim(u8, inner[0..name_end], " \t"), .signature = std.mem.trim(u8, inner[name_end + 1 ..], " \t") };
    }
    if (rest[0] == '[') {
        const end = std.mem.indexOfScalar(u8, rest, ']') orelse return null;
        return parseTypedBinding(rest[1..end]);
    }
    return null;
}

fn parseWispDefine(rest_raw: []const u8) ?FunctionDef {
    const rest = std.mem.trim(u8, rest_raw, " \t");
    if (rest.len == 0) return null;
    if (rest[0] == '[') {
        const end = std.mem.indexOfScalar(u8, rest, ']') orelse return null;
        return parseTypedBinding(rest[1..end]);
    }
    const name_end = firstSpace(rest) orelse return null;
    const name = std.mem.trim(u8, rest[0..name_end], " \t");
    var sig = std.mem.trim(u8, rest[name_end + 1 ..], " \t");
    if (std.mem.startsWith(u8, sig, "::")) sig = std.mem.trim(u8, sig[2..], " \t");
    if (std.mem.indexOf(u8, sig, "->") == null) return null;
    return .{ .name = name, .signature = sig };
}

fn parseTypedBinding(inner_raw: []const u8) ?FunctionDef {
    const inner = std.mem.trim(u8, inner_raw, " \t");
    const pos = std.mem.indexOf(u8, inner, "::") orelse std.mem.indexOfScalar(u8, inner, ':') orelse return null;
    const name = std.mem.trim(u8, inner[0..pos], " \t");
    const sig_start = if (pos + 1 < inner.len and inner[pos + 1] == ':') pos + 2 else pos + 1;
    const sig = std.mem.trim(u8, inner[sig_start..], " \t");
    if (std.mem.indexOf(u8, sig, "->") == null) return null;
    return .{ .name = name, .signature = sig };
}

fn firstSpace(s: []const u8) ?usize {
    for (s, 0..) |c, i| {
        if (std.ascii.isWhitespace(c)) return i;
    }
    return null;
}

fn namespaceTags(rel: []const u8, kind: model.ObjectKind) []const u8 {
    _ = rel;
    return switch (kind) {
        .test_kind => "@tests",
        .info => "@info",
        .source => "@source",
        .function_kind => "@functions",
        .script => "@source @scripts",
        .todo => "@todo",
        .done => "@done",
        else => "",
    };
}

fn readPreview(allocator: std.mem.Allocator, abs: []const u8, fallback: []const u8) ![]const u8 {
    const bytes = std.fs.cwd().readFileAlloc(abs, allocator, @enumFromInt(256 * 1024)) catch return allocator.dupe(u8, fallback);
    defer allocator.free(bytes);
    var out = std.array_list.Managed(u8).init(allocator);
    defer out.deinit();
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw| {
        const t = std.mem.trim(u8, raw, " \t\r;");
        if (t.len == 0) continue;
        if (out.items.len != 0) try out.append(' ');
        try out.appendSlice(t);
        if (out.items.len > 420) break;
    }
    if (out.items.len == 0) return allocator.dupe(u8, fallback);
    return allocator.dupe(u8, out.items[0..@min(out.items.len, 420)]);
}

fn parseOrgFile(allocator: std.mem.Allocator, ctx: *model.Context, pending: *std.array_list.Managed(PendingLink), abs_root: []const u8, rel: []const u8) !void {
    const abs = try std.fs.path.join(allocator, &.{ abs_root, rel });
    defer allocator.free(abs);
    const bytes = std.fs.cwd().readFileAlloc(abs, allocator, @enumFromInt(8 * 1024 * 1024)) catch return;
    defer allocator.free(bytes);

    var file_id_buf: [4096]u8 = undefined;
    const root_id = extractRootId(bytes);
    const file_id = root_id orelse try std.fmt.bufPrint(&file_id_buf, "file:{s}", .{rel});
    const file_kind = inferFileKind(rel);
    const file_title = extractTitle(bytes) orelse model.basename(rel);
    const file_preview = try extractCleanPreview(allocator, bytes, if (file_kind == .info) "info reference page" else "org file");
    defer allocator.free(file_preview);
    _ = try ctx.addObject(.{ .id = file_id, .kind = file_kind, .title = file_title, .path = rel, .line = 1, .tags = extractTags(bytes) orelse namespaceTags(rel, file_kind), .preview = file_preview });

    var current_heading_title: []const u8 = "";
    var current_heading_line: usize = 1;
    var current_heading_id: ?[]const u8 = null;
    var seen_heading = false;
    var current_preview = std.array_list.Managed(u8).init(allocator);
    defer current_preview.deinit();

    var line_no: usize = 0;
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line_raw| {
        line_no += 1;
        const line = std.mem.trimRight(u8, line_raw, "\r");
        if (isHeading(line)) {
            if (current_heading_id) |hid| {
                try finishHeading(ctx, file_id, hid, current_heading_title, rel, current_heading_line, current_preview.items);
                current_heading_id = null;
                current_preview.clearRetainingCapacity();
            }
            current_heading_title = trimHeading(line);
            current_heading_line = line_no;
            seen_heading = true;
            try addTodoDonePseudoHeading(ctx, file_id, current_heading_title, rel, line_no);
            continue;
        }
        if (seen_heading and std.mem.startsWith(u8, std.mem.trim(u8, line, " \t"), ":ID:")) {
            const id = std.mem.trim(u8, line[4..], " \t");
            current_heading_id = id;
        }
        if (line.len != 0 and current_preview.items.len < 360 and !std.mem.startsWith(u8, line, ":") and !std.mem.startsWith(u8, line, "#+")) {
            if (current_preview.items.len != 0) try current_preview.append(' ');
            try current_preview.appendSlice(std.mem.trim(u8, line, " \t"));
        }
        try parseRecordLine(ctx, pending, file_id, rel, line, line_no);
        try parseLinks(allocator, pending, file_id, rel, line, line_no);
    }
    if (current_heading_id) |hid| {
        try finishHeading(ctx, file_id, hid, current_heading_title, rel, current_heading_line, current_preview.items);
    }
}

fn inferFileKind(rel: []const u8) model.ObjectKind {
    if (std.mem.indexOf(u8, rel, "tests/") != null) return .test_kind;
    if (std.mem.indexOf(u8, rel, "info") != null or std.mem.indexOf(u8, rel, "docs") != null) return .info;
    return .file;
}

fn addTodoDonePseudoHeading(ctx: *model.Context, file_id: []const u8, title: []const u8, rel: []const u8, line: usize) !void {
    const t = std.mem.trim(u8, title, " \t");
    const kind: model.ObjectKind = if (std.mem.startsWith(u8, t, "TODO")) .todo else if (std.mem.startsWith(u8, t, "DONE")) .done else return;
    var id_buf: [4096]u8 = undefined;
    const id = try std.fmt.bufPrint(&id_buf, "{s}:{s}:{d}", .{ @tagName(kind), rel, line });
    _ = try ctx.addObject(.{ .id = id, .kind = kind, .title = t, .path = rel, .line = line, .tags = namespaceTags(rel, kind), .preview = t });
    var edge_id_buf: [8192]u8 = undefined;
    const eid = try std.fmt.bufPrint(&edge_id_buf, "contains:{s}:{s}", .{ file_id, id });
    _ = try ctx.addEdge(.{ .id = eid, .kind = .contains, .src = file_id, .dst = id, .label = "contains todo state", .path = rel, .line = line });
}

fn finishHeading(ctx: *model.Context, file_id: []const u8, hid: []const u8, title: []const u8, rel: []const u8, line: usize, preview: []const u8) !void {
    _ = try ctx.addObject(.{ .id = hid, .kind = inferHeadingKind(hid, title, rel), .title = title, .path = rel, .line = line, .preview = preview, .tags = namespaceTags(rel, inferHeadingKind(hid, title, rel)) });
    var edge_id_buf: [8192]u8 = undefined;
    const eid = try std.fmt.bufPrint(&edge_id_buf, "contains:{s}:{s}", .{ file_id, hid });
    _ = try ctx.addEdge(.{ .id = eid, .kind = .contains, .src = file_id, .dst = hid, .label = "contains heading", .path = rel, .line = line });
}

fn inferHeadingKind(id: []const u8, title: []const u8, rel: []const u8) model.ObjectKind {
    if (std.mem.startsWith(u8, std.mem.trim(u8, title, " \t"), "TODO")) return .todo;
    if (std.mem.startsWith(u8, std.mem.trim(u8, title, " \t"), "DONE")) return .done;
    if (std.mem.indexOf(u8, id, ".category.") != null or std.mem.indexOf(u8, title, "Category") != null) return .concept;
    if (std.mem.indexOf(u8, rel, "scripts/") != null or std.mem.endsWith(u8, rel, ".py")) return .script;
    if (std.mem.indexOf(u8, title, "Test") != null or std.mem.indexOf(u8, rel, "test") != null) return .test_kind;
    if (std.mem.indexOf(u8, rel, "info") != null or std.mem.indexOf(u8, rel, "docs") != null) return .info;
    return .heading;
}

fn parseRecordLine(ctx: *model.Context, pending: *std.array_list.Managed(PendingLink), file_id: []const u8, rel: []const u8, line: []const u8, line_no: usize) !void {
    const trimmed = std.mem.trim(u8, line, " \t");
    if (!std.mem.startsWith(u8, trimmed, "[") or std.mem.indexOf(u8, trimmed, " id:") == null) return;
    const rb = std.mem.indexOfScalar(u8, trimmed, ']') orelse return;
    const header = trimmed[1..rb];
    var parts = std.mem.splitScalar(u8, header, ' ');
    const typ = parts.next() orelse "DOC";
    var id: ?[]const u8 = null;
    var refs = std.array_list.Managed([]const u8).init(ctx.allocator);
    defer refs.deinit();
    while (parts.next()) |p| {
        if (std.mem.startsWith(u8, p, "id:")) id = p[3..];
        if (std.mem.startsWith(u8, p, "from:") or std.mem.startsWith(u8, p, "supports:") or std.mem.startsWith(u8, p, "verifies:") or std.mem.startsWith(u8, p, "blocks:") or std.mem.startsWith(u8, p, "supersedes:")) {
            const colon = std.mem.indexOfScalar(u8, p, ':') orelse continue;
            var rs = std.mem.splitScalar(u8, p[colon + 1 ..], ',');
            while (rs.next()) |r| if (r.len != 0) try refs.append(std.mem.trim(u8, r, ",.;"));
        }
    }
    const rid = id orelse return;
    _ = try ctx.addObject(.{ .id = rid, .kind = .record, .title = typ, .path = rel, .line = line_no, .tags = "@records", .preview = trimmed });
    var edge_id_buf: [8192]u8 = undefined;
    const eid = try std.fmt.bufPrint(&edge_id_buf, "contains:{s}:{s}", .{ file_id, rid });
    _ = try ctx.addEdge(.{ .id = eid, .kind = .contains, .src = file_id, .dst = rid, .label = typ, .path = rel, .line = line_no });
    for (refs.items) |r| {
        try pending.append(.{ .src = try ctx.allocator.dupe(u8, rid), .dst = try ctx.allocator.dupe(u8, r), .kind = .supports, .path = try ctx.allocator.dupe(u8, rel), .line = line_no });
    }
}

fn parseLinks(allocator: std.mem.Allocator, pending: *std.array_list.Managed(PendingLink), src: []const u8, rel: []const u8, line: []const u8, line_no: usize) !void {
    var pos: usize = 0;
    while (std.mem.indexOf(u8, line[pos..], "[[id:")) |off| {
        const start = pos + off + 5;
        const end = std.mem.indexOfScalarPos(u8, line, start, ']') orelse break;
        const id = line[start..end];
        try pending.append(.{ .src = try allocator.dupe(u8, src), .dst = try allocator.dupe(u8, id), .kind = .id_link, .path = try allocator.dupe(u8, rel), .line = line_no });
        pos = end + 1;
    }
}

fn resolvePending(ctx: *model.Context, pending: *std.array_list.Managed(PendingLink)) !void {
    for (pending.items, 0..) |p, i| {
        if (ctx.findObject(p.src) == null or ctx.findObject(p.dst) == null) continue;
        const id = try std.fmt.allocPrint(ctx.allocator, "{s}:{s}:{s}:{d}", .{ model.Context.edgeName(p.kind), p.src, p.dst, i });
        defer ctx.allocator.free(id);
        _ = try ctx.addEdge(.{ .id = id, .kind = p.kind, .src = p.src, .dst = p.dst, .label = model.Context.edgeName(p.kind), .path = p.path, .line = p.line });
    }
}

fn loadCategoryTables(allocator: std.mem.Allocator, ctx: *model.Context, root_path: []const u8) !void {
    const direct = try std.fs.path.join(allocator, &.{ root_path, "category" });
    defer allocator.free(direct);
    try loadObjectsTsv(allocator, ctx, direct);
    try loadMorphismsTsv(allocator, ctx, direct);
    const nested = try std.fs.path.join(allocator, &.{ root_path, "context", "category" });
    defer allocator.free(nested);
    try loadObjectsTsv(allocator, ctx, nested);
    try loadMorphismsTsv(allocator, ctx, nested);
}

fn loadObjectsTsv(allocator: std.mem.Allocator, ctx: *model.Context, cat_dir: []const u8) !void {
    const p = try std.fs.path.join(allocator, &.{ cat_dir, "objects.tsv" });
    defer allocator.free(p);
    const bytes = std.fs.cwd().readFileAlloc(p, allocator, @enumFromInt(2 * 1024 * 1024)) catch return;
    defer allocator.free(bytes);
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    var first = true;
    while (lines.next()) |line| {
        if (first) { first = false; continue; }
        if (line.len == 0) continue;
        var cols = std.mem.splitScalar(u8, line, '\t');
        const id = cols.next() orelse continue;
        const kind = cols.next() orelse "Concept";
        const title = cols.next() orelse id;
        _ = try ctx.addObject(.{ .id = id, .kind = model.Context.parseObjectKind(kind), .title = title, .path = "category/objects.tsv", .line = 1, .preview = "declared category object" });
    }
}

fn loadMorphismsTsv(allocator: std.mem.Allocator, ctx: *model.Context, cat_dir: []const u8) !void {
    const p = try std.fs.path.join(allocator, &.{ cat_dir, "morphisms.tsv" });
    defer allocator.free(p);
    const bytes = std.fs.cwd().readFileAlloc(p, allocator, @enumFromInt(2 * 1024 * 1024)) catch return;
    defer allocator.free(bytes);
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    var first = true;
    var n: usize = 0;
    while (lines.next()) |line| : (n += 1) {
        if (first) { first = false; continue; }
        if (line.len == 0) continue;
        var cols = std.mem.splitScalar(u8, line, '\t');
        const id = cols.next() orelse continue;
        const kind = cols.next() orelse "unknown";
        const src = cols.next() orelse continue;
        const dst = cols.next() orelse continue;
        const label = cols.next() orelse kind;
        if (ctx.findObject(src) == null or ctx.findObject(dst) == null) continue;
        _ = try ctx.addEdge(.{ .id = id, .kind = model.Context.parseEdgeKind(kind), .src = src, .dst = dst, .label = label, .path = "category/morphisms.tsv", .line = n + 1 });
    }
}

fn isHeading(line: []const u8) bool {
    return (line.len > 1 and line[0] == '*' and line[1] == ' ') or (line.len > 2 and line[0] == '*' and line[1] == '*');
}

fn trimHeading(line: []const u8) []const u8 {
    var i: usize = 0;
    while (i < line.len and line[i] == '*') i += 1;
    return std.mem.trim(u8, line[i..], " \t");
}

fn extractRootId(bytes: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r");
        if (line.len != 0 and line[0] == '*') return null;
        if (std.mem.startsWith(u8, line, ":ID:")) return std.mem.trim(u8, line[4..], " \t");
    }
    return null;
}

fn extractCleanPreview(allocator: std.mem.Allocator, bytes: []const u8, fallback: []const u8) ![]const u8 {
    var out = std.array_list.Managed(u8).init(allocator);
    defer out.deinit();
    var in_src = false;
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw| {
        var line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0) continue;
        if (std.mem.startsWith(u8, line, "#+BEGIN_SRC") or std.mem.startsWith(u8, line, "#+begin_src")) { in_src = true; continue; }
        if (std.mem.startsWith(u8, line, "#+END_SRC") or std.mem.startsWith(u8, line, "#+end_src")) { in_src = false; continue; }
        if (in_src) continue;
        if (line[0] == ':' or std.mem.startsWith(u8, line, "#+") or std.mem.startsWith(u8, line, "[[file:")) continue;
        if (line[0] == '|' or line[0] == '-' or line[0] == '*') continue;
        line = stripOrgMarkup(line);
        if (line.len == 0) continue;
        if (out.items.len != 0) try out.append(' ');
        try out.appendSlice(line);
        if (out.items.len > 520) break;
    }
    if (out.items.len == 0) return allocator.dupe(u8, fallback);
    return allocator.dupe(u8, out.items[0..@min(out.items.len, 520)]);
}

fn stripOrgMarkup(line: []const u8) []const u8 {
    var out = std.mem.trim(u8, line, " \t");
    if (std.mem.startsWith(u8, out, "- ")) out = std.mem.trim(u8, out[2..], " \t");
    return out;
}

fn extractTitle(bytes: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "#+TITLE:")) return std.mem.trim(u8, line[8..], " \t");
    }
    return null;
}

fn extractTags(bytes: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "#+FILETAGS:")) return std.mem.trim(u8, line[11..], " \t");
    }
    return null;
}

test "function define lines expose first-class signatures" {
    const a = parseWispDefine("id :: a -> a").?;
    try std.testing.expect(std.mem.eql(u8, a.name, "id"));
    try std.testing.expect(std.mem.eql(u8, a.signature, "a -> a"));
    const b = parseLispDefine("(id x -> a) x)").?;
    try std.testing.expect(std.mem.eql(u8, b.name, "id"));
    try std.testing.expect(std.mem.indexOf(u8, b.signature, "->") != null);
    const c = parseLispDefine("[id :: a -> a] x -> x)").?;
    try std.testing.expect(std.mem.eql(u8, c.name, "id"));
}

test "heading trim" {
    try std.testing.expect(std.mem.eql(u8, trimHeading("*** Hello"), "Hello"));
}

test "info org root id and clean preview are extracted" {
    const bytes = "#+TITLE: Advanced Types\n#+FILETAGS: :monad:info:\n:PROPERTIES:\n:ID:       monadc.info.advanced-types\n:END:\n[[file:index.org][Back]]\nMonadC type system overview.\n#+BEGIN_SRC monad\n(noisy)\n#+END_SRC\n* Dedicated Pages\nMore text.";
    try std.testing.expect(std.mem.eql(u8, extractRootId(bytes).?, "monadc.info.advanced-types"));
    const prev = try extractCleanPreview(std.testing.allocator, bytes, "fallback");
    defer std.testing.allocator.free(prev);
    try std.testing.expect(std.mem.indexOf(u8, prev, "MonadC type system overview") != null);
    try std.testing.expect(std.mem.indexOf(u8, prev, "#+") == null);
}
