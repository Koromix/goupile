// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "../core/libcc/libcc.hh"
#include "build.hh"
#include "compiler.hh"
#include "target.hh"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace RG {

static int RunTarget(const char *target_filename, Span<const char *const> arguments)
{
    LogInfo("Run '%1'", target_filename);
    LogInfo("%!D..--------------------------------------------------%!0");

#ifdef _WIN32
    HeapArray<char> cmd;

    Fmt(&cmd, "\"%1\"", target_filename);

    // Windows command line quoting rules are batshit crazy
    for (const char *arg: arguments) {
        bool quote = strchr(arg, ' ');

        cmd.Append(quote ? " \"" : " ");
        for (Size i = 0; arg[i]; i++) {
            if (strchr("\"", arg[i])) {
                cmd.Append('\\');
            }
            cmd.Append(arg[i]);
        }
        cmd.Append(quote ? "\"" : "");
    }
    cmd.Append(0);

    // XXX: Use ExecuteCommandLine() but it needs to be improved first, because right
    // now it always wants to capture output. And we don't want to do that here.
    STARTUPINFOA startup_info = {};
    PROCESS_INFORMATION process_info;
    if (!CreateProcessA(target_filename, cmd.ptr, nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup_info, &process_info)) {
        LogError("Failed to start process: %1", GetWin32ErrorString());
        return 127;
    }
    RG_DEFER {
        CloseHandle(process_info.hProcess);
        CloseHandle(process_info.hThread);
    };

    DWORD exit_code = 0;

    bool success = (WaitForSingleObject(process_info.hProcess, INFINITE) == WAIT_OBJECT_0) &&
                   GetExitCodeProcess(process_info.hProcess, &exit_code);
    RG_ASSERT(success);

    return (int)exit_code;
#else
    HeapArray<const char *> argv;

    argv.Append(target_filename);
    argv.Append(arguments);
    argv.Append(nullptr);

    execv(target_filename, (char **)argv.ptr);
    LogError("Failed to execute '%1': %2", target_filename, strerror(errno));
    return 127;
#endif
}

static const char *BuildGitVersionString(Allocator *alloc)
{
    RG_ASSERT(alloc);

    HeapArray<char> output(alloc);
    int exit_code;
    if (!ExecuteCommandLine("git log -n1 --pretty=format:%cd_%h --date=format:%Y%m%d.%H%M",
                            {}, Kilobytes(1), &output, &exit_code))
        return nullptr;
    if (exit_code) {
        LogError("Command 'git log' failed");
        return nullptr;
    }

    output.len = TrimStrRight(output.Take()).len;
    output.Append(0);

    return output.TrimAndLeak().ptr;
}

static const CompilerInfo *FindDefaultCompiler()
{
    for (const CompilerInfo &info: Compilers) {
        if (FindExecutableInPath(info.binary))
            return &info;
    }

    return nullptr;
}

