const std = @import("std");
const model = @import("model.zig");

pub const Direction = enum { out, in };

pub const Group = struct {
    dir: Direction,
    kind: model.EdgeKind,
};

pub const Action = union(enum) {
    none,
    select: usize,
    toggle_dir: Direction,
    toggle_group: Group,
};

pub const edge_kinds = [_]model.EdgeKind{
    .verifies,
    .supports,
    .blocks,
    .refines,
    .mentions,
    .contains,
    .id_link,
    .file_link,
    .supersedes,
    .generated_by,
    .classifies_as,
    .forgets_to,
    .unknown,
};

pub const EdgeKindCount = edge_kinds.len;

pub const State = struct {
    out_open: bool = true,
    in_open: bool = true,
    out_groups: [EdgeKindCount]bool = [_]bool{true} ** EdgeKindCount,
    in_groups: [EdgeKindCount]bool = [_]bool{true} ** EdgeKindCount,
    scroll: usize = 0,
    cursor: usize = 0,
    row_count: usize = 0,
    view_rows: usize = 1,

    pub fn init() State {
        return .{};
    }

    pub fn dirOpen(self: *const State, dir: Direction) bool {
        return switch (dir) {
            .out => self.out_open,
            .in => self.in_open,
        };
    }

    pub fn setDirOpen(self: *State, dir: Direction, open: bool) void {
        switch (dir) {
            .out => self.out_open = open,
            .in => self.in_open = open,
        }
    }

    pub fn toggleDir(self: *State, dir: Direction) void {
        self.setDirOpen(dir, !self.dirOpen(dir));
    }

    pub fn groupOpen(self: *const State, dir: Direction, kind: model.EdgeKind) bool {
        const idx = edgeIndex(kind);
        return switch (dir) {
            .out => self.out_groups[idx],
            .in => self.in_groups[idx],
        };
    }

    pub fn setGroupOpen(self: *State, dir: Direction, kind: model.EdgeKind, open: bool) void {
        const idx = edgeIndex(kind);
        switch (dir) {
            .out => self.out_groups[idx] = open,
            .in => self.in_groups[idx] = open,
        }
    }

    pub fn toggleGroup(self: *State, dir: Direction, kind: model.EdgeKind) void {
        self.setGroupOpen(dir, kind, !self.groupOpen(dir, kind));
    }

    pub fn apply(self: *State, action: Action) void {
        switch (action) {
            .toggle_dir => |dir| self.toggleDir(dir),
            .toggle_group => |g| self.toggleGroup(g.dir, g.kind),
            else => {},
        }
    }

    pub fn sync(self: *State, row_count: usize, view_rows: usize) void {
        self.row_count = row_count;
        self.view_rows = if (view_rows == 0) 1 else view_rows;
        if (self.row_count == 0) {
            self.cursor = 0;
            self.scroll = 0;
            return;
        }
        if (self.cursor >= self.row_count) self.cursor = self.row_count - 1;
        self.ensureCursorVisible();
    }

    pub fn moveCursor(self: *State, delta: isize) void {
        if (self.row_count == 0) return;
        var next: isize = @intCast(self.cursor);
        next += delta;
        if (next < 0) next = 0;
        const max: isize = @intCast(self.row_count - 1);
        if (next > max) next = max;
        self.cursor = @intCast(next);
        self.ensureCursorVisible();
    }

    pub fn scrollBy(self: *State, delta: isize) void {
        if (self.row_count == 0) return;
        const max_scroll = if (self.row_count > self.view_rows) self.row_count - self.view_rows else 0;
        if (delta < 0) {
            const amount: usize = @intCast(-delta);
            self.scroll = if (amount > self.scroll) 0 else self.scroll - amount;
        } else if (delta > 0) {
            const amount: usize = @intCast(delta);
            self.scroll = @min(max_scroll, self.scroll + amount);
        }
        if (self.cursor < self.scroll) self.cursor = self.scroll;
        if (self.cursor >= self.scroll + self.view_rows) self.cursor = @min(self.row_count - 1, self.scroll + self.view_rows - 1);
    }

    pub fn ensureCursorVisible(self: *State) void {
        if (self.row_count == 0) {
            self.scroll = 0;
            return;
        }
        if (self.cursor < self.scroll) self.scroll = self.cursor;
        if (self.cursor >= self.scroll + self.view_rows) self.scroll = self.cursor - self.view_rows + 1;
        const max_scroll = if (self.row_count > self.view_rows) self.row_count - self.view_rows else 0;
        if (self.scroll > max_scroll) self.scroll = max_scroll;
    }
};

pub fn edgeIndex(kind: model.EdgeKind) usize {
    for (edge_kinds, 0..) |k, i| {
        if (k == kind) return i;
    }
    return edge_kinds.len - 1;
}

pub fn directionName(dir: Direction) []const u8 {
    return switch (dir) {
        .out => "OUT",
        .in => "IN",
    };
}

pub fn directionSignature(dir: Direction) []const u8 {
    return switch (dir) {
        .out => "Hom(object, -)",
        .in => "Hom(-, object)",
    };
}

test "relation tree state toggles directions and groups" {
    var s = State.init();
    try std.testing.expect(s.dirOpen(.out));
    s.toggleDir(.out);
    try std.testing.expect(!s.dirOpen(.out));
    try std.testing.expect(s.groupOpen(.in, .verifies));
    s.toggleGroup(.in, .verifies);
    try std.testing.expect(!s.groupOpen(.in, .verifies));
}

test "relation tree cursor clamps and follows viewport" {
    var s = State.init();
    s.sync(20, 5);
    s.moveCursor(9);
    try std.testing.expect(s.cursor == 9);
    try std.testing.expect(s.scroll == 5);
    s.moveCursor(-99);
    try std.testing.expect(s.cursor == 0);
    try std.testing.expect(s.scroll == 0);
    s.scrollBy(100);
    try std.testing.expect(s.scroll == 15);
}

