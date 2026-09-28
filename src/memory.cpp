#include "memory.h"
#include <windows.h>
#include <mutex>
#include <vector>

namespace mem {

u8* g_host_base = nullptr;
u8* g_arena = nullptr;

void init() {
    const size_t size = ARENA_END - ARENA_BASE;
    // Prefer an identity mapping (guest address == host address): makes debugging much easier.
    g_arena = (u8*)VirtualAlloc((void*)(uintptr_t)ARENA_BASE, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) {
        g_arena = (u8*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_arena) fatal("could not allocate %zu MB guest arena", size >> 20);
    }
    g_host_base = g_arena - ARENA_BASE;
    LOGI("guest arena at host %p (%s)", g_arena, g_arena == (u8*)(uintptr_t)ARENA_BASE ? "identity" : "offset");
}

// ---------------------------------------------------------------------------
// Heap: size-class free lists on top of a bump allocator. Blocks are never
// returned to the bump region, but freed blocks are reused for the same class.
// ---------------------------------------------------------------------------
namespace {

constexpr u32 HDR = 16;
constexpr u32 MAGIC = 0xA110CA7E;
constexpr u32 FREED = 0xF7EEB10C;

struct Header {
    u32 cls;
    u32 magic;
    u32 capacity;  // usable bytes
    u32 next_free; // valid while on a free list
};

std::mutex g_heap_lock;
u32 g_bump = HEAP_BASE;
std::vector<u32> g_free_heads(256, 0);

u32 class_of(u32 size, u32* cap) {
    if (size == 0) size = 1;
    if (size <= 1024) {
        u32 c = (size + 15) / 16;  // 1..64
        *cap = c * 16;
        return c;
    }
    // Above 1 KB: 4 sub-steps per power of two.
    u32 p = 10;
    while ((1ull << (p + 1)) < size) p++;
    u64 base = 1ull << p;
    u64 step = base / 4;
    u32 sub = (u32)((size - base + step - 1) / step);  // 1..4
    if (sub == 0) sub = 1;
    *cap = (u32)(base + step * sub);
    return 64 + (p - 10) * 4 + sub;
}

Header* hdr(u32 p) { return ptr<Header>(p - HDR); }

}  // namespace

u32 malloc(u32 size) {
    u32 cap;
    u32 c = class_of(size, &cap);
    std::lock_guard<std::mutex> l(g_heap_lock);
    u32 block = g_free_heads[c];
    if (block) {
        Header* h = ptr<Header>(block);
        g_free_heads[c] = h->next_free;
    } else {
        u32 need = HDR + cap;
        if ((u64)g_bump + need > HEAP_END) {
            LOGE("guest heap exhausted (request %u bytes)", size);
            return 0;
        }
        block = g_bump;
        g_bump += (need + 15) & ~15u;
    }
    Header* h = ptr<Header>(block);
    h->cls = c;
    h->magic = MAGIC;
    h->capacity = cap;
    h->next_free = 0;
    return block + HDR;
}

u32 calloc(u32 n, u32 size) {
    u64 total = (u64)n * size;
    if (total > 0x7fffffff) return 0;
    u32 p = malloc((u32)total);
    if (p) memset(ptr(p), 0, (size_t)total);
    return p;
}

void free(u32 p) {
    if (!p) return;
    Header* h = hdr(p);
    if (h->magic != MAGIC) {
        if (h->magic == FREED) LOGE("double free of guest pointer 0x%08x", p);
        else LOGE("free of invalid guest pointer 0x%08x", p);
        return;
    }
    std::lock_guard<std::mutex> l(g_heap_lock);
    h->magic = FREED;
    h->next_free = g_free_heads[h->cls];
    g_free_heads[h->cls] = p - HDR;
}

u32 usable_size(u32 p) { return p ? hdr(p)->capacity : 0; }

u32 realloc(u32 p, u32 size) {
    if (!p) return malloc(size);
    if (size == 0) { free(p); return 0; }
    Header* h = hdr(p);
    if (h->magic != MAGIC) {
        LOGE("realloc of invalid guest pointer 0x%08x", p);
        return 0;
    }
    if (size <= h->capacity) return p;
    u32 n = malloc(size);
    if (!n) return 0;
    memcpy(ptr(n), ptr(p), h->capacity);
    free(p);
    return n;
}

u32 strdup(const char* s) {
    size_t len = strlen(s) + 1;
    u32 g = malloc((u32)len);
    memcpy(ptr(g), s, len);
    return g;
}

// Stacks are carved downwards from ARENA_END; each has a 64 KB unused guard gap.
static std::mutex g_stack_lock;
static u32 g_stack_next = ARENA_END;

u32 alloc_stack(u32 size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    std::lock_guard<std::mutex> l(g_stack_lock);
    u32 top = g_stack_next - 0x10000;
    u32 bottom = top - size;
    if (bottom < STACK_BASE) fatal("out of guest stack space");
    g_stack_next = bottom;
    return top;
}

std::u32string wstr(u32 g) {
    std::u32string s;
    if (!g) return s;
    for (u32 c; (c = r32(g)) != 0; g += 4) s.push_back(c);
    return s;
}

std::string utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string wstr_utf8(u32 g) { return utf8(wstr(g)); }

}  // namespace mem
