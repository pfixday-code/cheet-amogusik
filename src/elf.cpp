#include "elf.h"
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif

namespace {

struct Query {
    const char* suffix;
    LoadedModule* out;
    bool found;
};

size_t GnuSymbolCount(const uint32_t* h) {
    const uint32_t nbuckets = h[0];
    const uint32_t symoffset = h[1];
    const uint32_t bloomSize = h[2];
    const auto* bloom = reinterpret_cast<const uint64_t*>(h + 4);
    const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloomSize);
    const uint32_t* chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) last = buckets[i] > last ? buckets[i] : last;
    if (last < symoffset) return symoffset;
    while ((chain[last - symoffset] & 1u) == 0) ++last;
    return static_cast<size_t>(last) + 1;
}

int Callback(dl_phdr_info* info, size_t, void* data) {
    auto* q = static_cast<Query*>(data);
    const char* name = info->dlpi_name;
    if (!name || !*name) return 0;
    size_t nl = strlen(name), sl = strlen(q->suffix);
    if (nl < sl || strcmp(name + nl - sl, q->suffix) != 0) return 0;

    LoadedModule& m = *q->out;
    m = LoadedModule{};
    m.bias = info->dlpi_addr;
    m.base = UINTPTR_MAX;
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if (p.p_type == PT_LOAD) {
            uintptr_t lo = info->dlpi_addr + p.p_vaddr;
            uintptr_t hi = lo + p.p_memsz;
            if (lo < m.base) m.base = lo;
            if (hi > m.end) m.end = hi;
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + p.p_vaddr);
        } else if (p.p_type == PT_GNU_RELRO) {
            const uintptr_t pg = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
            const uintptr_t lo = info->dlpi_addr + p.p_vaddr;
            m.relroLo = lo & ~(pg - 1);
            m.relroHi = (lo + p.p_memsz + pg - 1) & ~(pg - 1);
        }
    }
    if (m.base == UINTPTR_MAX || !dyn) { m = LoadedModule{}; return 0; }

    // Some linkers already relocate d_ptr, some do not.
    auto fix = [&](uintptr_t p) { return p >= m.base ? p : m.bias + p; };
    const uint32_t* gnuHash = nullptr;
    const uint32_t* sysvHash = nullptr;
    size_t jmprelSz = 0, relaSz = 0;
    const void* jmprel = nullptr;
    const void* rela = nullptr;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB: m.syms = reinterpret_cast<const ElfW(Sym)*>(fix(d->d_un.d_ptr)); break;
            case DT_STRTAB: m.strtab = reinterpret_cast<const char*>(fix(d->d_un.d_ptr)); break;
            case DT_GNU_HASH: gnuHash = reinterpret_cast<const uint32_t*>(fix(d->d_un.d_ptr)); break;
            case DT_HASH: sysvHash = reinterpret_cast<const uint32_t*>(fix(d->d_un.d_ptr)); break;
            case DT_JMPREL: jmprel = reinterpret_cast<const void*>(fix(d->d_un.d_ptr)); break;
            case DT_PLTRELSZ: jmprelSz = d->d_un.d_val; break;
            case DT_RELA: rela = reinterpret_cast<const void*>(fix(d->d_un.d_ptr)); break;
            case DT_RELASZ: relaSz = d->d_un.d_val; break;
        }
    }
    if (sysvHash) m.nsyms = sysvHash[1];
    else if (gnuHash) m.nsyms = GnuSymbolCount(gnuHash);
    m.jmprel = static_cast<const ElfW(Rela)*>(jmprel);
    m.njmprel = jmprelSz / sizeof(ElfW(Rela));
    m.rela = static_cast<const ElfW(Rela)*>(rela);
    m.nrela = relaSz / sizeof(ElfW(Rela));
    strncpy(m.path, name, sizeof(m.path) - 1);
    q->found = m.syms && m.strtab && m.nsyms;
    return q->found ? 1 : 0;
}

}  // namespace

bool FindModule(const char* suffix, LoadedModule& out) {
    Query q{suffix, &out, false};
    dl_iterate_phdr(Callback, &q);
    return q.found;
}

void* FindExport(const LoadedModule& m, const char* name) {
    for (size_t i = 0; i < m.nsyms; ++i) {
        const ElfW(Sym)& s = m.syms[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        if (strcmp(m.strtab + s.st_name, name) == 0)
            return reinterpret_cast<void*>(m.bias + s.st_value);
    }
    return nullptr;
}

size_t FindExportSize(const LoadedModule& m, const char* name) {
    for (size_t i = 0; i < m.nsyms; ++i) {
        const ElfW(Sym)& s = m.syms[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        if (strcmp(m.strtab + s.st_name, name) == 0) return static_cast<size_t>(s.st_size);
    }
    return 0;
}

namespace {

bool PatchSlot(const LoadedModule& m, uintptr_t* slot, void* replacement) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    if (addr < m.base || addr + sizeof(uintptr_t) > m.end || (addr & 7u) != 0) return false;
    const uintptr_t pg = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t page = addr & ~(pg - 1);
    if (mprotect(reinterpret_cast<void*>(page), pg, PROT_READ | PROT_WRITE) != 0) return false;
    __atomic_store_n(slot, reinterpret_cast<uintptr_t>(replacement), __ATOMIC_RELEASE);
    // Put read-only protection back only where the linker had put it (full RELRO pages).
    if (page >= m.relroLo && page + pg <= m.relroHi) mprotect(reinterpret_cast<void*>(page), pg, PROT_READ);
    return true;
}

int PatchTable(const LoadedModule& m, const ElfW(Rela)* r, size_t count, const char* name, void* replacement) {
    int patched = 0;
    for (size_t i = 0; r && i < count; ++i) {
        const unsigned type = ELF64_R_TYPE(r[i].r_info);
        if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
        const size_t si = ELF64_R_SYM(r[i].r_info);
        if (si == 0 || si >= m.nsyms) continue;
        if (strcmp(m.strtab + m.syms[si].st_name, name) != 0) continue;
        if (PatchSlot(m, reinterpret_cast<uintptr_t*>(m.bias + r[i].r_offset), replacement)) ++patched;
    }
    return patched;
}

}  // namespace

int PatchImports(const LoadedModule& m, const char* name, void* replacement) {
    return PatchTable(m, m.jmprel, m.njmprel, name, replacement) +
           PatchTable(m, m.rela, m.nrela, name, replacement);
}
