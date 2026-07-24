const std = @import("std");
const zcc = @import("zig_compile_commands");

const c_flags = &.{
    "-std=c23",
    "-D_POSIX_C_SOURCE=199309L",
    "-pthread",
    "-march=native",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
};

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    // make a list of targets that have include files and c source files
    var cdbTargets: std.ArrayList(*std.Build.Step.Compile) = .empty;

    const zstd = b.dependency("zstd", .{ .target = target, .optimize = optimize });
    const blockchain_core = b.dependency("blockchain_core", .{ .target = target, .optimize = optimize });

    const exe = b.addExecutable(.{ .name = "parse_photon_jsonl_dump", .root_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    }) });

    exe.linkLibC();
    exe.linkSystemLibrary("pthread");

    // zstd
    exe.linkLibrary(zstd.artifact("zstd"));
    exe.linkLibrary(blockchain_core.artifact("gradido_blockchain_core"));

    // Project sources
    try addDirSources(exe, b, "src", c_flags);

    // gradido blockchain core
    exe.addIncludePath(blockchain_core.path("include"));

    // yyjson
    exe.addIncludePath(b.path("third_party/yyjson/src"));

    // stb
    exe.addIncludePath(b.path("third_party/stb"));
    exe.addCSourceFiles(.{
        .root = b.path("third_party/yyjson/src"),
        .files = &.{
            "yyjson.c",
        },
        .flags = c_flags,
    });
    cdbTargets.append(b.allocator, exe) catch @panic("OOM");

    b.installArtifact(exe);

    // zig build run -- <args>
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the application");
    run_step.dependOn(&run_cmd.step);

    const cdbTargetsSlice = cdbTargets.toOwnedSlice(b.allocator) catch @panic("OOM");
    const buildStep = zcc.createStep(b, "cdb", cdbTargetsSlice);

    buildStep.dependOn(&exe.step);
    b.getInstallStep().dependOn(buildStep);

    // Placeholder for future tests
    _ = b.step("test", "Run unit tests");
}

fn addDirSources(
    exe: *std.Build.Step.Compile,
    b: *std.Build,
    root: []const u8,
    flags: []const []const u8,
) !void {
    var dir = try std.fs.cwd().openDir(root, .{ .iterate = true });
    defer dir.close();

    var walker = try dir.walk(b.allocator);
    defer walker.deinit();

    var files: std.ArrayList([]const u8) = .empty;
    defer files.deinit(b.allocator);

    while (try walker.next()) |entry| {
        if (entry.kind != .file)
            continue;

        if (!std.mem.endsWith(u8, entry.path, ".c"))
            continue;

        try files.append(
            b.allocator,
            b.fmt("{s}", .{entry.path}),
        );
    }

    std.sort.block(
        []const u8,
        files.items,
        {},
        struct {
            fn less(_: void, left: []const u8, right: []const u8) bool {
                return std.mem.order(u8, left, right) == .lt;
            }
        }.less,
    );

    exe.addCSourceFiles(.{
        .root = b.path(root),
        .files = files.items,
        .flags = flags,
    });
}
