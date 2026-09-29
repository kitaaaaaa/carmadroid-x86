#include "cpu.h"
#include "elf_loader.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <windows.h>
#include <array>
#include <atomic>
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
// ===========================================================================
// Cpu: dynarmic A32 JIT backend
// ===========================================================================
thread_local Cpu* Cpu::current = nullptr;

// dynarmic optimizations are off by default: with them, FMOD's audio comes out garbled (beeping,
// warbling), and they only speed up loading. --jit-opt 0xFFFF turns them all back on.
u32 g_jit_optimizations = 0;
// JIT optimizations for FMOD's threads (mixer/streams); same as everything else unless
// --jit-opt-audio overrides it (kept as a diagnostic switch).
u32 g_jit_optimizations_audio = 0;

namespace profiler {
bool g_enabled = false;
std::atomic<bool> g_active{false};
void report() { LOGI("profiler: not available with the dynarmic CPU backend"); }
}  // namespace profiler

namespace {

constexpr u32 kPageBits = 12;
constexpr u32 kPageSize = 1u << kPageBits;

// Guest page -> host pointer, shared by every JIT (filled once, read-only afterwards).
std::array<u8*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>* g_page_table;
u8* g_kuser_page;
Dynarmic::ExclusiveMonitor* g_monitor;
std::atomic<size_t> g_next_processor{0};
constexpr size_t kMaxProcessors = 256;

void map_pages(u32 guest, u32 size, u8* host) {
    for (u32 off = 0; off < size; off += kPageSize) (*g_page_table)[(guest + off) >> kPageBits] = host + off;
}

void init_globals() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_kuser_page = (u8*)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        u32* p = (u32*)(g_kuser_page + 0xfe0);
        p[0] = 0xE51FF004;  // ldr pc, [pc, #-4]
        p[1] = hle::thunk_for("__kuser_get_tls");

        g_page_table = new std::array<u8*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>();
        g_page_table->fill(nullptr);
        map_pages(mem::THUNK_BASE, mem::THUNK_SIZE, hle::thunk_mem());
        map_pages(mem::ARENA_BASE, mem::ARENA_END - mem::ARENA_BASE, mem::g_arena);
        map_pages(mem::KUSER_PAGE, 0x10000, g_kuser_page);
        g_monitor = new Dynarmic::ExclusiveMonitor(kMaxProcessors);
    });
}

u8* host_ptr(u32 vaddr) {
    u8* page = (*g_page_table)[vaddr >> kPageBits];
    return page ? page + (vaddr & (kPageSize - 1)) : nullptr;
}

}  // namespace

struct Cpu::Backend final : Dynarmic::A32::UserCallbacks {
    Cpu& cpu;
    std::unique_ptr<Dynarmic::A32::Jit> jit;

    Backend(Cpu& c, size_t code_cache) : cpu(c) {
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = this;
        cfg.page_table = g_page_table;
        cfg.processor_id = g_next_processor++ % kMaxProcessors;
        cfg.global_monitor = g_monitor;
        // Must stay enabled: with cycle counting off, dynarmic (x64 backend) switches MXCSR to the
        // host value when calling CallSVC/ExceptionRaised but never switches it back, so guest
        // floating-point code after any HLE call runs without flush-to-zero once blocks are linked.
        // That corrupted FMOD's DSP state (beeping, warbling audio). The tick budget is unlimited.
        cfg.enable_cycle_counting = true;
        cfg.code_cache_size = code_cache;
        cfg.define_unpredictable_behaviour = true;
        cfg.optimizations = static_cast<Dynarmic::OptimizationFlag>(
            c.audio_thread ? g_jit_optimizations_audio : g_jit_optimizations);
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
        jit->SetCpsr(0x10);  // user mode, ARM
    }

    template <class T>
    T read(u32 vaddr) {
        u8* p = host_ptr(vaddr);
        if (!p || !host_ptr(vaddr + sizeof(T) - 1)) bad_access("read", vaddr, sizeof(T));
        T v;
        memcpy(&v, p, sizeof v);
        return v;
    }
    template <class T>
    void write(u32 vaddr, T v) {
        u8* p = host_ptr(vaddr);
        if (!p || !host_ptr(vaddr + sizeof(T) - 1)) bad_access("write", vaddr, sizeof(T));
        memcpy(p, &v, sizeof v);
    }
    template <class T>
    bool write_exclusive(u32 vaddr, T value, T expected) {
        u8* p = host_ptr(vaddr);
        if (!p) bad_access("exclusive write", vaddr, sizeof(T));
        return std::atomic_ref<T>(*reinterpret_cast<T*>(p)).compare_exchange_strong(expected, value);
    }
    [[noreturn]] void bad_access(const char* kind, u32 vaddr, size_t size) {
        char buf[128];
        snprintf(buf, sizeof buf, "invalid memory %s at 0x%08x (size %zu)", kind, vaddr, size);
        cpu.dump_state(buf);
        fatal("%s", buf);
    }

