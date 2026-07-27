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

    // options
    const disable_avx512 = b.option(bool, "ROARING_DISABLE_AVX512", "Disable AVX512 in CRoaring") orelse autoDetectDisableAvx512(target);

    const zstd = b.dependency("zstd", .{ .target = target, .optimize = optimize });
    const blockchain_core = b.dependency("blockchain_core", .{ .target = target, .optimize = optimize });

    const exe = b.addExecutable(.{ .name = "parse_photon_jsonl_dump", .root_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    }) });

    exe.linkLibC();
    exe.linkSystemLibrary("pthread");
    exe.root_module.addCMacro("_GNU_SOURCE", "1");

    // zstd
    exe.linkLibrary(zstd.artifact("zstd"));

    // gradido blockchain core
    exe.linkLibrary(blockchain_core.artifact("gradido_blockchain_core"));
    exe.addIncludePath(blockchain_core.path("include"));

    // yyjson
    exe.addIncludePath(b.path("third_party/yyjson/src"));
    exe.addCSourceFiles(.{
        .root = b.path("third_party/yyjson/src"),
        .files = &.{
            "yyjson.c",
        },
        .flags = c_flags,
    });

    // stb
    exe.addIncludePath(b.path("third_party/stb"));

    // roaring bitmaps
    const lib_flags: []const []const u8 = if (disable_avx512) &[_][]const u8{"-DCROARING_COMPILER_SUPPORTS_AVX512=0"} else &[_][]const u8{};
    var flags_list: std.ArrayList([]const u8) = .empty;
    try flags_list.appendSlice(b.allocator, c_flags);

    // lib_flags hinzufügen (nur wenn sie nicht leer sind)
    if (lib_flags.len > 0) {
        try flags_list.appendSlice(b.allocator, lib_flags);
    }
    exe.addIncludePath(b.path("third_party/CRoaring/include"));
    exe.addCSourceFiles(.{
        .root = b.path("third_party/CRoaring"),
        .files = &.{
            "roaring.c",
        },
        .flags = flags_list.items,
    });

    // Project sources
    try addDirSources(exe, b, "src", c_flags);

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

fn autoDetectDisableAvx512(target: std.Build.ResolvedTarget) bool {
    const cpu_arch = target.result.cpu.arch;

    // AVX512 ist nur auf x86_64 verfügbar
    if (cpu_arch != .x86_64) {
        std.log.info("Non-x86_64 target detected ({s}), disabling AVX512 in CRoaring", .{@tagName(cpu_arch)});
        return true;
    }

    // Für x86_64 prüfen, ob die benötigten Features unterstützt werden
    const cpu_features = target.result.cpu.features;
    const has_avx512f = cpu_features.isEnabled(@intFromEnum(std.Target.x86.Feature.avx512f));
    const has_avx512dq = cpu_features.isEnabled(@intFromEnum(std.Target.x86.Feature.avx512dq));
    const has_avx512bw = cpu_features.isEnabled(@intFromEnum(std.Target.x86.Feature.avx512bw));

    const has_required_avx512 = has_avx512f and has_avx512dq and has_avx512bw;

    if (!has_required_avx512) {
        std.log.info("AVX512 features not detected on x86_64 target CPU, disabling AVX512 in CRoaring", .{});
    } else {
        std.log.info("AVX512 features detected on x86_64 target CPU, enabling AVX512 optimizations", .{});
    }

    return !has_required_avx512;
}
