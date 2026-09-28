#pragma once
#include "common.h"
#include "memory.h"
#include <memory>
#include <vector>
#include <functional>
#include <initializer_list>
#include <string>
#include <atomic>

struct Cpu;
using HleFn = std::function<void(Cpu&)>;

// ---------------------------------------------------------------------------
// HLE (high-level emulation) registry: every import the guest can call is an
// 8-byte thunk "svc #0; bx lr" in the thunk region. The SVC hook looks up the
// thunk index and runs the host implementation.
// ---------------------------------------------------------------------------
namespace hle {

constexpr u32 RET_MAGIC = mem::THUNK_BASE;  // thunk #0: return address for host->guest calls

void init();
u8* thunk_mem();
u32 thunk_for(const std::string& name);       // implemented fn, or an auto-generated "unimplemented" stub
bool is_implemented(const std::string& name);
u32 add_dynamic(const std::string& name, HleFn fn);  // always creates a new thunk
const char* name_of(u32 addr);                 // nullptr if addr is not a thunk
void dispatch(u32 index, Cpu& c);
void dump_stats();

// Replace a guest ARM-mode function with a host handler. The first two instructions are
// relocated into a trampoline (they must not be PC-relative); the returned trampoline address
// calls the original function. Install before the function first runs.
u32 hook_function(u32 addr, const std::string& name, HleFn fn);

// Data imports (e.g. __sF, _ctype_): guest address of host-provided objects
void register_data(const std::string& name, u32 addr);
u32 data_for(const std::string& name);  // 0 if unknown

struct Reg {
    Reg(const char* name, void (*fn)(Cpu&));
};

}  // namespace hle

#define HLE(name)                                    \
    static void hle_fn_##name(Cpu& c);               \
    static hle::Reg hle_reg_##name(#name, hle_fn_##name); \
    static void hle_fn_##name([[maybe_unused]] Cpu& c)

// ---------------------------------------------------------------------------
// Argument readers (AAPCS softfp: floats/doubles travel in core registers)
// ---------------------------------------------------------------------------
class Args {
public:
    virtual ~Args() = default;
    virtual u32 u32v() = 0;
    virtual u64 u64v() = 0;
    s32 s32v() { return (s32)u32v(); }
    s64 s64v() { return (s64)u64v(); }
    float f32() { u32 v = u32v(); float f; memcpy(&f, &v, 4); return f; }
    double f64() { u64 v = u64v(); double d; memcpy(&d, &v, 8); return d; }
};

class RegArgs : public Args {
public:
    RegArgs(Cpu& c, int first_reg = 0);
    u32 u32v() override;
    u64 u64v() override;
private:
    Cpu& c_;
    int ncrn_;
    u32 nsaa_;
};

class VaArgs : public Args {
public:
    explicit VaArgs(u32 ap) : ap_(ap) {}
    u32 u32v() override { u32 v = mem::r32(ap_); ap_ += 4; return v; }
    u64 u64v() override {
        ap_ = (ap_ + 7) & ~7u;
        u64 v = mem::r32(ap_) | ((u64)mem::r32(ap_ + 4) << 32);
        ap_ += 8;
        return v;
    }
private:
    u32 ap_;
};

// ---------------------------------------------------------------------------
// One emulated CPU per guest thread (each runs on its own host thread).
// ---------------------------------------------------------------------------
struct Cpu {
    u32 stack_top = 0;
    u32 errno_addr = 0;
    u32 thread_id = 0;
    u32 kuser_tls = 0;
    int depth = 0;
    bool exiting = false;   // set by pthread_exit / exit
    u32 exit_value = 0;

    static thread_local Cpu* current;

    Cpu(u32 tid, u32 stack_size);
    ~Cpu();

    u32 r(int n);
    void set_r(int n, u32 v);
    u32 sp();
    void set_sp(u32 v);
    u32 lr();
    void set_lr(u32 v);
    u32 pc();
    u64 d(int n);
    void set_d(int n, u64 v);

    u32 arg(int i);  // i-th 32-bit argument (no 64-bit alignment rules)
    void ret(u32 v) { set_r(0, v); }
    void ret64(u64 v) { set_r(0, (u32)v); set_r(1, (u32)(v >> 32)); }
    void retf(float f) { u32 v; memcpy(&v, &f, 4); set_r(0, v); }
    void retd(double d) { u64 v; memcpy(&v, &d, 8); ret64(v); }

    // Call a guest function. Safe to use from inside an HLE handler (nested).
    u32 call(u32 fn, std::initializer_list<u32> args);
    u64 call64(u32 fn, std::initializer_list<u32> args);

    // Stop the current guest thread (pthread_exit): unwinds all nested guest calls.
    void request_exit(u32 value);

    void dump_state(const char* why);
    void backtrace();

    // CPU backend (dynarmic). One JIT per nesting level: host->guest calls made from inside a
    // guest->host call run on the next level, since a JIT cannot be re-entered.
    struct Backend;
    std::vector<std::unique_ptr<Backend>> levels;
    Backend* active = nullptr;  // level whose registers r()/set_r() access
    Backend& level(int i);
};

// Profiler (debug; not supported by the dynarmic backend, kept for command-line compatibility).
namespace profiler {
extern bool g_enabled;
extern std::atomic<bool> g_active;
void report();
}

// Symbolizer hook (implemented by the ELF loader)
std::string symbolize(u32 addr);
