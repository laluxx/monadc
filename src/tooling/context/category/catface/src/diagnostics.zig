const std = @import("std");
const fmtbuf = @import("fmtbuf.zig");
const model = @import("model.zig");
const math = @import("math.zig");
const graph = @import("graph.zig");

pub const Severity = enum { info, warning, error_ };

pub const Diagnostic = struct {
    severity: Severity,
    message: []const u8,
    object_id: []const u8 = "",
    path: []const u8 = "",
    line: usize = 0,
};

pub const Report = struct {
    allocator: std.mem.Allocator,
    diagnostics: std.array_list.Managed(Diagnostic),

    pub fn init(allocator: std.mem.Allocator) Report {
        return .{ .allocator = allocator, .diagnostics = std.array_list.Managed(Diagnostic).init(allocator) };
    }

    pub fn deinit(self: *Report) void {
        for (self.diagnostics.items) |d| {
            self.allocator.free(d.message);
            self.allocator.free(d.object_id);
            self.allocator.free(d.path);
        }
        self.diagnostics.deinit();
    }

    pub fn add(self: *Report, severity: Severity, message: []const u8, object_id: []const u8, path: []const u8, line: usize) !void {
        try self.diagnostics.append(.{
            .severity = severity,
            .message = try self.allocator.dupe(u8, message),
            .object_id = try self.allocator.dupe(u8, object_id),
            .path = try self.allocator.dupe(u8, path),
            .line = line,
        });
    }

    pub fn hasErrors(self: *const Report) bool {
        for (self.diagnostics.items) |d| {
            if (d.severity == .error_) return true;
        }
        return false;
    }
};

pub fn analyze(allocator: std.mem.Allocator, ctx: *const model.Context) !Report {
    var r = Report.init(allocator);
    const cr = math.checkCategory(ctx);
    if (cr.unresolved_edges != 0) try r.add(.error_, "unresolved generating morphisms", "", "", 0);
    for (ctx.objects.items, 0..) |o, i| {
        if (o.id.len == 0) try r.add(.error_, "empty object id", o.id, o.path, o.line);
        if (o.kind == .unknown) try r.add(.warning, "unknown object kind", o.id, o.path, o.line);
        const deg = graph.degree(ctx, i);
        if (deg.incoming == 0 and deg.outgoing == 0 and o.kind != .file) try r.add(.info, "isolated object", o.id, o.path, o.line);
        if (o.preview.len > 900) try r.add(.warning, "preview too large for card", o.id, o.path, o.line);
    }
    return r;
}

pub fn writeReport(buf: *std.array_list.Managed(u8), report: *const Report) !void {
    for (report.diagnostics.items) |d| {
        const sev = switch (d.severity) { .info => "info", .warning => "warning", .error_ => "error" };
        try fmtbuf.print(buf, "{s}:{d}: {s}: {s}", .{ d.path, d.line, sev, d.message });
        if (d.object_id.len != 0) try fmtbuf.print(buf, " [{s}]", .{d.object_id});
        try buf.append('\n');
    }
}

test "empty report" {
    var r = Report.init(std.testing.allocator);
    defer r.deinit();
    try std.testing.expect(!r.hasErrors());
}
