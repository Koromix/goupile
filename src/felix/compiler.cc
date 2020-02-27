// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "../core/libcc/libcc.hh"
#include "compiler.hh"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace RG {

void MakePackCommand(Span<const char *const> pack_filenames, CompileMode compile_mode,
                     const char *pack_options, const char *dest_filename,
                     Allocator *alloc, BuildCommand *out_cmd)
{
    HeapArray<char> buf;
    buf.allocator = alloc;

    Fmt(&buf, "\"%1\" pack -O \"%2\"", GetApplicationExecutable(), dest_filename);

    switch (compile_mode) {
        case CompileMode::Debug: { Fmt(&buf, " -m SourceMap"); } break;
        case CompileMode::Fast:
        case CompileMode::Release: { Fmt(&buf, " -m RunTransform"); } break;
    }

    if (pack_options) {
        Fmt(&buf, " %1", pack_options);
    }
    for (const char *pack_filename: pack_filenames) {
        Fmt(&buf, " \"%1\"", pack_filename);
    }

    out_cmd->line = buf.Leak();
}

static bool TestBinary(const char *name)
{
    Span<const char> env = getenv("PATH");

    while (env.len) {
#ifdef _WIN32
        Span<const char> path = SplitStr(env, ';', &env);
#else
        Span<const char> path = SplitStr(env, ':', &env);
#endif

        LocalArray<char, 4096> buf;
        buf.len = Fmt(buf.data, "%1%/%2", path, name).len;

#ifdef _WIN32
        static const Span<const char> extensions[] = {".com", ".exe", ".bat", ".cmd"};

        for (Span<const char> ext: extensions) {
            if (RG_LIKELY(ext.len < buf.Available())) {
                memcpy(buf.end(), ext.ptr, ext.len + 1);

                if (TestFile(buf.data))
                    return true;
            }
        }
#else
        if (RG_LIKELY(buf.len < RG_SIZE(buf.data) - 1) && TestFile(buf.data))
            return true;
#endif
    }

    return false;
}

bool Compiler::Test() const
{
    if (!test_init) {
        test = TestBinary(binary);
        test_init = true;
    }

    return test;
}

static void AppendGccObjectArguments(const char *src_filename, CompileMode compile_mode,
                                     const char *pch_filename, Span<const char *const> definitions,
                                     Span<const char *const> include_directories,
                                     const char *deps_filename, HeapArray<char> *out_buf)
{
    if (LogUsesTerminalOutput()) {
        Fmt(out_buf, " -fdiagnostics-color=always");
    }

    switch (compile_mode) {
        case CompileMode::Debug: { Fmt(out_buf, " -O0 -g -ftrapv"); } break;
        case CompileMode::Fast: { Fmt(out_buf, " -O2 -g -DNDEBUG"); } break;
        case CompileMode::Release: { Fmt(out_buf, " -O2 -flto -DNDEBUG"); } break;
    }

    Fmt(out_buf, " -c \"%1\"", src_filename);
    if (pch_filename) {
        Fmt(out_buf, " -include \"%1\"", pch_filename);
    }
    for (const char *definition: definitions) {
        Fmt(out_buf, " -D%1", definition);
    }
    for (const char *include_directory: include_directories) {
        Fmt(out_buf, " -I%1", include_directory);
    }
    if (deps_filename) {
        Fmt(out_buf, " -MD -MF \"%1\"", deps_filename);
    }
}

static bool AppendGccLinkArguments(Span<const char *const> obj_filenames,
                                   Span<const char *const> libraries, HeapArray<char> *out_buf)
{
    if (LogUsesTerminalOutput()) {
        Fmt(out_buf, " -fdiagnostics-color=always");
    }

    for (const char *obj_filename: obj_filenames) {
        Fmt(out_buf, " \"%1\"", obj_filename);
    }
    for (const char *lib: libraries) {
        Fmt(out_buf, " -l%1", lib);
    }

    // Platform libraries
#if defined(_WIN32)
#elif defined(__APPLE__)
    Fmt(out_buf, " -ldl -pthread");
#else
    Fmt(out_buf, " -lrt -ldl -pthread");
#endif

    return true;
}

class ClangCompiler: public Compiler {
public:
    ClangCompiler(const char *name) : Compiler(name, "clang") {}

