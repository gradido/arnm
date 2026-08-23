const std = @import("std");
const zcc = @import("compile_commands");

/// Warning flags every first-party C source is compiled with.
///
/// -Wconversion earns its place here because this library narrows on purpose and often:
/// element_size into a uint16_t field, element counts into bucket counts, bucket counts into
/// index slots. A silent narrowing is exactly how those turn into wrong results instead of
/// ARNM_ERROR_ARITHMETIC_OVERFLOW, so every remaining one is meant to carry an explicit cast
/// next to the check that bounds it -- and anything without that check is a bug worth hearing
/// about. The googletest sources are exempt: their macros warn under this flag and they are
/// not ours to fix. CMakeLists.txt mirrors this, MSVC spelling included.
///
/// Where these actually become visible is not obvious: `zig build` drops C compiler warnings
/// entirely when a step succeeds, and relabels them as errors inside its diagnostic bundle when
/// one fails -- so a green `zig build` says nothing about them either way. They surface in the
/// editor, because the cdb step copies these flags into compile_commands.json, and in the CMake
/// build, which prints them as the warnings they are. To check a single file by hand:
///
///   zig cc -std=c11 -Wconversion -Iinclude -c src/<file>.c -o /dev/null
///
/// -c and not -fsyntax-only: zig 0.15.1 answers the latter with "error: FileNotFound" no matter
/// what it is handed, so the diagnostics arrive but the exit code is useless.
const c_warning_flags = [_][]const u8{"-Wconversion"};

