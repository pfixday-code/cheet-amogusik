#pragma once
#include <cstdint>
#include <cstddef>
#include <link.h>

struct LoadedModule {
    uintptr_t base = 0;   // lowest mapped address
    uintptr_t end = 0;    // highest mapped address
    uintptr_t bias = 0;   // load bias (dlpi_addr)
    const ElfW(Sym)* syms = nullptr;
    const char* strtab = nullptr;
    size_t nsyms = 0;
    const ElfW(Rela)* jmprel = nullptr;  // .rela.plt
    size_t njmprel = 0;
    const ElfW(Rela)* rela = nullptr;    // .rela.dyn
    size_t nrela = 0;
    uintptr_t relroLo = 0;               // PT_GNU_RELRO, page-rounded like the linker does
    uintptr_t relroHi = 0;
    char path[256] = {};

    bool valid() const { return base != 0; }
    bool contains(const void* p) const {
        auto a = reinterpret_cast<uintptr_t>(p);
        return a >= base && a < end;
    }
};

// Finds an already loaded library whose path ends with `suffix` (e.g. "libil2cpp.so").
bool FindModule(const char* suffix, LoadedModule& out);
// Looks a symbol up in the library's own .dynsym, independent of linker namespaces.
void* FindExport(const LoadedModule& m, const char* name);
// st_size of an exported function (0 when unknown). Used to refuse inline hooks that would
// overwrite the neighbouring function.
size_t FindExportSize(const LoadedModule& m, const char* name);
// Rewrites every GOT slot of `m` that imports `name` (JUMP_SLOT and GLOB_DAT). No code is patched.
// Returns the number of slots rewritten.
int PatchImports(const LoadedModule& m, const char* name, void* replacement);
