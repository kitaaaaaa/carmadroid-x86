#include "elf_loader.h"
#include "cpu.h"
#include "memory.h"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace {

#pragma pack(push, 1)
struct Elf32_Ehdr {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32_Phdr { u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; };
struct Elf32_Dyn { s32 d_tag; u32 d_val; };
struct Elf32_Sym { u32 st_name, st_value, st_size; u8 st_info, st_other; u16 st_shndx; };
struct Elf32_Rel { u32 r_offset, r_info; };
#pragma pack(pop)

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_ARM_EXIDX = 0x70000001 };
enum {
    DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT = 12, DT_REL = 17, DT_RELSZ = 18,
    DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
};
enum { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

std::vector<Module*> g_modules;

}  // namespace

namespace loader {

const std::vector<Module*>& modules() { return g_modules; }

u32 find_symbol(const std::string& name) {
    for (Module* m : g_modules) {
        auto it = m->exports.find(name);
        if (it != m->exports.end()) return it->second;
    }
    return 0;
}

Module* module_at(u32 addr) {
    for (Module* m : g_modules)
        if (addr >= m->base && addr < m->base + m->size) return m;
    return nullptr;
}

Module* load(const std::string& path, u32 base) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fatal("cannot open %s", path.c_str());
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), {});
    return load_image(path, file, base);
}

Module* load_image(const std::string& path, const std::vector<u8>& file, u32 base) {
    auto* eh = (Elf32_Ehdr*)file.data();
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) || eh->e_ident[4] != 1 || eh->e_machine != 40)
        fatal("%s is not a 32-bit ARM ELF", path.c_str());

    auto* m = new Module();
    m->name = path.substr(path.find_last_of("/\\") + 1);
    m->base = base;

    auto* ph = (Elf32_Phdr*)(file.data() + eh->e_phoff);
    u32 dyn_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            if (!mem::valid(base + p.p_vaddr, p.p_memsz)) fatal("%s: segment outside arena", m->name.c_str());
            memcpy(mem::ptr(base + p.p_vaddr), file.data() + p.p_offset, p.p_filesz);
            memset(mem::ptr(base + p.p_vaddr + p.p_filesz), 0, p.p_memsz - p.p_filesz);
            m->size = std::max(m->size, p.p_vaddr + p.p_memsz);
            if (p.p_flags & 1) m->text_end = std::max(m->text_end, p.p_vaddr + p.p_memsz);
        } else if (p.p_type == PT_DYNAMIC) {
            dyn_vaddr = p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            m->exidx = base + p.p_vaddr;
            m->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dyn_vaddr) fatal("%s has no dynamic section", m->name.c_str());

    u32 strtab = 0, symtab = 0, hash = 0, rel = 0, relsz = 0, jmprel = 0, jmprelsz = 0;
    u32 init_array = 0, init_arraysz = 0;
    for (auto* d = mem::ptr<Elf32_Dyn>(base + dyn_vaddr); d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB: strtab = base + d->d_val; break;
            case DT_SYMTAB: symtab = base + d->d_val; break;
            case DT_HASH: hash = base + d->d_val; break;
            case DT_REL: rel = base + d->d_val; break;
            case DT_RELSZ: relsz = d->d_val; break;
            case DT_JMPREL: jmprel = base + d->d_val; break;
            case DT_PLTRELSZ: jmprelsz = d->d_val; break;
            case DT_INIT: m->dt_init = base + d->d_val; break;
            case DT_INIT_ARRAY: init_array = base + d->d_val; break;
            case DT_INIT_ARRAYSZ: init_arraysz = d->d_val; break;
        }
    }
    const u32 nsyms = mem::r32(hash + 4);  // nchain == number of symbols
    auto* syms = mem::ptr<Elf32_Sym>(symtab);
    auto sym_name = [&](u32 i) { return std::string(mem::str(strtab + syms[i].st_name)); };

    for (u32 i = 1; i < nsyms; i++) {
        const Elf32_Sym& s = syms[i];
        if (s.st_shndx == 0 || !s.st_value) continue;
        std::string n = sym_name(i);
        u32 addr = base + s.st_value;
        m->exports.emplace(n, addr);
        m->sorted_syms.emplace_back(addr & ~1u, n);
        if ((s.st_info & 0xf) == 2) m->sym_sizes[addr & ~1u] = s.st_size;
    }
    std::sort(m->sorted_syms.begin(), m->sorted_syms.end());

    // Resolve a symbol reference: prefer earlier-loaded modules, then this one, then HLE.
    std::unordered_map<u32, u32> cache;
    auto resolve = [&](u32 symidx) -> u32 {
        auto c = cache.find(symidx);
        if (c != cache.end()) return c->second;
        const Elf32_Sym& s = syms[symidx];
        std::string n = sym_name(symidx);
        u32 v = 0;
        const bool weak = (s.st_info >> 4) == 2;
        const bool is_func_or_notype = (s.st_info & 0xf) == 2 || (s.st_info & 0xf) == 0;
        if (s.st_shndx == 0 && hle::data_for(n)) {
            v = hle::data_for(n);
        } else if (hle::is_implemented(n) && s.st_shndx == 0) {
            v = hle::thunk_for(n);  // host implementations override undefined imports
        } else if (s.st_shndx != 0) {
            v = base + s.st_value;
        } else if ((v = find_symbol(n)) != 0) {
        } else if (weak) {
            v = 0;
        } else if (is_func_or_notype) {
            v = hle::thunk_for(n);
            m->unresolved.push_back(n);
        } else {
            LOGE("%s: unresolved data import %s", m->name.c_str(), n.c_str());
            m->unresolved.push_back(n);
        }
        cache[symidx] = v;
        return v;
    };

    auto apply = [&](u32 table, u32 size) {
        auto* r = mem::ptr<Elf32_Rel>(table);
        for (u32 i = 0; i < size / 8; i++) {
            const u32 where = base + r[i].r_offset;
            const u32 type = r[i].r_info & 0xff;
            const u32 symidx = r[i].r_info >> 8;
            switch (type) {
                case R_ARM_RELATIVE: mem::w32(where, mem::r32(where) + base); break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT: mem::w32(where, resolve(symidx)); break;
                case R_ARM_ABS32: mem::w32(where, mem::r32(where) + resolve(symidx)); break;
                default: fatal("%s: unsupported relocation type %u", m->name.c_str(), type);
            }
        }
    };
    if (rel) apply(rel, relsz);
    if (jmprel) apply(jmprel, jmprelsz);

    for (u32 i = 0; i < init_arraysz / 4; i++) {
        u32 fn = mem::r32(init_array + i * 4);
        if (fn && fn != 0xffffffff) m->init_array.push_back(fn);
    }

    g_modules.push_back(m);
    LOGI("loaded %s at 0x%08x (%u KB, %zu exports, %zu host imports, %zu initializers)", m->name.c_str(), base,
         m->size >> 10, m->exports.size(), m->unresolved.size(), m->init_array.size());
    return m;
}

