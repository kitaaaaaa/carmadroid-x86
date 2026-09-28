// Wide-character functions. Android's wchar_t is 32-bit, so these operate on u32 units.
#include "hle_common.h"
#include <cwctype>

using mem::r32;
using mem::w32;

static u32 wlen(u32 s) { u32 n = 0; while (r32(s + n * 4)) n++; return n; }
static int wcmp(u32 a, u32 b, u32 n = 0xffffffff) {
    for (u32 i = 0; i < n; i++) {
        u32 x = r32(a + i * 4), y = r32(b + i * 4);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

HLE(wcslen) { c.ret(wlen(c.r(0))); }
HLE(wcscmp) { c.ret((u32)wcmp(c.r(0), c.r(1))); }
HLE(wcscoll) { c.ret((u32)wcmp(c.r(0), c.r(1))); }
HLE(wcsncmp) { c.ret((u32)wcmp(c.r(0), c.r(1), c.r(2))); }
HLE(wcscpy) {
    u32 d = c.r(0), s = c.r(1);
    u32 n = wlen(s);
    memmove(mem::ptr(d), mem::ptr(s), (n + 1) * 4);
}
HLE(wcsncpy) {
    u32 d = c.r(0), s = c.r(1), n = c.r(2);
    u32 i = 0;
    for (; i < n; i++) { u32 v = r32(s + i * 4); w32(d + i * 4, v); if (!v) break; }
    for (; i < n; i++) w32(d + i * 4, 0);
}
HLE(wcscat) {
    u32 d = c.r(0), s = c.r(1);
    u32 dl = wlen(d), sl = wlen(s);
    memmove(mem::ptr(d + dl * 4), mem::ptr(s), (sl + 1) * 4);
}
HLE(wcsstr) {
    u32 h = c.r(0), n = c.r(1);
    u32 nl = wlen(n);
    if (!nl) { c.ret(h); return; }
    for (u32 p = h; r32(p); p += 4) {
        u32 i = 0;
        while (i < nl && r32(p + i * 4) == r32(n + i * 4)) i++;
        if (i == nl) { c.ret(p); return; }
    }
    c.ret(0);
}
HLE(wcsxfrm) {
    u32 n = wlen(c.r(1));
    if (c.r(2) > n) memcpy(mem::ptr(c.r(0)), mem::ptr(c.r(1)), (n + 1) * 4);
    c.ret(n);
}
HLE(wmemcpy) { memcpy(mem::ptr(c.r(0)), mem::ptr(c.r(1)), (size_t)c.r(2) * 4); }
HLE(wmemmove) { memmove(mem::ptr(c.r(0)), mem::ptr(c.r(1)), (size_t)c.r(2) * 4); }
HLE(wmemset) { for (u32 i = 0; i < c.r(2); i++) w32(c.r(0) + i * 4, c.r(1)); }
HLE(wmemcmp) {
    for (u32 i = 0; i < c.r(2); i++) {
        u32 x = r32(c.r(0) + i * 4), y = r32(c.r(1) + i * 4);
        if (x != y) { c.ret(x < y ? (u32)-1 : 1); return; }
    }
    c.ret(0);
}
HLE(wmemchr) {
    for (u32 i = 0; i < c.r(2); i++)
        if (r32(c.r(0) + i * 4) == c.r(1)) { c.ret(c.r(0) + i * 4); return; }
    c.ret(0);
}

// Character classification: treat as Unicode code points via the host (UTF-16 range).
#define WCLASS(n) HLE(n) { u32 v = c.r(0); c.ret(v < 0x10000 && std::n((wint_t)v) ? 1 : 0); }
WCLASS(iswdigit)
WCLASS(iswspace)
WCLASS(iswalpha)
WCLASS(iswupper)
WCLASS(iswlower)
WCLASS(iswalnum)
WCLASS(iswpunct)
WCLASS(iswprint)
HLE(towlower) { u32 v = c.r(0); c.ret(v < 0x10000 ? (u32)std::towlower((wint_t)v) : v); }
HLE(towupper) { u32 v = c.r(0); c.ret(v < 0x10000 ? (u32)std::towupper((wint_t)v) : v); }

// wctype()/iswctype(): our own class ids
static const char* kClasses[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                 "lower", "print", "punct", "space", "upper", "xdigit"};
HLE(wctype) {
    const char* n = mem::str(c.r(0));
    for (u32 i = 1; i < 13; i++)
        if (!strcmp(n, kClasses[i])) { c.ret(i); return; }
    c.ret(0);
}
HLE(iswctype) {
    u32 v = c.r(0), cls = c.r(1);
    if (v >= 0x10000) { c.ret(0); return; }
    wint_t w = (wint_t)v;
    int r = 0;
    switch (cls) {
        case 1: r = iswalnum(w); break;
        case 2: r = iswalpha(w); break;
        case 3: r = w == L' ' || w == L'\t'; break;
        case 4: r = iswcntrl(w); break;
        case 5: r = iswdigit(w); break;
        case 6: r = iswgraph(w); break;
        case 7: r = iswlower(w); break;
        case 8: r = iswprint(w); break;
        case 9: r = iswpunct(w); break;
        case 10: r = iswspace(w); break;
        case 11: r = iswupper(w); break;
        case 12: r = iswxdigit(w); break;
    }
    c.ret(r ? 1 : 0);
}

// Multibyte conversions: the game runs in the "C" locale, so bytes map 1:1.
HLE(btowc) { s32 v = (s32)c.r(0); c.ret(v < 0 || v > 255 ? (u32)-1 : (u32)v); }
HLE(wctob) { u32 v = c.r(0); c.ret(v < 256 ? v : (u32)-1); }
HLE(mbrtowc) {
    u32 pwc = c.r(0), s = c.r(1), n = c.r(2);
    if (!s) { c.ret(0); return; }
    if (n == 0) { c.ret((u32)-2); return; }
    u8 b = mem::r8(s);
    if (pwc) w32(pwc, b);
    c.ret(b ? 1 : 0);
}
HLE(wcrtomb) {
    u32 s = c.r(0), wc = c.r(1);
    if (!s) { c.ret(1); return; }
    mem::w8(s, wc < 256 ? (u8)wc : '?');
    c.ret(1);
}
HLE(mbstowcs) {
    u32 d = c.r(0), s = c.r(1), n = c.r(2);
    u32 len = (u32)strlen(mem::str(s));
    if (!d) { c.ret(len); return; }
    u32 i = 0;
    for (; i < n && i < len; i++) w32(d + i * 4, mem::r8(s + i));
    if (i < n) w32(d + i * 4, 0);
    c.ret(i);
}
HLE(wcstombs) {
    u32 d = c.r(0), s = c.r(1), n = c.r(2);
    u32 len = wlen(s);
    if (!d) { c.ret(len); return; }
    u32 i = 0;
    for (; i < n && i < len; i++) { u32 v = r32(s + i * 4); mem::w8(d + i, v < 256 ? (u8)v : '?'); }
    if (i < n) mem::w8(d + i, 0);
    c.ret(i);
}
