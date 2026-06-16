const std = @import("std");

const fmtbuf = @import("fmtbuf.zig");
const terminal = @import("terminal.zig");
const palette = @import("palette.zig");
const glyphs = @import("glyphs.zig");
const version = @import("version.zig");
const tree = @import("tree.zig");
const perf = @import("perf.zig");
const model = @import("model.zig");
const org = @import("org.zig");
const fuzzy = @import("fuzzy.zig");
const query = @import("query.zig");
const math = @import("math.zig");
const render = @import("render.zig");
const cards = @import("cards.zig");
const ui = @import("ui.zig");
const text = @import("text.zig");
const graph = @import("graph.zig");
const functor = @import("functor.zig");
const keymap = @import("keymap.zig");
const inspector = @import("inspector.zig");
const language = @import("language.zig");
const index = @import("index.zig");
const diagnostics = @import("diagnostics.zig");
const session = @import("session.zig");
const exporter = @import("export.zig");
const relation = @import("relation.zig");
const surface = @import("surface.zig");
const command_palette = @import("command_palette.zig");
const law_tests = @import("law_tests.zig");
const manual = @import("manual.zig");
const app = @import("app.zig");
const file_cache = @import("file_cache.zig");
const perf_report = @import("perf_report.zig");
const context_cache = @import("context_cache.zig");

// This file is intentionally a test harness, not a library root. The shipped
// tool is an executable; tests import every module so Zig type-checks them and
// discovers their local test blocks.
test "catface module graph type-checks" {
    _ = std;
    _ = fmtbuf;
    _ = terminal;
    _ = palette;
    _ = glyphs;
    _ = version;
    _ = tree;
    _ = perf;
    _ = model;
    _ = org;
    _ = fuzzy;
    _ = query;
    _ = math;
    _ = render;
    _ = cards;
    _ = ui;
    _ = text;
    _ = graph;
    _ = functor;
    _ = keymap;
    _ = inspector;
    _ = language;
    _ = index;
    _ = diagnostics;
    _ = session;
    _ = exporter;
    _ = relation;
    _ = surface;
    _ = command_palette;
    _ = law_tests;
    _ = manual;
    _ = app;
    _ = file_cache;
    _ = perf_report;
    _ = context_cache;
}
