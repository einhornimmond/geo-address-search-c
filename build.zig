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

/// One test executable per unit of src/, named after the file it exercises.
/// Adding a unit means adding its test here and writing tests/unit/src/<name>.cpp.
const unit_tests = [_][]const u8{
    "test_client",
    "test_doc_collector",
    "test_error",
    "test_format",
    "test_geo_index",
    "test_house_collector",
    "test_json_parse",
    "test_json_stats",
    "test_line_buffer",
    "test_meta_area_allocator",
    "test_name_collector",
    "test_parse_queue",
    "test_prefix_tree",
    "test_text_tokenize",
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
    const enable_tests = b.option(bool, "tests", "Build the unit tests under tests/unit") orelse false;
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
    try addDirSources(exe, b, "src", c_flags, &.{});

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

    // =====================================================================
    //  Unit tests: one executable per unit of src/, built only on request
    // =====================================================================

    const test_step = b.step("test", "Run unit tests (needs -Dtests=true)");

    if (enable_tests) {
        // Every unit under test, compiled once and shared by all test binaries.
        // main.c stays out — it carries the entry point the tests bring
        // themselves, through gtest_main.
        const test_lib = b.addLibrary(.{
            .name = "geoindex_under_test",
            .linkage = .static,
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
            }),
        });
        test_lib.linkLibC();
        test_lib.root_module.addCMacro("_GNU_SOURCE", "1");
        test_lib.linkLibrary(zstd.artifact("zstd"));
        test_lib.linkLibrary(blockchain_core.artifact("gradido_blockchain_core"));
        test_lib.addIncludePath(blockchain_core.path("include"));
        test_lib.addIncludePath(b.path("third_party/yyjson/src"));
        test_lib.addIncludePath(b.path("third_party/stb"));
        test_lib.addIncludePath(b.path("third_party/CRoaring/include"));
        test_lib.addCSourceFiles(.{
            .root = b.path("third_party/yyjson/src"),
            .files = &.{"yyjson.c"},
            .flags = c_flags,
        });
        test_lib.addCSourceFiles(.{
            .root = b.path("third_party/CRoaring"),
            .files = &.{"roaring.c"},
            .flags = roaring_flags.items,
        });
        try addDirSources(test_lib, b, "src", c_flags, &.{"main.c"});

        cdbTargets.append(b.allocator, test_lib) catch @panic("OOM");

        const googletest = b.lazyDependency("googletest", .{ .target = target, .optimize = optimize });

        for (unit_tests) |name| {
            const t = b.addExecutable(.{
                .name = name,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                }),
            });
            t.linkLibCpp();
            t.linkLibrary(test_lib);
            t.root_module.addCMacro("_GNU_SOURCE", "1");
            t.addIncludePath(b.path("src"));
            t.addIncludePath(b.path("tests/unit/src"));
            t.addIncludePath(blockchain_core.path("include"));
            t.addIncludePath(b.path("third_party/yyjson/src"));
            t.addIncludePath(b.path("third_party/CRoaring/include"));
            // No -cflags here on purpose: Zig appends the resolved target triple
            // to that group, and the glibc-versioned form it produces is one
            // clang++ refuses to parse. The language comes from the extension.
            t.addCSourceFiles(.{
                .root = b.path("tests/unit/src"),
                .files = &.{b.fmt("{s}.cpp", .{name})},
            });
            if (googletest) |dep| {
                t.linkLibrary(dep.artifact("gtest"));
                t.linkLibrary(dep.artifact("gtest_main"));
            }

            // Deliberately not handed to the compile-commands step: it appends
            // a -cflags group carrying Zig's own target triple, and the form
            // that takes for a native build —
            // x86_64-unknown-linux.6.1...6.1-gnu.2.36 — is one clang++ refuses
            // to parse. The C sources under src/ are covered there already, so
            // what the database loses is the test files themselves.
            b.installArtifact(t);

            // The fixtures are addressed relative to the project root, so every
            // test is run from there rather than from the install directory.
            const run_test = b.addRunArtifact(t);
            run_test.setCwd(b.path("."));
            test_step.dependOn(&run_test.step);
        }
    } else {
        const hint = b.addFail("no tests were built — configure with -Dtests=true");
        test_step.dependOn(&hint.step);
    }

    const cdbTargetsSlice = cdbTargets.toOwnedSlice(b.allocator) catch @panic("OOM");
    const buildStep = zcc.createStep(b, "cdb", cdbTargetsSlice);

    buildStep.dependOn(&exe.step);
    b.getInstallStep().dependOn(buildStep);
}

fn addDirSources(
    exe: *std.Build.Step.Compile,
    b: *std.Build,
    root: []const u8,
    flags: []const []const u8,
    skip: []const []const u8,
) !void {
    var dir = try std.fs.cwd().openDir(root, .{ .iterate = true });
    defer dir.close();

    var walker = try dir.walk(b.allocator);
    defer walker.deinit();

    var files: std.ArrayList([]const u8) = .empty;
    defer files.deinit(b.allocator);

    outer: while (try walker.next()) |entry| {
        if (entry.kind != .file)
            continue;

        if (!std.mem.endsWith(u8, entry.path, ".c"))
            continue;

        for (skip) |skipped| {
            if (std.mem.eql(u8, entry.path, skipped)) continue :outer;
        }

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
