const std = @import("std");

pub const ObjectKind = enum {
    file,
    heading,
    record,
    script,
    report,
    concept,
    test_kind,
    source,
    info,
    todo,
    done,
    unknown,
};

pub const EdgeKind = enum {
    contains,
    file_link,
    id_link,
    supports,
    supersedes,
    verifies,
    blocks,
    refines,
    classifies_as,
    forgets_to,
    generated_by,
    mentions,
    unknown,
};

pub const Object = struct {
    id: []const u8,
    kind: ObjectKind,
    title: []const u8 = "",
    path: []const u8 = "",
    line: usize = 0,
    tags: []const u8 = "",
    preview: []const u8 = "",
    weight: i32 = 0,
};

pub const Edge = struct {
    id: []const u8,
    kind: EdgeKind,
    src: []const u8,
    dst: []const u8,
    label: []const u8 = "",
    path: []const u8 = "",
    line: usize = 0,
};

pub const ObjectRef = struct {
    index: usize,
    score: i32,
};

pub const EdgeRef = struct {
    index: usize,
    score: i32,
};

pub const Context = struct {
    allocator: std.mem.Allocator,
    root: []const u8,
    objects: std.array_list.Managed(Object),
    edges: std.array_list.Managed(Edge),
    id_to_index: std.StringHashMap(usize),
    edge_id_to_index: std.StringHashMap(usize),

    pub fn init(allocator: std.mem.Allocator, root: []const u8) !Context {
        return .{
            .allocator = allocator,
            .root = try allocator.dupe(u8, root),
            .objects = std.array_list.Managed(Object).init(allocator),
            .edges = std.array_list.Managed(Edge).init(allocator),
            .id_to_index = std.StringHashMap(usize).init(allocator),
            .edge_id_to_index = std.StringHashMap(usize).init(allocator),
        };
    }

    pub fn deinit(self: *Context) void {
        for (self.objects.items) |obj| {
            self.allocator.free(obj.id);
            self.allocator.free(obj.title);
            self.allocator.free(obj.path);
            self.allocator.free(obj.tags);
            self.allocator.free(obj.preview);
        }
        for (self.edges.items) |edge| {
            self.allocator.free(edge.id);
            self.allocator.free(edge.src);
            self.allocator.free(edge.dst);
            self.allocator.free(edge.label);
            self.allocator.free(edge.path);
        }
        self.objects.deinit();
        self.edges.deinit();
        self.id_to_index.deinit();
        self.edge_id_to_index.deinit();
        self.allocator.free(self.root);
    }

    pub fn addObject(self: *Context, obj: Object) !usize {
        if (self.id_to_index.get(obj.id)) |idx| return idx;
        const idx = self.objects.items.len;
        const owned = Object{
            .id = try self.allocator.dupe(u8, obj.id),
            .kind = obj.kind,
            .title = try self.allocator.dupe(u8, obj.title),
            .path = try self.allocator.dupe(u8, obj.path),
            .line = obj.line,
            .tags = try self.allocator.dupe(u8, obj.tags),
            .preview = try self.allocator.dupe(u8, obj.preview),
            .weight = obj.weight,
        };
        try self.objects.append(owned);
        try self.id_to_index.put(owned.id, idx);
        return idx;
    }

    pub fn addEdge(self: *Context, edge: Edge) !usize {
        if (self.edge_id_to_index.get(edge.id)) |idx| return idx;
        const idx = self.edges.items.len;
        const owned = Edge{
            .id = try self.allocator.dupe(u8, edge.id),
            .kind = edge.kind,
            .src = try self.allocator.dupe(u8, edge.src),
            .dst = try self.allocator.dupe(u8, edge.dst),
            .label = try self.allocator.dupe(u8, edge.label),
            .path = try self.allocator.dupe(u8, edge.path),
            .line = edge.line,
        };
        try self.edges.append(owned);
        try self.edge_id_to_index.put(owned.id, idx);
        return idx;
    }

    pub fn findObject(self: *const Context, id: []const u8) ?usize {
        return self.id_to_index.get(id);
    }

    pub fn findEdge(self: *const Context, id: []const u8) ?usize {
        return self.edge_id_to_index.get(id);
    }

    pub fn outgoingInto(self: *const Context, allocator: std.mem.Allocator, id: []const u8) !std.array_list.Managed(EdgeRef) {
        var out = std.array_list.Managed(EdgeRef).init(allocator);
        for (self.edges.items, 0..) |e, i| {
            if (std.mem.eql(u8, e.src, id)) {
                try out.append(.{ .index = i, .score = 0 });
            }
        }
        return out;
    }

    pub fn incomingInto(self: *const Context, allocator: std.mem.Allocator, id: []const u8) !std.array_list.Managed(EdgeRef) {
        var out = std.array_list.Managed(EdgeRef).init(allocator);
        for (self.edges.items, 0..) |e, i| {
            if (std.mem.eql(u8, e.dst, id)) {
                try out.append(.{ .index = i, .score = 0 });
            }
        }
        return out;
    }

    pub fn objectPath(self: *const Context, idx: usize) []const u8 {
        return self.objects.items[idx].path;
    }

    pub fn kindName(k: ObjectKind) []const u8 {
        return switch (k) {
            .file => "File",
            .heading => "Heading",
            .record => "Record",
            .script => "Script",
            .report => "Report",
            .concept => "Concept",
            .test_kind => "Test",
            .source => "Source",
            .info => "Info",
            .todo => "Todo",
            .done => "Done",
            .unknown => "Unknown",
        };
    }

    pub fn edgeName(k: EdgeKind) []const u8 {
        return switch (k) {
            .contains => "contains",
            .file_link => "file-link",
            .id_link => "id-link",
            .supports => "supports",
            .supersedes => "supersedes",
            .verifies => "verifies",
            .blocks => "blocks",
            .refines => "refines",
            .classifies_as => "classifies-as",
            .forgets_to => "forgets-to",
            .generated_by => "generated-by",
            .mentions => "mentions",
            .unknown => "unknown",
        };
    }

    pub fn parseObjectKind(s: []const u8) ObjectKind {
        if (eqlIgnoreCase(s, "File")) return .file;
        if (eqlIgnoreCase(s, "Heading")) return .heading;
        if (eqlIgnoreCase(s, "Record")) return .record;
        if (eqlIgnoreCase(s, "Script")) return .script;
        if (eqlIgnoreCase(s, "Report")) return .report;
        if (eqlIgnoreCase(s, "Concept")) return .concept;
        if (eqlIgnoreCase(s, "Test") or eqlIgnoreCase(s, "Tests")) return .test_kind;
        if (eqlIgnoreCase(s, "Source")) return .source;
        if (eqlIgnoreCase(s, "Info")) return .info;
        if (eqlIgnoreCase(s, "Todo")) return .todo;
        if (eqlIgnoreCase(s, "Done")) return .done;
        return .unknown;
    }

    pub fn parseEdgeKind(s: []const u8) EdgeKind {
        if (eqlDash(s, "contains")) return .contains;
        if (eqlDash(s, "file-link")) return .file_link;
        if (eqlDash(s, "id-link")) return .id_link;
        if (eqlDash(s, "supports")) return .supports;
        if (eqlDash(s, "supersedes")) return .supersedes;
        if (eqlDash(s, "verifies")) return .verifies;
        if (eqlDash(s, "blocks")) return .blocks;
        if (eqlDash(s, "refines")) return .refines;
        if (eqlDash(s, "classifies-as")) return .classifies_as;
        if (eqlDash(s, "forgets-to")) return .forgets_to;
        if (eqlDash(s, "generated-by")) return .generated_by;
        if (eqlDash(s, "mentions")) return .mentions;
        return .unknown;
    }
};

pub fn eqlIgnoreCase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |ca, cb| {
        if (std.ascii.toLower(ca) != std.ascii.toLower(cb)) return false;
    }
    return true;
}

pub fn eqlDash(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |ca, cb| {
        const xa = if (ca == '_') '-' else std.ascii.toLower(ca);
        const xb = if (cb == '_') '-' else std.ascii.toLower(cb);
        if (xa != xb) return false;
    }
    return true;
}

pub fn basename(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |i| return path[i + 1 ..];
    return path;
}

test "kind parser" {
    try std.testing.expect(Context.parseObjectKind("record") == .record);
    try std.testing.expect(Context.parseEdgeKind("classifies_as") == .classifies_as);
}
