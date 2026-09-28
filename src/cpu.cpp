#include "cpu.h"
#include "elf_loader.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ===========================================================================
// HLE registry
// ===========================================================================
namespace hle {
namespace {

struct Entry {
    std::string name;
    HleFn fn;
    bool implemented;
    u64 calls = 0;
};

constexpr u32 MAX_THUNKS = mem::THUNK_SIZE / 8;

std::recursive_mutex g_lock;
std::vector<Entry>* g_entries;
std::unordered_map<std::string, u32>* g_by_name;
std::unordered_map<std::string, void (*)(Cpu&)>* g_static_fns;  // filled by static Reg objects
u8* g_thunk_mem;

std::unordered_map<std::string, void (*)(Cpu&)>& static_fns() {
    if (!g_static_fns) g_static_fns = new std::unordered_map<std::string, void (*)(Cpu&)>();
    return *g_static_fns;
}

u32 add_entry(const std::string& name, HleFn fn, bool implemented) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    u32 idx = (u32)g_entries->size();
    if (idx >= MAX_THUNKS) fatal("out of HLE thunks");
    g_entries->push_back({name, std::move(fn), implemented});
    u32* t = (u32*)(g_thunk_mem + idx * 8);
    t[0] = 0xEF000000;  // svc #0
    t[1] = 0xE12FFF1E;  // bx lr
    return mem::THUNK_BASE + idx * 8;
}

}  // namespace

Reg::Reg(const char* name, void (*fn)(Cpu&)) { static_fns()[name] = fn; }

static std::unordered_map<std::string, u32> g_data;
void register_data(const std::string& name, u32 addr) { g_data[name] = addr; }
u32 data_for(const std::string& name) {
    auto it = g_data.find(name);
    return it == g_data.end() ? 0 : it->second;
}

void init() {
    g_thunk_mem = (u8*)VirtualAlloc(nullptr, mem::THUNK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_entries = new std::vector<Entry>();
    g_by_name = new std::unordered_map<std::string, u32>();
    add_entry("<return to host>", [](Cpu&) { fatal("executed RET_MAGIC thunk"); }, true);
    LOGI("HLE: %zu host functions registered", static_fns().size());
}

u8* thunk_mem() { return g_thunk_mem; }

bool is_implemented(const std::string& name) { return static_fns().count(name) != 0; }

u32 thunk_for(const std::string& name) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_by_name->find(name);
    if (it != g_by_name->end()) return it->second;
    u32 addr;
    auto sf = static_fns().find(name);
    if (sf != static_fns().end()) {
        addr = add_entry(name, sf->second, true);
    } else {
        std::string n = name;
        addr = add_entry(name, [n](Cpu& c) {
            LOGV("unimplemented: %s(0x%x, 0x%x, 0x%x, 0x%x) from %s", n.c_str(), c.r(0), c.r(1), c.r(2), c.r(3),
                 symbolize(c.lr()).c_str());
            c.ret(0);
        }, false);
    }
    (*g_by_name)[name] = addr;
    return addr;
}

u32 add_dynamic(const std::string& name, HleFn fn) { return add_entry(name, std::move(fn), true); }

u32 hook_function(u32 addr, const std::string& name, HleFn fn) {
    if (addr & 1) fatal("hook_function: %s is Thumb code (unsupported)", name.c_str());
    u32 tramp = mem::malloc(16);
    mem::w32(tramp + 0, mem::r32(addr));
    mem::w32(tramp + 4, mem::r32(addr + 4));
    mem::w32(tramp + 8, 0xE51FF004);  // ldr pc, [pc, #-4]
    mem::w32(tramp + 12, addr + 8);
    u32 thunk = add_entry("hook:" + name, std::move(fn), true);
    mem::w32(addr + 0, 0xE51FF004);   // ldr pc, [pc, #-4]
    mem::w32(addr + 4, thunk);
    LOGI("hooked %s at 0x%08x (original via 0x%08x)", name.c_str(), addr, tramp);
    return tramp;
}

const char* name_of(u32 addr) {
    if (addr < mem::THUNK_BASE || addr >= mem::THUNK_BASE + mem::THUNK_SIZE) return nullptr;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    u32 idx = (addr - mem::THUNK_BASE) / 8;
    return idx < g_entries->size() ? (*g_entries)[idx].name.c_str() : nullptr;
}

void dispatch(u32 index, Cpu& c) {
    Entry* e;
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (index >= g_entries->size()) fatal("SVC from unknown thunk index %u", index);
        e = &(*g_entries)[index];
        e->calls++;
        if (!e->implemented && e->calls == 1 && g_log_level < 2)
            LOGI("first call to unimplemented %s (from %s)", e->name.c_str(), symbolize(c.lr()).c_str());
    }
    LOGT("-> %s(0x%x, 0x%x, 0x%x, 0x%x)", e->name.c_str(), c.r(0), c.r(1), c.r(2), c.r(3));
    e->fn(c);
}