    void MakePchCommand(const char *pch_filename, SourceType src_type, CompileMode compile_mode,
                        bool warnings, Span<const char *const> definitions,
                        Span<const char *const> include_directories, const char *deps_filename,
                        Allocator *alloc, BuildCommand *out_cmd) const override
    {
        MakeObjectCommand(pch_filename, src_type, compile_mode, warnings, nullptr, definitions,
                          include_directories, nullptr, deps_filename, alloc, out_cmd);
    }

    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, CompileMode compile_mode,
                           bool warnings, const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, const char *dest_filename,
                           const char *deps_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "clang -std=gnu11"); } break;
            case SourceType::CXX: { Fmt(&buf, "clang++ -std=gnu++17"); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header"); } break;
                case SourceType::CXX: { Fmt(&buf, " -x c++-header"); } break;
            }
        }
        out_cmd->rsp_offset = buf.len;

        Fmt(&buf, warnings ? " -Wall" : " -Wno-everything");

        // Platform flags
#if defined(_WIN32)
        Fmt(&buf, " -D_MT -Xclang --dependent-lib=libcmt -Xclang --dependent-lib=oldnames"
                  " -Wno-unknown-warning-option -Wno-unknown-pragmas -Wno-deprecated-declarations"
                  " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DNOMINMAX"
                  " -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE");

        if (src_type == SourceType::CXX) {
            Fmt(&buf, " -Xclang -flto-visibility-public-std");
        }
#elif defined(__APPLE__)
        Fmt(&buf, " -pthread -fPIC");
#else
        Fmt(&buf, " -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
                  " -pthread -fPIC -fstack-protector-strong --param ssp-buffer-size=4");
        if (compile_mode == CompileMode::Fast || compile_mode == CompileMode::Release) {
            Fmt(&buf, " -D_FORTIFY_SOURCE=2");
        }
#endif

        // Common flags (source, definitions, include directories, etc.)
        AppendGccObjectArguments(src_filename, compile_mode, pch_filename, definitions,
                                 include_directories, deps_filename, &buf);

        out_cmd->line = buf.Leak();
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames, CompileMode compile_mode,
                         Span<const char *const> libraries, LinkType link_type,
                         const char *dest_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "clang++"); } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "clang++ -shared"); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (compile_mode == CompileMode::Release) {
            Fmt(&buf, " -flto");
            if (link_type == LinkType::Executable) {
                Fmt(&buf, " -static");
            }
        } else {
            Fmt(&buf, " -g");
        }

        // Platform flags
#if defined(_WIN32)
        Fmt(&buf, " -fuse-ld=lld --rtlib=compiler-rt");
#elif defined(__APPLE__)
#else
        Fmt(&buf, " -Wl,-z,relro,-z,now");
#endif

        // Objects and libraries
        AppendGccLinkArguments(obj_filenames, libraries, &buf);

        out_cmd->line = buf.Leak();
    }
};

class GnuCompiler: public Compiler {
public:
    GnuCompiler(const char *name) : Compiler(name, "gcc") {}

    void MakePchCommand(const char *pch_filename, SourceType src_type, CompileMode compile_mode,
                        bool warnings, Span<const char *const> definitions,
                        Span<const char *const> include_directories, const char *deps_filename,
                        Allocator *alloc, BuildCommand *out_cmd) const override
    {
        MakeObjectCommand(pch_filename, src_type, compile_mode, warnings, nullptr, definitions,
                          include_directories, nullptr, deps_filename, alloc, out_cmd);
    }

    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, CompileMode compile_mode,
                           bool warnings, const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, const char *dest_filename,
                           const char *deps_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "gcc -std=gnu11"); } break;
            case SourceType::CXX: { Fmt(&buf, "g++ -std=gnu++17 -fno-exceptions"); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header"); } break;
                case SourceType::CXX: { Fmt(&buf, " -x c++-header"); } break;
            }
        }
        out_cmd->rsp_offset = buf.len;

        if (warnings) {
            Fmt(&buf, " -Wall");
            if (src_type == SourceType::CXX) {
                Fmt(&buf, " -Wno-class-memaccess");
            }
        } else {
            Fmt(&buf, " -w");
        }

        // Platform flags
#if defined(_WIN32)
        Fmt(&buf, " -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
                  " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -D__USE_MINGW_ANSI_STDIO=1");
#elif defined(__APPLE__)
        Fmt(&buf, " -pthread -fPIC");
#else
        Fmt(&buf, " -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
                  " -pthread -fPIC -fstack-protector-strong --param ssp-buffer-size=4");
        if (compile_mode == CompileMode::Fast || compile_mode == CompileMode::Release) {
            Fmt(&buf, " -D_FORTIFY_SOURCE=2");
        }
