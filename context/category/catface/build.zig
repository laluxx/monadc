const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    const exe = b.addExecutable(.{
        .name = "catface",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run the Catface category explorer");
    run_step.dependOn(&run_cmd.step);

    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/tests.zig"),
        .target = target,
        .optimize = optimize,
    });
    const unit_tests = b.addTest(.{ .root_module = test_mod });
    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run Catface unit tests");
    test_step.dependOn(&run_unit_tests.step);

    const check_cmd = b.addRunArtifact(exe);
    check_cmd.addArg("--check");
    check_cmd.addArg("../../..");
    const check_step = b.step("check-context", "Load project root and verify category laws");
    check_step.dependOn(&check_cmd.step);

    const perf_cmd = b.addRunArtifact(exe);
    perf_cmd.addArg("--perf");
    perf_cmd.addArg("../../..");
    const perf_step = b.step("perf", "Print structured Catface performance JSONL");
    perf_step.dependOn(&perf_cmd.step);

    const query_report_cmd = b.addRunArtifact(exe);
    query_report_cmd.addArg("--query-report");
    query_report_cmd.addArg("../../..");
    const query_report_step = b.step("query-report", "Print structured Catface query catalogue JSONL");
    query_report_step.dependOn(&query_report_cmd.step);

    const cache_report_cmd = b.addRunArtifact(exe);
    cache_report_cmd.addArg("--cache-report");
    cache_report_cmd.addArg("../../..");
    const cache_report_step = b.step("cache-report", "Print structured Catface persistent cache JSONL");
    cache_report_step.dependOn(&cache_report_cmd.step);

    const test_report_cmd = b.addRunArtifact(exe);
    test_report_cmd.addArg("--test-report");
    test_report_cmd.addArg("../../..");
    const test_report_step = b.step("test-report", "Run tests, then print structured Catface query/performance JSONL");
    test_report_step.dependOn(&run_unit_tests.step);
    test_report_step.dependOn(&test_report_cmd.step);

}