int RunBuild(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    HeapArray<const char *> selectors;
    const char *config_filename = nullptr;
    CompilerInfo compiler_info = {};
    BuildSettings build = {};
    int jobs = std::min(GetCoreCount() + 1, RG_ASYNC_MAX_WORKERS + 1);
    bool quiet = false;
    bool verbose = false;
    const char *run_target_name = nullptr;
    Span<const char *> run_arguments = {};
    bool run_here = false;

    const auto print_usage = [=](FILE *fp) {
        const CompilerInfo *default_compiler = FindDefaultCompiler();

        PrintLn(fp,
R"(Usage: %!..+%1 build [options] [target...]
       %1 build [options] --run target [arguments...]%!0

Options:
    %!..+-C, --config <filename>%!0      Set configuration filename
                                 %!D..(default: FelixBuild.ini)%!0
    %!..+-O, --output <directory>%!0     Set output directory
                                 %!D..(default: bin/<toolchain>)%!0

    %!..+-c, --compiler <compiler>%!0    Set compiler, see below
                                 %!D..(default: %2)%!0
    %!..+-m, --mode <mode>%!0            Set build mode, see below
                                 %!D..(default: %3)%!0
    %!..+-f, --features <features>%!0    Compiler features (see below)
    %!..+-e, --environment%!0            Use compiler flags found in environment (CFLAGS, LDFLAGS, etc.)

    %!..+-j, --jobs <count>%!0           Set maximum number of parallel jobs
                                 %!D..(default: %4)%!0
    %!..+-s, --stop_after_error%!0       Continue build after errors
        %!..+--rebuild%!0                Force rebuild all files

    %!..+-q, --quiet%!0                  Hide felix progress statements
    %!..+-v, --verbose%!0                Show detailed build commands
    %!..+-n, --dry_run%!0                Fake command execution

        %!..+--run <target>%!0           Run target after successful build
                                 %!D..(all remaining arguments are passed as-is)%!0
        %!..+--run_here <target>%!0      Same thing, but run from current directory

Supported compilers:)", FelixTarget, default_compiler ? default_compiler->name : "?",
                        CompileModeNames[(int)build.compile_mode], jobs);

        for (const CompilerInfo &info: Compilers) {
            PrintLn(fp, "    %!..+%1%!0 %2", FmtArg(info.name).Pad(28), info.binary);
        }

        PrintLn(fp, R"(
Use %!..+--compiler=<compiler>:<binary>%!0 to specify a custom compiler binary, for example
you can use: %!..+felix --compiler=Clang:clang-11%!0.

Supported compilation modes: %!..+%1%!0
Supported compiler features: %!..+%2%!0)", FmtSpan(CompileModeNames),
                                           FmtSpan(CompileFeatureNames));

        PrintLn(fp, R"(
Felix can also run the following special commands:
    %!..+build%!0                        Build C and C++ projects %!D..(default)%!0
    %!..+pack%!0                         Pack assets to C source file and other formats

For help about those commands, type: %!..+%1 <command> --help%!0)", FelixTarget);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        for (;;) {
            // We need to consume values (target names) as we go because
            // the --run option will break the loop and all remaining
            // arguments will be passed as-is to the target.
            opt.ConsumeNonOptions(&selectors);
            if (!opt.Next())
                break;

            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config", OptionType::Value)) {
                config_filename = opt.current_value;
            } else if (opt.Test("-O", "--output", OptionType::Value)) {
                build.output_directory = opt.current_value;
            } else if (opt.Test("-c", "--compiler", OptionType::Value)) {
                Span<const char> binary;
                Span<const char> name = SplitStr(opt.current_value, ':', &binary);

                const CompilerInfo *info =
                    FindIfPtr(Compilers, [&](const CompilerInfo &info) { return TestStr(info.name, name); });

                if (!info) {
                    LogError("Unknown compiler '%1'", name);
                    return 1;
                }

                compiler_info = *info;
                if (binary.ptr > name.end()) {
                    compiler_info.binary = binary.ptr;
                }
            } else if (opt.Test("-m", "--mode", OptionType::Value)) {
                if (!OptionToEnum(CompileModeNames, opt.current_value, &build.compile_mode)) {
                    LogError("Unknown build mode '%1'", opt.current_value);
                    return 1;
                }
            } else if (opt.Test("-f", "--features", OptionType::Value)) {
                const char *features_str = opt.current_value;

                while (features_str[0]) {
                    Span<const char> part = TrimStr(SplitStr(features_str, ',', &features_str), " ");

                    if (part.len) {
                        CompileFeature feature;
                        if (!OptionToEnum(CompileFeatureNames, part, &feature)) {
                            LogError("Unknown target feature '%1'", part);
                            return 1;
                        }

                        build.features |= 1u << (int)feature;
                    }
                }
            } else if (opt.Test("-e", "--environment")) {
                build.env = true;
            } else if (opt.Test("-j", "--jobs", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &jobs))
                    return 1;
                if (jobs < 1) {
                    LogError("Jobs count cannot be < 1");
                    return 1;
                }
            } else if (opt.Test("-s", "--stop_after_error")) {
                build.stop_after_error = true;
            } else if (opt.Test("--rebuild")) {
                build.rebuild = true;
            } else if (opt.Test("-q", "--quiet")) {
                quiet = true;
            } else if (opt.Test("-v", "--verbose")) {
                verbose = true;
            } else if (opt.Test("-n", "--dry_run")) {
                build.fake = true;
            } else if (opt.Test("--run", OptionType::Value)) {
                run_target_name = opt.current_value;
                break;
            } else if (opt.Test("--run_here", OptionType::Value)) {
                run_target_name = opt.current_value;
                run_here = true;
                break;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        if (run_target_name) {
            selectors.Append(run_target_name);
            run_arguments = opt.GetRemainingArguments();
        }
    }

    if (quiet) {
        SetLogHandler([](LogLevel level, const char *ctx, const char *msg) {
            if (level != LogLevel::Info) {
                DefaultLogHandler(level, ctx, msg);
            }
        });
    }

    // Find supported compiler (if none was specified)
    if (!compiler_info.name) {
        const CompilerInfo *default_compiler = FindDefaultCompiler();

        if (default_compiler) {
            compiler_info = *default_compiler;
        } else {
            LogError("Could not find any supported compiler in PATH");
            return 1;
        }
    }

    // Initialize and check compiler
    std::unique_ptr<const Compiler> compiler = compiler_info.Create();
    if (!compiler)
        return 1;
    if (!compiler->CheckFeatures(build.compile_mode, build.features))
        return 1;
    build.compiler = compiler.get();

    // Root directory
    const char *start_directory = DuplicateString(GetWorkingDirectory(), &temp_alloc).ptr;
    if (config_filename) {
        Span<const char> root_directory;
        config_filename = SplitStrReverseAny(config_filename, RG_PATH_SEPARATORS, &root_directory).ptr;

        if (root_directory.len) {
            const char *root_directory0 = DuplicateString(root_directory, &temp_alloc).ptr;
            if (!SetWorkingDirectory(root_directory0))
                return 1;
        }
    } else {
        config_filename = "FelixBuild.ini";

        // Try to find FelixBuild.ini in current directory and all parent directories. We
        // don't need to handle not finding it anywhere, because in this case the config load
        // will fail with a simple "Cannot open 'FelixBuild.ini'" message.
        for (Size i = 0; start_directory[i]; i++) {
            if (IsPathSeparator(start_directory[i])) {
                if (TestFile(config_filename))
                    break;

                SetWorkingDirectory("..");
            }
        }
    }

    // Output directory
    if (build.output_directory) {
        build.output_directory = NormalizePath(build.output_directory, start_directory, &temp_alloc).ptr;
    } else {
        build.output_directory = Fmt(&temp_alloc, "%1%/bin%/%2_%3", GetWorkingDirectory(),
                                     build.compiler->name, CompileModeNames[(int)build.compile_mode]).ptr;
    }

    // Load configuration file
    TargetSet target_set;
    if (!LoadTargetSet(config_filename, &target_set))
        return 1;
    if (!target_set.targets.len) {
        LogError("Configuration file does not contain any target");
        return 1;
    }

    // Select targets
    HeapArray<const TargetInfo *> enabled_targets;
    HeapArray<const SourceFileInfo *> enabled_sources;
    if (selectors.len) {
        HashSet<const char *> handled_set;

        for (const char *selector: selectors) {
            bool match = false;
            for (const TargetInfo &target: target_set.targets) {
                if (MatchPathSpec(target.name, selector)) {
                    if (handled_set.TrySet(target.name).second) {
                        enabled_targets.Append(&target);
                    }

                    match = true;
                }
            }
            for (const SourceFileInfo &src: target_set.sources) {
                if (MatchPathSpec(src.filename, selector)) {
                    if (handled_set.TrySet(src.filename).second) {
                        enabled_sources.Append(&src);
                    }

                    match = true;
                }
            }

            if (!match) {
                LogError("Selector '%1' does not match anything", selector);
                return 1;
            }
        }
    } else {
        for (const TargetInfo &target: target_set.targets) {
            if (target.enable_by_default) {
                enabled_targets.Append(&target);
            }
        }

        if (!enabled_targets.len) {
            LogError("No target to build by default");
            return 1;
        }
    }

    // Find and check target used with --run
    const TargetInfo *run_target = nullptr;
    if (run_target_name) {
        run_target = target_set.targets_map.FindValue(run_target_name, nullptr);

        if (!run_target) {
            LogError("Run target '%1' does not exist", run_target_name);
            return 1;
        } else if (run_target->type != TargetType::Executable) {
            LogError("Cannot run non-executable target '%1'", run_target->name);
            return 1;
        }
    }

    // Build version string from git commit (date, hash)
    {
        const char *version_str = BuildGitVersionString(&temp_alloc);

        if (version_str) {
            build.version_str = version_str;
        } else {
            LogError("Failed to use git to build version string");
        }
    }

    // We're ready to output stuff
    LogInfo("Root directory: %!..+%1%!0", GetWorkingDirectory());
    LogInfo("  Compiler: %!..+%1 (%2)%!0", build.compiler->name, CompileModeNames[(int)build.compile_mode]);
    LogInfo("  Output directory: %!..+%1%!0", build.output_directory);
    LogInfo("  Version: %!..+%1%!0", build.version_str);
    if (!build.fake && !MakeDirectoryRec(build.output_directory))
        return 1;

    // The detection of SIGINT (or the Win32 equivalent) by WaitForInterrupt() remains after timing out,
    // which will allow RunBuildNodes() to clean up files produced by interrupted commands.
    WaitForInterrupt(0);

    // Build stuff!
    Builder builder(build);
    for (const TargetInfo *target: enabled_targets) {
        if (!builder.AddTarget(*target))
            return 1;
    }
    for (const SourceFileInfo *src: enabled_sources) {
        if (!builder.AddSource(*src))
            return 1;
    }
    if (!builder.Build(jobs, verbose))
        return 1;

    // Run?
    if (run_target) {
        RG_ASSERT(run_target->type == TargetType::Executable);

        if (run_here && !SetWorkingDirectory(start_directory))
            return 1;

        const char *target_filename = builder.target_filenames.FindValue(run_target->name, nullptr);
        return RunTarget(target_filename, run_arguments);
    } else {
        return 0;
    }
}

}
