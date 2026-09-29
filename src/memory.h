#pragma once
// Guest (32-bit ARM) address space.
//
// One contiguous host allocation backs guest addresses [ARENA_BASE, ARENA_END).
// Host pointer = g_host_base + guest address, so buffers can be handed to host APIs directly.
#include "common.h"
#include <cstring>

namespace mem {

constexpr u32 THUNK_BASE = 0x0F000000;  // HLE import thunks (mapped separately)
constexpr u32 THUNK_SIZE = 0x00100000;
constexpr u32 ARENA_BASE = 0x10000000;
constexpr u32 LIB_BASE   = 0x10000000;  // shared libraries
constexpr u32 HEAP_BASE  = 0x14000000;  // guest malloc
constexpr u32 HEAP_END   = 0x60000000;
constexpr u32 STACK_BASE = 0x60000000;  // thread stacks
constexpr u32 ARENA_END  = 0x70000000;
constexpr u32 KUSER_PAGE = 0xFFFF0000;  // Linux kernel user helpers (__kuser_get_tls)

extern u8* g_host_base;
extern u8* g_arena;

void init();

inline bool valid(u32 g, u32 len = 1) { return g >= ARENA_BASE && (u64)g + len <= ARENA_END; }

template <class T = u8>
inline T* ptr(u32 g) { return g ? reinterpret_cast<T*>(g_host_base + g) : nullptr; }

inline u32 guest(const void* h) {
    return h ? (u32)((const u8*)h - g_host_base) : 0;
}

inline u32 r32(u32 g) { u32 v; memcpy(&v, ptr(g), 4); return v; }
inline u16 r16(u32 g) { u16 v; memcpy(&v, ptr(g), 2); return v; }
inline u8 r8(u32 g) { return *ptr(g); }
inline void w32(u32 g, u32 v) { memcpy(ptr(g), &v, 4); }
inline void w16(u32 g, u16 v) { memcpy(ptr(g), &v, 2); }
inline void w8(u32 g, u8 v) { *ptr(g) = v; }
inline const char* str(u32 g) { return g ? ptr<const char>(g) : nullptr; }

// Guest heap
u32 malloc(u32 size);
u32 calloc(u32 n, u32 size);
u32 realloc(u32 p, u32 size);
void free(u32 p);
u32 usable_size(u32 p);
u32 strdup(const char* s);
u32 alloc_stack(u32 size);  // returns stack *top*
void free_stack(u32 top);   // top as returned by alloc_stack

// Guest "wide" strings are UTF-32 (Android wchar_t is 4 bytes).
std::u32string wstr(u32 g);
std::string wstr_utf8(u32 g);
std::string utf8(const std::u32string& s);

}  // namespace mem
