// Core libc: memory, strings, ctype, conversions, process control, misc.
#include "hle_common.h"
#include "../elf_loader.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

using namespace hle;
using mem::ptr;
using mem::str;

namespace hle {
void set_errno(Cpu& c, int e) { mem::w32(c.errno_addr, (u32)e); }

void init_libc_data() {
    // BSD/bionic ctype table: _ctype_ points at a 257-entry table indexed by (c + 1).
    enum { U = 1, L = 2, N = 4, S = 8, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    u32 table = mem::calloc(1, 260);
    for (int ch = 0; ch < 256; ch++) {
        u8 f = 0;
        if (ch < 128) {
            if (isupper(ch)) f |= U;
            if (islower(ch)) f |= L;
            if (isdigit(ch)) f |= N;
            if (isspace(ch)) f |= S;
            if (ispunct(ch)) f |= P;
            if (iscntrl(ch)) f |= C;
            if (isxdigit(ch) && !isdigit(ch)) f |= X;
            if (ch == ' ') f |= B;
        }
        mem::w8(table + 1 + ch, f);
    }
    u32 ctype_ptr = mem::malloc(4);
    mem::w32(ctype_ptr, table);
    register_data("_ctype_", ctype_ptr);

    u32 guard = mem::malloc(4);
    mem::w32(guard, 0xC0DEF00D);
    register_data("__stack_chk_guard", guard);
}
}  // namespace hle

// ----------------------------------------------------------------------------
// Heap
// ----------------------------------------------------------------------------
HLE(malloc) { c.ret(mem::malloc(c.r(0))); }
HLE(calloc) { c.ret(mem::calloc(c.r(0), c.r(1))); }
HLE(realloc) { c.ret(mem::realloc(c.r(0), c.r(1))); }
HLE(free) { mem::free(c.r(0)); }
HLE(_ZdlPv) { mem::free(c.r(0)); }        // operator delete(void*)
HLE(_ZdaPv) { mem::free(c.r(0)); }        // operator delete[](void*)
HLE(_Znwj) { c.ret(mem::malloc(c.r(0))); } // operator new(size_t)
HLE(_Znaj) { c.ret(mem::malloc(c.r(0))); } // operator new[](size_t)
HLE(memalign) {
    // Over-allocate and keep alignment simple: our heap is 16-byte aligned already.
    u32 align = c.r(0), size = c.r(1);
    if (align <= 16) { c.ret(mem::malloc(size)); return; }
    LOGE("memalign(%u) > 16 not supported", align);
    c.ret(0);
}

// ----------------------------------------------------------------------------
// Memory / strings
// ----------------------------------------------------------------------------
HLE(memcpy) { memcpy(ptr(c.r(0)), ptr(c.r(1)), c.r(2)); }  // r0 already holds dst
HLE(memmove) { memmove(ptr(c.r(0)), ptr(c.r(1)), c.r(2)); }
HLE(memset) { memset(ptr(c.r(0)), (int)c.r(1), c.r(2)); }
HLE(memcmp) { c.ret((u32)memcmp(ptr(c.r(0)), ptr(c.r(1)), c.r(2))); }
HLE(memchr) {
    const void* r = memchr(ptr(c.r(0)), (int)c.r(1), c.r(2));
    c.ret(mem::guest(r));
}
HLE(memmem) {
    const u8* h = ptr(c.r(0));
    u32 hl = c.r(1);
    const u8* n = ptr(c.r(2));
    u32 nl = c.r(3);
    if (nl == 0) { c.ret(c.r(0)); return; }
    for (u32 i = 0; i + nl <= hl; i++)
        if (memcmp(h + i, n, nl) == 0) { c.ret(c.r(0) + i); return; }
    c.ret(0);
}
HLE(strlen) { c.ret((u32)strlen(str(c.r(0)))); }
HLE(strcmp) { c.ret((u32)strcmp(str(c.r(0)), str(c.r(1)))); }
HLE(strncmp) { c.ret((u32)strncmp(str(c.r(0)), str(c.r(1)), c.r(2))); }
HLE(strcasecmp) { c.ret((u32)_stricmp(str(c.r(0)), str(c.r(1)))); }
HLE(strncasecmp) { c.ret((u32)_strnicmp(str(c.r(0)), str(c.r(1)), c.r(2))); }
HLE(strcoll) { c.ret((u32)strcmp(str(c.r(0)), str(c.r(1)))); }
HLE(strcpy) { strcpy(ptr<char>(c.r(0)), str(c.r(1))); }
HLE(strncpy) { strncpy(ptr<char>(c.r(0)), str(c.r(1)), c.r(2)); }
HLE(stpcpy) {
    size_t n = strlen(str(c.r(1)));
    memcpy(ptr(c.r(0)), str(c.r(1)), n + 1);
    c.ret(c.r(0) + (u32)n);
}
HLE(strcat) { strcat(ptr<char>(c.r(0)), str(c.r(1))); }
HLE(strncat) { strncat(ptr<char>(c.r(0)), str(c.r(1)), c.r(2)); }
HLE(strchr) { c.ret(mem::guest(strchr(str(c.r(0)), (int)c.r(1)))); }
HLE(strrchr) { c.ret(mem::guest(strrchr(str(c.r(0)), (int)c.r(1)))); }
HLE(strstr) { c.ret(mem::guest(strstr(str(c.r(0)), str(c.r(1))))); }
HLE(strpbrk) { c.ret(mem::guest(strpbrk(str(c.r(0)), str(c.r(1))))); }
HLE(strspn) { c.ret((u32)strspn(str(c.r(0)), str(c.r(1)))); }
HLE(strcspn) { c.ret((u32)strcspn(str(c.r(0)), str(c.r(1)))); }
HLE(strdup) { c.ret(mem::strdup(str(c.r(0)))); }
HLE(strxfrm) {
    size_t n = strlen(str(c.r(1)));
    if (c.r(2) > n) memcpy(ptr(c.r(0)), str(c.r(1)), n + 1);
    c.ret((u32)n);
}
static thread_local u32 t_strtok_save;
HLE(strtok) {
    u32 s = c.r(0) ? c.r(0) : t_strtok_save;
    const char* delim = str(c.r(1));
    if (!s) { c.ret(0); return; }
    s += (u32)strspn(str(s), delim);
    if (!mem::r8(s)) { t_strtok_save = 0; c.ret(0); return; }
    u32 e = s + (u32)strcspn(str(s), delim);
    if (mem::r8(e)) { mem::w8(e, 0); t_strtok_save = e + 1; }
    else t_strtok_save = 0;
    c.ret(s);
}
HLE(strerror) {
    static u32 buf = mem::malloc(256);
    snprintf(ptr<char>(buf), 256, "error %d", (int)c.r(0));
    c.ret(buf);
}

// ----------------------------------------------------------------------------
// ctype (C locale). Bionic's _ctype_ table is exported as data (see init_libc_data).
// ----------------------------------------------------------------------------
static int ch(Cpu& c) { return (int)c.r(0); }
static bool in_range(int v) { return v >= -1 && v <= 255; }
#define CTYPE_FN(n) HLE(n) { int v = ch(c); c.ret(in_range(v) && v >= 0 ? (::n(v) ? 1 : 0) : 0); }
CTYPE_FN(isalnum)
CTYPE_FN(isalpha)
CTYPE_FN(iscntrl)
CTYPE_FN(isdigit)
CTYPE_FN(isgraph)
CTYPE_FN(islower)
CTYPE_FN(isprint)
CTYPE_FN(ispunct)
CTYPE_FN(isspace)
CTYPE_FN(isupper)
CTYPE_FN(isxdigit)
HLE(tolower) { int v = ch(c); c.ret((u32)(v >= 0 && v <= 255 ? ::tolower(v) : v)); }
HLE(toupper) { int v = ch(c); c.ret((u32)(v >= 0 && v <= 255 ? ::toupper(v) : v)); }

// ----------------------------------------------------------------------------
// Conversions
// ----------------------------------------------------------------------------
HLE(atoi) { c.ret((u32)atoi(str(c.r(0)))); }
HLE(atol) { c.ret((u32)atol(str(c.r(0)))); }
HLE(atof) { c.retd(atof(str(c.r(0)))); }
HLE(strtol) {
    char* end;
    long v = strtol(str(c.r(0)), &end, (int)c.r(2));
    if (c.r(1)) mem::w32(c.r(1), mem::guest(end));
    c.ret((u32)v);
}
HLE(strtoul) {
    char* end;
    unsigned long v = strtoul(str(c.r(0)), &end, (int)c.r(2));
    if (c.r(1)) mem::w32(c.r(1), mem::guest(end));
    c.ret((u32)v);
}
HLE(strtod) {
    char* end;
    double v = strtod(str(c.r(0)), &end);
    if (c.r(1)) mem::w32(c.r(1), mem::guest(end));
    c.retd(v);
}
HLE(strtof) {
    char* end;
    float v = strtof(str(c.r(0)), &end);
    if (c.r(1)) mem::w32(c.r(1), mem::guest(end));
    c.retf(v);
}

// ----------------------------------------------------------------------------
// Random numbers
// ----------------------------------------------------------------------------
static u64 g_rand48 = 0x1234ABCD330Eull;
HLE(rand) { c.ret((u32)(::rand() & 0x7fffffff)); }
HLE(srand) { ::srand(c.r(0)); }
HLE(srand48) { g_rand48 = ((u64)c.r(0) << 16) | 0x330E; }
HLE(lrand48) {
    g_rand48 = (0x5DEECE66Dull * g_rand48 + 0xB) & ((1ull << 48) - 1);
    c.ret((u32)(g_rand48 >> 17));
}

// ----------------------------------------------------------------------------
// qsort: comparator runs as a nested guest call
// ----------------------------------------------------------------------------
HLE(qsort) {
    u32 base = c.r(0), n = c.r(1), size = c.r(2), cmp = c.r(3);
    if (n < 2) return;
    std::vector<u32> idx(n);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    std::vector<u8> copy(ptr(base), ptr(base) + (size_t)n * size);
    u32 tmp_a = mem::malloc(size), tmp_b = mem::malloc(size);
    std::stable_sort(idx.begin(), idx.end(), [&](u32 x, u32 y) {
        memcpy(ptr(tmp_a), copy.data() + (size_t)x * size, size);
        memcpy(ptr(tmp_b), copy.data() + (size_t)y * size, size);
        return (s32)c.call(cmp, {tmp_a, tmp_b}) < 0;
    });
    for (u32 i = 0; i < n; i++) memcpy(ptr(base + i * size), copy.data() + (size_t)idx[i] * size, size);
    mem::free(tmp_a);
    mem::free(tmp_b);
}

// ----------------------------------------------------------------------------
// setjmp / longjmp. The thunk ends in "bx lr", so longjmp works by restoring
// registers (including lr) and letting the thunk "return" into setjmp's caller.
// Layout (private to us): r4-r11, sp, lr, d8-d15.
// ----------------------------------------------------------------------------
HLE(setjmp) {
    u32 buf = c.r(0);
    for (int i = 4; i <= 11; i++) mem::w32(buf + (i - 4) * 4, c.r(i));
    mem::w32(buf + 32, c.sp());
    mem::w32(buf + 36, c.lr());
    for (int i = 0; i < 8; i++) {
        u64 v = c.d(8 + i);
        memcpy(ptr(buf + 40 + i * 8), &v, 8);
    }
    c.ret(0);
}
HLE(longjmp) {
    u32 buf = c.r(0), val = c.r(1);
    for (int i = 4; i <= 11; i++) c.set_r(i, mem::r32(buf + (i - 4) * 4));
    c.set_sp(mem::r32(buf + 32));
    c.set_lr(mem::r32(buf + 36));
    for (int i = 0; i < 8; i++) {
        u64 v;
        memcpy(&v, ptr(buf + 40 + i * 8), 8);
        c.set_d(8 + i, v);
    }
    c.ret(val ? val : 1);
}
HLE(_setjmp) { hle_fn_setjmp(c); }
HLE(_longjmp) { hle_fn_longjmp(c); }

// ----------------------------------------------------------------------------
// Process control / C++ runtime support
// ----------------------------------------------------------------------------
HLE(abort) {
    c.dump_state("abort() called");
    fatal("guest called abort()");
}
HLE(exit) {
    LOGI("guest called exit(%d)", (int)c.r(0));
    hle::dump_stats();
    fflush(stdout);
    ExitProcess(c.r(0));
}
HLE(__assert2) {
    LOGE("assertion failed: %s:%d %s: %s", str(c.r(0)), (int)c.r(1), str(c.r(2)), str(c.r(3)));
    c.dump_state("assert");
    fatal("guest assertion");
}
HLE(__stack_chk_fail) {
    c.dump_state("stack smashing detected");
    fatal("__stack_chk_fail");
}
HLE(__cxa_pure_virtual) {
    c.dump_state("pure virtual call");
    fatal("pure virtual call");
}
HLE(__cxa_atexit) { c.ret(0); }
HLE(__aeabi_atexit) { c.ret(0); }
HLE(atexit) { c.ret(0); }
HLE(__cxa_finalize) {}
HLE(__errno) { c.ret(c.errno_addr); }
HLE(raise) { LOGI("raise(%d) ignored", (int)c.r(0)); c.ret(0); }
HLE(sigaction) { c.ret(0); }
HLE(getenv) { c.ret(0); }
HLE(setlocale) {
    static u32 s = mem::strdup("C");
    c.ret(s);
}
HLE(localeconv) {
    // struct lconv: first member decimal_point, then thousands_sep, grouping, ... (all char*)
    static u32 lc = [] {
        u32 p = mem::calloc(1, 96);
        u32 dot = mem::strdup("."), empty = mem::strdup("");
        mem::w32(p, dot);
        for (u32 off = 4; off < 40; off += 4) mem::w32(p + off, empty);
        for (u32 off = 40; off < 56; off++) mem::w8(p + off, 127);  // CHAR_MAX for numeric fields
        return p;
    }();
    c.ret(lc);
}
HLE(__gnu_Unwind_Find_exidx) {
    Module* m = loader::module_at(c.r(0));
    if (!m || !m->exidx) { c.ret(0); return; }
    mem::w32(c.r(1), m->exidx_count);
    c.ret(m->exidx);
}
HLE(__kuser_get_tls) { c.ret(c.kuser_tls); }
HLE(syscall) {
    u32 nr = c.r(0);
    if (nr == 224) { c.ret(c.thread_id); return; }  // gettid
    LOGI("syscall(%u) not supported", nr);
    c.ret((u32)-1);
}

// ----------------------------------------------------------------------------
// Dynamic linking: resolve against loaded modules + host functions
// ----------------------------------------------------------------------------
static const char* g_dlerror = nullptr;
HLE(dlopen) {
    const char* name = str(c.r(0));
    LOGI("dlopen(\"%s\")", name ? name : "(null)");
    c.ret(0x1001);  // fake handle; dlsym searches everything
}
HLE(dlsym) {
    const char* name = str(c.r(1));
    u32 v = loader::find_symbol(name);
    if (!v && hle::is_implemented(name)) v = hle::thunk_for(name);
    LOGI("dlsym(\"%s\") -> 0x%08x", name, v);
    if (!v) g_dlerror = "symbol not found";
    c.ret(v);
}
HLE(dlclose) { c.ret(0); }
HLE(dlerror) {
    static u32 s = mem::strdup("symbol not found");
    c.ret(g_dlerror ? s : 0);
    g_dlerror = nullptr;
}