/// Recursively add .c files from a directory
fn addDirSources(
    lib: *std.Build.Step.Compile,
    b: *std.Build,
    dir_path: []const u8,
) void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.panic("Failed to open directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close();

    var walker = dir.walk(b.allocator) catch |err| {
        std.debug.panic("Failed to walk directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer walker.deinit();

    while (walker.next() catch null) |entry| {
        if (entry.kind == .file and std.mem.endsWith(u8, entry.path, ".c")) {
            const full_path = b.fmt("{s}/{s}", .{ dir_path, entry.path });
            lib.addCSourceFiles(.{
                .files = &[_][]const u8{full_path},
                .flags = &c_warning_flags,
            });
        }
    }
}

/// Sanitizers the zig toolchain can apply to the C/C++ sources of this project.
///
/// AddressSanitizer is deliberately absent: zig does not ship the asan runtime, so
/// `-fsanitize=address` would compile but fail to link.
const SanitizeMode = enum {
    /// no instrumentation
    off,
    /// UndefinedBehaviorSanitizer, aborting with a diagnostic on the first finding
    undefined_behavior,
    /// ThreadSanitizer, reporting data races between threads
    thread,
};

/// Instrument a module according to @p mode. Applied to every target of the build so that
/// library and test binary always agree on their instrumentation.
fn applySanitize(module: *std.Build.Module, mode: SanitizeMode) void {
    switch (mode) {
        .off => {},
        .undefined_behavior => module.sanitize_c = .full,
        .thread => module.sanitize_thread = true,
    }
}

const BuildContext = struct {
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    core_lib: *std.Build.Step.Compile,
    googletest_dep: ?*std.Build.Dependency,
    singleOutputDir: bool,
    sanitize: SanitizeMode,
    cdb: *std.ArrayList(*std.Build.Step.Compile),
};

const BuildTarget = struct {
    link_googletest: bool = false,
    name: []const u8,
    srcs: []const []const u8,
};

fn processBuildTarget(context: *const BuildContext, build_target: BuildTarget, path: []const u8) void {
    const b = context.b;
    const exe = b.addExecutable(.{
        .name = build_target.name,
        .root_module = b.createModule(.{
            .target = context.target,
            .optimize = context.optimize,
        }),
    });

    applySanitize(exe.root_module, context.sanitize);
    exe.linkLibrary(context.core_lib);

    if (build_target.link_googletest) {
        if (context.googletest_dep) |dep| {
            exe.linkLibrary(dep.artifact("gtest"));
            exe.linkLibrary(dep.artifact("gtest_main"));
        }
    }

    exe.addIncludePath(b.path("include"));
    // the white box tests read the layout behind the opaque arnm from src/memory_intern.h,
    // which is not installed and is on no consumer's path -- mirrors tests/CMakeLists.txt
    if (build_target.link_googletest) {
        exe.addIncludePath(b.path("src"));
    }

    for (build_target.srcs) |src_file| {
        exe.addCSourceFiles(.{
            .files = &.{b.fmt("{s}/{s}", .{ path, src_file })},
            // benchmarks are ours and get the flags; the googletest translation units do not
            .flags = if (build_target.link_googletest) &[_][]const u8{} else &c_warning_flags,
        });
    }

    context.cdb.append(b.allocator, exe) catch @panic("OOM");

    if (context.singleOutputDir) {
        const bin_install_step = b.addInstallBinFile(exe.getEmittedBin(), b.fmt("../{s}", .{exe.out_filename}));
        b.getInstallStep().dependOn(&bin_install_step.step);
    } else {
        b.installArtifact(exe);
    }
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    // make a list of targets that have include files and c source files
    var cdbTargets: std.ArrayList(*std.Build.Step.Compile) = .empty;

    // Options
    const enable_benchmarks = b.option(bool, "benchmarks", "Enable benchmarks") orelse false;
    const enable_tests = b.option(bool, "tests", "Enable tests") orelse false;
    const lib_shared = b.option(bool, "shared", "Make lib shared") orelse false;
    const singleOutputDir = b.option(bool, "singleOutputDir", "Put direct into output folder, without lib or bin folder") orelse false;
    const sanitize = b.option(SanitizeMode, "sanitize", "Instrument C sources: undefined_behavior (UBSan) or thread (TSan). AddressSanitizer is not available through zig") orelse .off;

    const core_lib = b.addLibrary(.{ .name = "arnm", .linkage = if (lib_shared) .dynamic else .static, .root_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    }) });
    applySanitize(core_lib.root_module, sanitize);

    const context: BuildContext = .{
        .b = b,
        .target = target,
        .optimize = optimize,
        .core_lib = core_lib,
        .googletest_dep = b.lazyDependency("googletest", .{
            .target = target,
            .optimize = optimize,
        }),
        .singleOutputDir = singleOutputDir,
        .sanitize = sanitize,
        .cdb = &cdbTargets,
    };

    core_lib.linkLibC();
    core_lib.addIncludePath(b.path("include"));
    core_lib.installHeadersDirectory(b.path("include/arnm"), "arnm", .{});

    addDirSources(core_lib, b, "src");

    // keep track of it, so later we can pass it to compile_commands
    cdbTargets.append(b.allocator, core_lib) catch @panic("OOM");

    if (singleOutputDir) {
        const bin_install_step = b.addInstallBinFile(core_lib.getEmittedBin(), b.fmt("../{s}", .{core_lib.out_filename}));
        b.getInstallStep().dependOn(&bin_install_step.step);
        if (target.result.os.tag == .windows) {
            const lib_install_step = b.addInstallLibFile(core_lib.getEmittedImplib(), b.fmt("../{s}", .{core_lib.out_lib_filename}));
            b.getInstallStep().dependOn(&lib_install_step.step);
        }
    } else {
        b.installArtifact(core_lib);
    }

    if (enable_benchmarks) {
        const path = "benchmarks/src";
        processBuildTarget(&context, .{ .name = "bench_bucket_vector", .srcs = &.{"bench_bucket_vector.c"} }, path);
        processBuildTarget(&context, .{ .name = "bench_multi_arena", .srcs = &.{"bench_multi_arena.c"} }, path);
        processBuildTarget(&context, .{ .name = "bench_numberToString", .srcs = &.{"bench_numberToString.c"} }, path);
        processBuildTarget(&context, .{ .name = "bench_binaryToString", .srcs = &.{"bench_binaryToString.c"} }, path);
    }

    if (enable_tests) {
        const path = "tests/unit/src";
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_bucket_vector", .srcs = &.{"test_bucket_vector.cpp"} }, path);
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_converter", .srcs = &.{"test_converter.cpp"} }, path);
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_duration", .srcs = &.{"test_duration.cpp"} }, path);
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_memory", .srcs = &.{"test_memory.cpp"} }, path);
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_fixed_arena_pool", .srcs = &.{"test_fixed_arena_pool.cpp"} }, path);
        processBuildTarget(&context, .{ .link_googletest = true, .name = "test_multi_arena", .srcs = &.{"test_multi_arena.cpp"} }, path);
    }

    const cdbTargetsSlice = cdbTargets.toOwnedSlice(b.allocator) catch @panic("OOM");
    const buildStep = zcc.createStep(b, "cdb", cdbTargetsSlice);
    // Build everything in the project before generating the compile_commands
    for (cdbTargetsSlice) |cdbTarget| buildStep.dependOn(&cdbTarget.step);
    // Regenerate compile_commands.json on every build, not only on `zig build cdb`
    b.getInstallStep().dependOn(buildStep);
}