#endif

        // Common flags (source, definitions, include directories, etc.)
        AppendGccObjectArguments(src_filename, compile_mode, pch_filename, definitions,
                                 include_directories, deps_filename, &buf);

        out_cmd->line = buf.Leak();
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames, CompileMode compile_mode,
                         Span<const char *const> libraries, LinkType link_type,
                         const char *dest_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "g++"); } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "g++ -shared"); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (compile_mode == CompileMode::Release) {
            Fmt(&buf, " -flto -s");
            if (link_type == LinkType::Executable) {
                Fmt(&buf, " -static");
            }
        } else {
            Fmt(&buf, " -g");
        }

        // Platform flags
#if defined(_WIN32)
        Fmt(&buf, " -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va");
#elif defined(__APPLE__)
#else
        Fmt(&buf, " -Wl,-z,relro,-z,now");
        if (link_type == LinkType::Executable) {
            Fmt(&buf, " -pie");
        }
#endif

        // Objects and libraries
        AppendGccLinkArguments(obj_filenames, libraries, &buf);

        out_cmd->line = buf.Leak();
    }
};

#ifdef _WIN32
class MsCompiler: public Compiler {
public:
    MsCompiler(const char *name) : Compiler(name, "cl") {}

    void MakePchCommand(const char *pch_filename, SourceType src_type, CompileMode compile_mode,
                        bool warnings, Span<const char *const> definitions,
                        Span<const char *const> include_directories, const char *deps_filename,
                        Allocator *alloc, BuildCommand *out_cmd) const override
    {
        MakeObjectCommand(pch_filename, src_type, compile_mode, warnings, nullptr, definitions,
                          include_directories, nullptr, deps_filename, alloc, out_cmd);
    }

    const char *GetPchObject(const char *pch_filename, Allocator *alloc) const override
    {
        const char *obj_filename = Fmt(alloc, "%1.obj", pch_filename).ptr;
        return obj_filename;
    }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, CompileMode compile_mode,
                           bool warnings, const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, const char *dest_filename,
                           const char *deps_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "cl /nologo"); } break;
            case SourceType::CXX: { Fmt(&buf, "cl /nologo /std:c++17 "); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " \"/Fo%1\"", dest_filename);
        } else {
            Fmt(&buf, " /Yc \"/Fp%1.pch\" \"/Fo%1.obj\"", src_filename);
        }
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " /MT /EHsc %1", warnings ? "/W3 /wd4200" : "/w");
        switch (compile_mode) {
            case CompileMode::Debug: { Fmt(&buf, " /Od /Z7"); } break;
            case CompileMode::Fast: { Fmt(&buf, " /O2 /DNDEBUG"); } break;
            case CompileMode::Release: { Fmt(&buf, " /O2 /GL /DNDEBUG"); } break;
        }

        // Platform flags
        Fmt(&buf, " /D_LARGEFILE_SOURCE /D_LARGEFILE64_SOURCE /D_FILE_OFFSET_BITS=64"
                  " /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DNOMINMAX"
                  " /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE");

        // Sources and definitions
        Fmt(&buf, " /c /utf-8 \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " \"/FI%1\" \"/Yu%1\" \"/Fp%1.pch\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " /D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " /I%1", include_directory);
        }
        if (deps_filename) {
            Fmt(&buf, " /showIncludes");
            out_cmd->parse_cl_includes = true;
        }

        out_cmd->line = buf.Leak();
        out_cmd->skip_lines = 1;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames, CompileMode compile_mode,
                         Span<const char *const> libraries, LinkType link_type,
                         const char *dest_filename, Allocator *alloc, BuildCommand *out_cmd) const override
    {
        HeapArray<char> buf;
        buf.allocator = alloc;

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "link /nologo"); } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "link /nologo /DLL"); } break;
        }
        Fmt(&buf, " \"/OUT:%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        switch (compile_mode) {
            case CompileMode::Debug: { Fmt(&buf, " /DEBUG:FULL"); } break;
            case CompileMode::Fast: { Fmt(&buf, " /DEBUG:NONE"); } break;
            case CompileMode::Release: { Fmt(&buf, " /LTCG /DEBUG:NONE"); } break;
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            Fmt(&buf, " %1.lib", lib);
        }

        out_cmd->line = buf.Leak();
    }
};
#endif

static ClangCompiler ClangCompiler("Clang");
static GnuCompiler GnuCompiler("GCC");
#ifdef _WIN32
static MsCompiler MsCompiler("MSVC");
#endif

static const Compiler *const CompilerTable[] = {
#ifdef _WIN32
    &MsCompiler,
#endif
    &ClangCompiler,
    &GnuCompiler
};
const Span<const Compiler *const> Compilers = CompilerTable;

}