void dump_stats() {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    std::vector<Entry*> v;
    for (auto& e : *g_entries) if (e.calls) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](Entry* a, Entry* b) { return a->calls > b->calls; });
    LOGI("---- HLE call counts ----");
    for (auto* e : v) LOGI("%10llu %s%s", (unsigned long long)e->calls, e->name.c_str(), e->implemented ? "" : "  [UNIMPLEMENTED]");
}

}  // namespace hle

// ===========================================================================
// Argument readers
// ===========================================================================
RegArgs::RegArgs(Cpu& c, int first_reg) : c_(c), ncrn_(first_reg), nsaa_(c.sp()) {}

u32 RegArgs::u32v() {
    if (ncrn_ < 4) return c_.r(ncrn_++);
    u32 v = mem::r32(nsaa_);
    nsaa_ += 4;
    return v;
}

u64 RegArgs::u64v() {
    if (ncrn_ & 1) ncrn_++;
    if (ncrn_ <= 2) {
        u64 v = c_.r(ncrn_) | ((u64)c_.r(ncrn_ + 1) << 32);
        ncrn_ += 2;
        return v;
    }
    ncrn_ = 4;
    nsaa_ = (nsaa_ + 7) & ~7u;
    u64 v = mem::r32(nsaa_) | ((u64)mem::r32(nsaa_ + 4) << 32);
    nsaa_ += 8;
    return v;
}

// ===========================================================================
// Cpu
// ===========================================================================
thread_local Cpu* Cpu::current = nullptr;

static const int kRegs[16] = {
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4,  UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
};

namespace profiler {
bool g_enabled = false;
std::atomic<bool> g_active{false};
static std::mutex g_lock;
static std::unordered_map<u32, u64> g_counts;  // block address -> instructions executed
static thread_local std::unordered_map<u32, u64>* t_counts;
static std::vector<std::unordered_map<u32, u64>*> g_all;

static void hook_block(uc_engine*, uint64_t addr, uint32_t size, void*) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (!t_counts) {
        t_counts = new std::unordered_map<u32, u64>();
        std::lock_guard<std::mutex> l(g_lock);
        g_all.push_back(t_counts);
    }
    (*t_counts)[(u32)addr] += size / 4 ? size / 4 : 1;
}

void report() {
    std::unordered_map<std::string, u64> by_fn;
    u64 total = 0;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto* m : g_all)
            for (auto& [addr, n] : *m) {
                by_fn[loader::function_of(addr)] += n;
                total += n;
            }
    }
    {
        std::vector<std::pair<u64, u32>> blocks;
        std::lock_guard<std::mutex> l(g_lock);
        std::unordered_map<u32, u64> merged;
        for (auto* m : g_all)
            for (auto& [addr, n] : *m) merged[addr] += n;
        for (auto& [addr, n] : merged) blocks.push_back({n, addr});
        std::sort(blocks.rbegin(), blocks.rend());
        LOGI("==== hottest basic blocks ====");
        for (size_t i = 0; i < blocks.size() && i < 25; i++)
            LOGI("%12llu  %s", (unsigned long long)blocks[i].first, symbolize(blocks[i].second).c_str());
    }
    std::vector<std::pair<u64, std::string>> v;
    for (auto& [fn, n] : by_fn) v.push_back({n, fn});
    std::sort(v.rbegin(), v.rend());
    LOGI("==== profile: %llu guest instructions ====", (unsigned long long)total);
    for (size_t i = 0; i < v.size() && i < 60; i++)
        LOGI("%6.2f%% %12llu  %s", 100.0 * v[i].first / (total ? total : 1), (unsigned long long)v[i].first, v[i].second.c_str());
}
}  // namespace profiler

static u8* g_kuser_page;
static std::once_flag g_kuser_once;

static void hook_intr(uc_engine* uc, uint32_t intno, void* user) {
    Cpu* c = (Cpu*)user;
    u32 pc = c->pc();
    if (intno != 2) {
        c->dump_state("unexpected CPU exception");
        fatal("CPU exception %u at 0x%08x", intno, pc);
    }
    u32 svc_addr = pc - 4;
    if (svc_addr < mem::THUNK_BASE || svc_addr >= mem::THUNK_BASE + mem::THUNK_SIZE) {
        c->dump_state("SVC outside thunk region (raw syscall?)");
        fatal("raw SVC at 0x%08x", svc_addr);
    }
    hle::dispatch((svc_addr - mem::THUNK_BASE) / 8, *c);
    if (c->exiting) uc_emu_stop(uc);
}

static bool hook_bad_mem(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user) {
    Cpu* c = (Cpu*)user;
    const char* kind = type == UC_MEM_READ_UNMAPPED ? "read" : type == UC_MEM_WRITE_UNMAPPED ? "write"
                     : type == UC_MEM_FETCH_UNMAPPED ? "fetch" : "protected access";
    char buf[128];
    snprintf(buf, sizeof buf, "invalid memory %s at 0x%08llx (size %d)", kind, (unsigned long long)addr, size);
    c->dump_state(buf);
    return false;
}

