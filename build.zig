const std = @import("std");
const zcc = @import("zig_compile_commands");

const c_flags = &.{
    "-std=c23",
    "-D_POSIX_C_SOURCE=199309L",
    "-pthread",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
};

/// What the client needs to read an index file — and nothing else.
/// The builder (main, parser, collectors) stays out, as do yyjson and zstd.
const client_sources = [_][]const u8{
    "client.c",
    "geo_index.c",
    "prefix_tree.c",
    "text_tokenize.c",
};

pub fn build(b: *std.Build) !void {
    // A bare `zig build` would compile for "native" and reach into /usr/include,
    // which needs kernel headers this project does not otherwise depend on.
    // Naming the ABI keeps the host's architecture but makes Zig use its own
    // libc headers; -Dtarget=… still overrides it for cross builds.
    const target = b.standardTargetOptions(.{ .default_target = .{ .abi = .gnu } });
    const optimize = b.standardOptimizeOption(.{});
    // make a list of targets that have include files and c source files
    var cdbTargets: std.ArrayList(*std.Build.Step.Compile) = .empty;

    // options
    const disable_avx512 = b.option(bool, "ROARING_DISABLE_AVX512", "Disable AVX512 in CRoaring") orelse autoDetectDisableAvx512(target);
    const shared = b.option(bool, "shared", "Build the client as a shared library (default: static)") orelse false;
    const node_headers = b.option(
        []const u8,
        "node-headers",
        "Directory holding node_api.h (default: bindings/node/node_modules/node-api-headers/include)",
    ) orelse "bindings/node/node_modules/node-api-headers/include";

    const zstd = b.dependency("zstd", .{ .target = target, .optimize = optimize });
    const blockchain_core = b.dependency("blockchain_core", .{ .target = target, .optimize = optimize });

    // CRoaring is compiled by both artifacts, with the same flags
    var roaring_flags: std.ArrayList([]const u8) = .empty;
    try roaring_flags.appendSlice(b.allocator, c_flags);
    if (disable_avx512) {
        try roaring_flags.append(b.allocator, "-DCROARING_COMPILER_SUPPORTS_AVX512=0");
    }

    // =====================================================================
    //  Client library: open and search
    // =====================================================================

    const lib = b.addLibrary(.{
        .name = "geoindex",
        .linkage = if (shared) .dynamic else .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    lib.linkLibC();
    lib.root_module.addCMacro("_GNU_SOURCE", "1");

    // Only the headers of blockchain-core are needed (grd_result), not the library
    lib.addIncludePath(blockchain_core.path("include"));
    lib.addIncludePath(b.path("third_party/CRoaring/include"));
    lib.addCSourceFiles(.{
        .root = b.path("third_party/CRoaring"),
        .files = &.{"roaring.c"},
        .flags = roaring_flags.items,
    });
    lib.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &client_sources,
        .flags = c_flags,
    });
    lib.installHeader(b.path("src/client.h"), "geoindex/client.h");
    // client.h names its enums by inclusion, so they travel with it — the paths
    // stay relative to client.h, and an installed tree resolves them unaided.
    lib.installHeader(b.path("src/types/geo_status.h"), "geoindex/types/geo_status.h");
    lib.installHeader(b.path("src/types/geo_place_kind.h"), "geoindex/types/geo_place_kind.h");

    b.installArtifact(lib);
    cdbTargets.append(b.allocator, lib) catch @panic("OOM");

    // =====================================================================
    //  Node addon: the same client behind a N-API surface
    // =====================================================================

    const addon = b.addLibrary(.{
        .name = "geoindex-node",
        .linkage = .dynamic,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    addon.linkLibC();
    addon.root_module.addCMacro("_GNU_SOURCE", "1");
    addon.root_module.addCMacro("NAPI_VERSION", "8");

    // The N-API symbols are resolved by the Node process that loads the addon,
    // so they stay undefined here. On macOS the linker needs to be told;
    // on Linux and Windows this is the default for a shared library.
    addon.addIncludePath(b.path(node_headers));
    addon.addIncludePath(b.path("src"));
    addon.addIncludePath(blockchain_core.path("include"));
    addon.addIncludePath(b.path("third_party/CRoaring/include"));
    addon.addCSourceFiles(.{
        .root = b.path("third_party/CRoaring"),
        .files = &.{"roaring.c"},
        .flags = roaring_flags.items,
    });
    addon.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &client_sources,
        .flags = c_flags,
    });
    addon.addCSourceFiles(.{
        .root = b.path("bindings/node"),
        .files = &.{"binding.c"},
        .flags = c_flags,
    });

    // Node insists on the .node extension, so the artifact is installed under
    // that name rather than as libgeoindex-node.so.
    const install_addon = b.addInstallFileWithDir(addon.getEmittedBin(), .lib, "geoindex.node");

    const node_step = b.step("node", "Build the Node addon (bindings/node)");
    node_step.dependOn(&install_addon.step);

    // =====================================================================
    //  Builder: turn a dump into an index file
    // =====================================================================

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
    exe.addIncludePath(b.path("third_party/CRoaring/include"));
    exe.addCSourceFiles(.{
        .root = b.path("third_party/CRoaring"),
        .files = &.{
            "roaring.c",
        },
        .flags = roaring_flags.items,
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

    // zig build client — the library alone, without the builder
    const client_step = b.step("client", "Build only the client library");
    client_step.dependOn(&b.addInstallArtifact(lib, .{}).step);

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

    // AVX512 only exists on x86_64
    if (cpu_arch != .x86_64) {
        std.log.info("Non-x86_64 target detected ({s}), disabling AVX512 in CRoaring", .{@tagName(cpu_arch)});
        return true;
    }

    // On x86_64, check whether the required features are supported
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