    std::uint8_t MemoryRead8(u32 a) override { return read<u8>(a); }
    std::uint16_t MemoryRead16(u32 a) override { return read<u16>(a); }
    std::uint32_t MemoryRead32(u32 a) override { return read<u32>(a); }
    std::uint64_t MemoryRead64(u32 a) override { return read<u64>(a); }
    void MemoryWrite8(u32 a, std::uint8_t v) override { write(a, v); }
    void MemoryWrite16(u32 a, std::uint16_t v) override { write(a, v); }
    void MemoryWrite32(u32 a, std::uint32_t v) override { write(a, v); }
    void MemoryWrite64(u32 a, std::uint64_t v) override { write(a, v); }
    bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t e) override { return write_exclusive(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t e) override { return write_exclusive(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t e) override { return write_exclusive(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t e) override { return write_exclusive(a, v, e); }

    void InterpreterFallback(u32 pc, size_t n) override {
        cpu.dump_state("instruction not supported by the JIT");
        fatal("interpreter fallback at 0x%08x (%zu instructions)", pc, n);
    }
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override {
        if (e == Dynarmic::A32::Exception::Yield || e == Dynarmic::A32::Exception::WaitForEvent ||
            e == Dynarmic::A32::Exception::WaitForInterrupt || e == Dynarmic::A32::Exception::SendEvent ||
            e == Dynarmic::A32::Exception::SendEventLocal || e == Dynarmic::A32::Exception::PreloadData ||
            e == Dynarmic::A32::Exception::PreloadDataWithIntentToWrite || e == Dynarmic::A32::Exception::PreloadInstruction)
            return;  // hints
        char buf[96];
        snprintf(buf, sizeof buf, "CPU exception %d at 0x%08x", (int)e, pc);
        cpu.dump_state(buf);
        fatal("%s", buf);
    }
    void CallSVC(std::uint32_t) override {
        const u32 svc_addr = jit->Regs()[15] - 4;
        if (svc_addr == hle::RET_MAGIC) {  // a host->guest call returned
            returned = true;
            jit->HaltExecution();
            return;
        }
        if (svc_addr < mem::THUNK_BASE || svc_addr >= mem::THUNK_BASE + mem::THUNK_SIZE) {
            cpu.dump_state("SVC outside thunk region (raw syscall?)");
            fatal("raw SVC at 0x%08x", svc_addr);
        }
        hle::dispatch((svc_addr - mem::THUNK_BASE) / 8, cpu);
        if (cpu.exiting) jit->HaltExecution();
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 40; }
    bool returned = false;  // set when the current host->guest call reaches RET_MAGIC
};

Cpu::Backend& Cpu::level(int i) {
    while ((int)levels.size() <= i) {
        // Level 0 runs whole threads; deeper levels only run short callbacks, so give them less cache.
        levels.push_back(std::make_unique<Backend>(*this, levels.empty() ? (128u << 20) : (16u << 20)));
    }
    return *levels[i];
}

Cpu::Cpu(u32 tid, u32 stack_size, bool audio) : thread_id(tid), audio_thread(audio) {
    init_globals();
    active = &level(0);
    stack_top = mem::alloc_stack(stack_size);
    set_sp(stack_top);
    errno_addr = mem::calloc(1, 4);
}

Cpu::~Cpu() { mem::free(errno_addr); }

u32 Cpu::r(int n) { return active->jit->Regs()[n]; }
void Cpu::set_r(int n, u32 v) { active->jit->Regs()[n] = v; }
u32 Cpu::sp() { return r(13); }
void Cpu::set_sp(u32 v) { set_r(13, v); }
u32 Cpu::lr() { return r(14); }
void Cpu::set_lr(u32 v) { set_r(14, v); }
u32 Cpu::pc() { return r(15); }
u64 Cpu::d(int n) {
    auto& e = active->jit->ExtRegs();
    return e[n * 2] | ((u64)e[n * 2 + 1] << 32);
}
void Cpu::set_d(int n, u64 v) {
    auto& e = active->jit->ExtRegs();
    e[n * 2] = (u32)v;
    e[n * 2 + 1] = (u32)(v >> 32);
}

u32 Cpu::arg(int i) {
    if (i < 4) return r(i);
    return mem::r32(sp() + (i - 4) * 4);
}

void Cpu::request_exit(u32 value) {
    exiting = true;
    exit_value = value;
    active->jit->HaltExecution();
}

u64 Cpu::call64(u32 fn, std::initializer_list<u32> args) {
    Cpu* prev_current = current;
    current = this;
    Backend* caller = active;
    const bool nested = depth > 0;
    Backend& b = level(depth);

    u32 s = (nested ? caller->jit->Regs()[13] : stack_top) - 64;
    const int n = (int)args.size();
    if (n > 4) s -= (n - 4) * 4;
    s &= ~7u;
    auto& regs = b.jit->Regs();
    int i = 0;
    for (u32 a : args) {
        if (i < 4) regs[i] = a;
        else mem::w32(s + (i - 4) * 4, a);
        i++;
    }
    regs[13] = s;
    regs[14] = hle::RET_MAGIC;
    regs[15] = fn & ~1u;
    b.jit->SetCpsr((fn & 1) ? 0x30 : 0x10);  // Thumb bit from the target address
    b.jit->SetFpscr(caller->jit->Fpscr());

    active = &b;
    depth++;
    b.returned = false;
    do {
        b.jit->Run();  // may also return when the tick budget runs out; then just continue
    } while (!b.returned && !exiting);
    b.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    depth--;
    u64 result = regs[0] | ((u64)regs[1] << 32);
    active = caller;
    current = prev_current;
    if (exiting && nested) caller->jit->HaltExecution();  // keep unwinding towards the thread's top level
    return result;
}

u32 Cpu::call(u32 fn, std::initializer_list<u32> args) { return (u32)call64(fn, args); }

void Cpu::dump_state(const char* why) {
    LOGE("==== thread %u: %s ====", thread_id, why);
    for (int i = 0; i < 16; i += 4)
        LOGE("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r(i), i + 1, r(i + 1), i + 2, r(i + 2), i + 3, r(i + 3));
    u32 cpsr = active->jit->Cpsr();
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
        if (!mem::valid(a, 4)) break;
        u32 v = mem::r32(a);
        Module* m = loader::module_at(v);
        if (m && (v & ~1u) - m->base < m->text_end) {
            LOGE("  stack[%04x] %s", a - s, symbolize(v).c_str());
            found++;
        }
    }
}
