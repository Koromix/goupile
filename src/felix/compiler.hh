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

#pragma once

#include "../core/libcc/libcc.hh"

namespace RG {

enum class CompileOptimization {
    None,
    Debug,
    Fast,
    LTO
};
static const char *const CompileOptimizationNames[] = {
    "None",
    "Debug",
    "Fast",
    "LTO"
};

enum class CompileFeature {
    PCH = 1 << 0,
    NoDebug = 1 << 1,
    StaticLink = 1 << 2,
    ASan = 1 << 3,
    TSan = 1 << 4,
    UBSan = 1 << 5,
    SafeStack = 1 << 6,
    ZeroInit = 1 << 7,
    CFI = 1 << 8,
    ShuffleCode = 1 << 9
};
static const char *const CompileFeatureNames[] = {
    "PCH",
    "NoDebug",
    "StaticLink",
    "ASan",
    "TSan",
    "UBSan",
    "SafeStack", // Clang
    "ZeroInit", // Clang
    "CFI", // Clang
    "ShuffleCode" // Clang
};

enum class SourceType {
    C,
    CXX
};

enum class LinkType {
    Executable,
    SharedLibrary
};

struct Command {
    enum class DependencyMode {
        None,
        MakeLike,
        ShowIncludes
    };

    Span<const char> cmd_line; // Must be C safe (NULL termination)
    Size cache_len;
    Size rsp_offset;
    bool skip_success;
    int skip_lines;
    DependencyMode deps_mode;
    const char *deps_filename; // Used by MakeLike mode
};

class Compiler {
    RG_DELETE_COPY(Compiler)

public:
    const char *name;

    virtual ~Compiler() {}

    virtual bool CheckFeatures(CompileOptimization compile_opt, uint32_t features) const = 0;

    virtual const char *GetObjectExtension() const = 0;
    virtual const char *GetExecutableExtension() const = 0;

    virtual void MakePackCommand(Span<const char *const> pack_filenames, CompileOptimization compile_opt,
                                 const char *pack_options, const char *dest_filename,
                                 Allocator *alloc, Command *out_cmd) const = 0;

    virtual void MakePchCommand(const char *pch_filename, SourceType src_type, CompileOptimization compile_opt,
                                bool warnings, Span<const char *const> definitions,
                                Span<const char *const> include_directories, uint32_t features, bool env_flags,
                                Allocator *alloc, Command *out_cmd) const = 0;
    virtual const char *GetPchObject(const char *pch_filename, Allocator *alloc) const = 0;

    virtual void MakeObjectCommand(const char *src_filename, SourceType src_type, CompileOptimization compile_opt,
                                   bool warnings, const char *pch_filename, Span<const char *const> definitions,
                                   Span<const char *const> include_directories, uint32_t features, bool env_flags,
                                   const char *dest_filename, Allocator *alloc, Command *out_cmd) const = 0;

    virtual void MakeLinkCommand(Span<const char *const> obj_filenames, CompileOptimization compile_opt,
                                 Span<const char *const> libraries, LinkType link_type,
                                 uint32_t features, bool env_flags, const char *dest_filename,
                                 Allocator *alloc, Command *out_cmd) const = 0;

protected:
    Compiler(const char *name) : name(name) {}
};

struct SupportedCompiler {
    const char *name;
    const char *cc;
};

struct CompilerInfo {
    const char *cc = nullptr;
    const char *ld = nullptr;
};

std::unique_ptr<const Compiler> PrepareCompiler(CompilerInfo info);

extern const Span<const SupportedCompiler> SupportedCompilers;

}
