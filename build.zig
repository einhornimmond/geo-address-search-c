const std = @import("std");

const c_flags = &.{
    "-std=c23",
    "-O3",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
};

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const cjson = b.dependency("cjson", .{});

    const exe = b.addExecutable(.{ .name = "parse_photon_jsonl_dump", .root_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    }) });

    exe.linkLibC();

    // Projektquellen
    try addDirSources(exe, b, "src", c_flags);

    // cJSON
    exe.addIncludePath(cjson.path(""));
    exe.addCSourceFiles(.{
        .root = .{
            .dependency = .{
                .dependency = cjson,
                .sub_path = "",
            },
        },
        .files = &.{
            "cJSON.c",
        },
        .flags = c_flags,
    });

    b.installArtifact(exe);

    // zig build run -- <args>
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the application");
    run_step.dependOn(&run_cmd.step);

    // Platzhalter für spätere Tests
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

    var files = try std.ArrayList([]const u8).initCapacity(b.allocator, 0);
    defer files.deinit(b.allocator);

    while (try walker.next()) |entry| {
        if (entry.kind != .file)
            continue;

        if (!std.mem.endsWith(u8, entry.path, ".c"))
            continue;

        try files.append(b.allocator, try b.allocator.dupe(u8, entry.path));
    }

    std.mem.sort(
        []const u8,
        files.items,
        {},
        struct {
            fn less(_: void, a: []const u8, bLocal: []const u8) bool {
                return std.mem.order(u8, a, bLocal) == .lt;
            }
        }.less,
    );

    exe.addCSourceFiles(.{
        .root = b.path(root),
        .files = files.items,
        .flags = flags,
    });
}