std::string function_of(u32 addr) {
    Module* m = module_at(addr);
    char buf[64];
    if (!m) {
        const char* n = hle::name_of(addr & ~1u);
        return n ? std::string("host:") + n : "unknown";
    }
    u32 a = addr & ~1u;
    auto it = std::upper_bound(m->sorted_syms.begin(), m->sorted_syms.end(), std::make_pair(a, std::string("\xff")));
    if (it != m->sorted_syms.begin()) {
        --it;
        auto sz = m->sym_sizes.find(it->first);
        if (sz != m->sym_sizes.end() && a < it->first + sz->second) return it->second;
    }
    snprintf(buf, sizeof buf, "%s+0x%x(unnamed)", m->name.c_str(), (a - m->base) & ~0xfffu);
    return buf;
}

void run_initializers(Module* m) {
    Cpu* c = Cpu::current;
    if (m->dt_init) c->call(m->dt_init, {});
    for (size_t i = 0; i < m->init_array.size(); i++) {
        LOGT("%s init[%zu] %s", m->name.c_str(), i, symbolize(m->init_array[i]).c_str());
        c->call(m->init_array[i], {});
    }
    LOGI("%s: ran %zu initializers", m->name.c_str(), m->init_array.size());
}

}  // namespace loader

std::string symbolize(u32 addr) {
    char buf[64];
    if (const char* n = hle::name_of(addr & ~1u)) {
        snprintf(buf, sizeof buf, "0x%08x <host:%s>", addr, n);
        return buf;
    }
    Module* m = loader::module_at(addr);
    if (!m) {
        snprintf(buf, sizeof buf, "0x%08x", addr);
        return buf;
    }
    u32 a = addr & ~1u;
    auto it = std::upper_bound(m->sorted_syms.begin(), m->sorted_syms.end(), std::make_pair(a, std::string("\xff")));
    std::string out;
    snprintf(buf, sizeof buf, "0x%08x %s+0x%x", addr, m->name.c_str(), a - m->base);
    out = buf;
    if (it != m->sorted_syms.begin()) {
        --it;
        snprintf(buf, sizeof buf, " (%s+0x%x)", it->second.c_str(), a - it->first);
        out += buf;
    }
    return out;
}