Cpu::Cpu(u32 tid, u32 stack_size) : thread_id(tid) {
    std::call_once(g_kuser_once, [] {
        g_kuser_page = (u8*)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        u32* p = (u32*)(g_kuser_page + 0xfe0);
        p[0] = 0xE51FF004;  // ldr pc, [pc, #-4]
        p[1] = hle::thunk_for("__kuser_get_tls");
    });

    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) fatal("uc_open failed");
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A15);
    uc_ctl_set_tcg_buffer_size(uc, 64u << 20);

    if (uc_mem_map_ptr(uc, mem::THUNK_BASE, mem::THUNK_SIZE, UC_PROT_READ | UC_PROT_EXEC, hle::thunk_mem()))
        fatal("map thunks failed");
    if (uc_mem_map_ptr(uc, mem::ARENA_BASE, mem::ARENA_END - mem::ARENA_BASE, UC_PROT_ALL, mem::g_arena))
        fatal("map arena failed");
    if (uc_mem_map_ptr(uc, mem::KUSER_PAGE, 0x10000, UC_PROT_READ | UC_PROT_EXEC, g_kuser_page))
        fatal("map kuser page failed");

    // Enable VFP/NEON: CPACR full access to cp10/cp11, then FPEXC.EN
    u32 cpacr = 0;
    uc_reg_read(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= 0xF << 20;
    uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    u32 fpexc = 0x40000000;
    uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)hook_intr, this, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_INVALID, (void*)hook_bad_mem, this, 1, 0);
    if (profiler::g_enabled) uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void*)profiler::hook_block, this, 1, 0);

    stack_top = mem::alloc_stack(stack_size);
    set_sp(stack_top);
    errno_addr = mem::calloc(1, 4);
}

Cpu::~Cpu() {
    if (uc) uc_close(uc);
    mem::free(errno_addr);
}

u32 Cpu::r(int n) { u32 v = 0; uc_reg_read(uc, kRegs[n], &v); return v; }
void Cpu::set_r(int n, u32 v) { uc_reg_write(uc, kRegs[n], &v); }
u32 Cpu::sp() { return r(13); }
void Cpu::set_sp(u32 v) { set_r(13, v); }
u32 Cpu::lr() { return r(14); }
void Cpu::set_lr(u32 v) { set_r(14, v); }
u32 Cpu::pc() { return r(15); }
u64 Cpu::d(int n) { u64 v = 0; uc_reg_read(uc, UC_ARM_REG_D0 + n, &v); return v; }
void Cpu::set_d(int n, u64 v) { uc_reg_write(uc, UC_ARM_REG_D0 + n, &v); }

u32 Cpu::arg(int i) {
    if (i < 4) return r(i);
    return mem::r32(sp() + (i - 4) * 4);
}

u64 Cpu::call64(u32 fn, std::initializer_list<u32> args) {
    Cpu* prev = current;
    current = this;
    uc_context* saved = nullptr;
    const bool nested = depth > 0;
    if (nested) {
        uc_context_alloc(uc, &saved);
        uc_context_save(uc, saved);
    }
    u32 s = (nested ? sp() : stack_top) - 64;
    const int n = (int)args.size();
    if (n > 4) s -= (n - 4) * 4;
    s &= ~7u;
    int i = 0;
    for (u32 a : args) {
        if (i < 4) set_r(i, a);
        else mem::w32(s + (i - 4) * 4, a);
        i++;
    }
    set_sp(s);
    set_lr(hle::RET_MAGIC);

    depth++;
    uc_err err = uc_emu_start(uc, fn, hle::RET_MAGIC, 0, 0);
    depth--;
    if (err != UC_ERR_OK && !exiting) {
        char buf[160];
        snprintf(buf, sizeof buf, "emulation error '%s' while running %s", uc_strerror(err), symbolize(fn).c_str());
        dump_state(buf);
        fatal("%s", buf);
    }
    u64 result = r(0) | ((u64)r(1) << 32);
    if (nested) {
        uc_context_restore(uc, saved);
        uc_context_free(saved);
    }
    current = prev;
    return result;
}

u32 Cpu::call(u32 fn, std::initializer_list<u32> args) { return (u32)call64(fn, args); }

void Cpu::dump_state(const char* why) {
    LOGE("==== thread %u: %s ====", thread_id, why);
    for (int i = 0; i < 16; i += 4)
        LOGE("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r(i), i + 1, r(i + 1), i + 2, r(i + 2), i + 3, r(i + 3));
    u32 cpsr = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    LOGE("  cpsr=%08x (%s)", cpsr, (cpsr & 0x20) ? "thumb" : "arm");
    LOGE("  pc = %s", symbolize(pc()).c_str());
    LOGE("  lr = %s", symbolize(lr()).c_str());
    backtrace();
}

void Cpu::backtrace() {
    // No frame pointers: scan the stack for words that look like return addresses into loaded code.
    u32 s = sp();
    int found = 0;
    for (u32 a = s; a < stack_top && a < s + 0x4000 && found < 16; a += 4) {
        u32 v = mem::r32(a);
        Module* m = loader::module_at(v);
        if (m && (v & ~1u) - m->base < m->text_end) {
            LOGE("  stack[%04x] %s", a - s, symbolize(v).c_str());
            found++;
        }
    }
}
