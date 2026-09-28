// printf / scanf families. Guest varargs are read with Args (registers + stack, or a va_list).
#include "hle_common.h"
#include <cstring>
#include <vector>

namespace hle {

namespace {

struct Spec {
    std::string flags;
    int width = -1;        // -2 means '*'
    int precision = -1;    // -2 means '*'
    std::string length;    // "", "h", "hh", "l", "ll", "L", "z", "j", "t"
    char conv = 0;
};

// Parses one conversion starting after '%'. Returns pointer past the conversion character.
const char* parse_spec(const char* p, Spec& s) {
    while (*p && strchr("-+ #0'", *p)) s.flags += *p++;
    if (*p == '*') { s.width = -2; p++; }
    else if (isdigit((u8)*p)) { s.width = 0; while (isdigit((u8)*p)) s.width = s.width * 10 + (*p++ - '0'); }
    if (*p == '.') {
        p++;
        if (*p == '*') { s.precision = -2; p++; }
        else { s.precision = 0; while (isdigit((u8)*p)) s.precision = s.precision * 10 + (*p++ - '0'); }
    }
    while (*p && strchr("hlLqjzt", *p)) s.length += *p++;
    s.conv = *p ? *p++ : 0;
    return p;
}

bool is_64bit_int(const Spec& s) { return s.length == "ll" || s.length == "q" || s.length == "j"; }

template <class GetFmtChar>
std::string format_impl(GetFmtChar next, Args& a) {
    std::string out;
    char buf[512];
    std::string fmt_chunk;
    // Collect the whole format string first (narrow; wide formats are converted by the caller).
    std::string f;
    for (u32 ch; (ch = next()) != 0;) f.push_back((char)ch);
    const char* p = f.c_str();
    while (*p) {
        if (*p != '%') { out += *p++; continue; }
        if (p[1] == '%') { out += '%'; p += 2; continue; }
        Spec s;
        p = parse_spec(p + 1, s);
        if (s.width == -2) { int w = a.s32v(); if (w < 0) { s.flags += '-'; w = -w; } s.width = w; }
        if (s.precision == -2) { int pr = a.s32v(); s.precision = pr < 0 ? -1 : pr; }
        std::string hf = "%" + s.flags;
        if (s.width >= 0) hf += std::to_string(s.width);
        if (s.precision >= 0) hf += "." + std::to_string(s.precision);
        switch (s.conv) {
            case 'd': case 'i': {
                if (is_64bit_int(s)) { snprintf(buf, sizeof buf, (hf + "lld").c_str(), (long long)a.s64v()); }
                else {
                    s32 v = a.s32v();
                    if (s.length == "h") v = (s16)v;
                    else if (s.length == "hh") v = (s8)v;
                    snprintf(buf, sizeof buf, (hf + "d").c_str(), v);
                }
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                if (is_64bit_int(s)) { snprintf(buf, sizeof buf, (hf + "ll" + s.conv).c_str(), (unsigned long long)a.u64v()); }
                else {
                    u32 v = a.u32v();
                    if (s.length == "h") v = (u16)v;
                    else if (s.length == "hh") v = (u8)v;
                    snprintf(buf, sizeof buf, (hf + s.conv).c_str(), v);
                }
                out += buf;
                break;
            }
            case 'c': {
                u32 v = a.u32v();
                if (s.length == "l") { std::u32string w(1, (char32_t)v); snprintf(buf, sizeof buf, (hf + "s").c_str(), mem::utf8(w).c_str()); }
                else snprintf(buf, sizeof buf, (hf + "c").c_str(), (int)(u8)v);
                out += buf;
                break;
            }
            case 's': case 'S': {
                u32 g = a.u32v();
                std::string str;
                if (!g) str = "(null)";
                else if (s.conv == 'S' || s.length == "l") str = mem::wstr_utf8(g);
                else str = mem::str(g);
                if (s.precision >= 0 && (size_t)s.precision < str.size()) str.resize(s.precision);
                int pad = s.width > (int)str.size() ? s.width - (int)str.size() : 0;
                if (s.flags.find('-') != std::string::npos) out += str + std::string(pad, ' ');
                else out += std::string(pad, ' ') + str;
                break;
            }
            case 'p': {
                snprintf(buf, sizeof buf, "0x%x", a.u32v());
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double d = a.f64();  // float varargs are promoted to double
                snprintf(buf, sizeof buf, (hf + s.conv).c_str(), d);
                out += buf;
                break;
            }
            case 'n': {
                u32 g = a.u32v();
                if (g) mem::w32(g, (u32)out.size());
                break;
            }
            default:
                LOGE("printf: unsupported conversion '%c' in \"%s\"", s.conv ? s.conv : '?', f.c_str());
                return out;
        }
    }
    return out;
}

}  // namespace

std::string format(const char* fmt, Args& args) {
    if (!fmt) return "(null)";
    const u8* p = (const u8*)fmt;
    return format_impl([&]() -> u32 { return *p ? *p++ : 0; }, args);
}

std::string wformat(u32 wfmt, Args& args) {
    // Wide formats: convert the format to narrow (formats are ASCII in practice), and treat %s as wide.
    std::u32string w = mem::wstr(wfmt);
    std::string narrow;
    for (char32_t ch : w) narrow.push_back(ch < 0x80 ? (char)ch : '?');
    // In wide printf, %s means a wide string; map it to %S.
    std::string fixed;
    for (size_t i = 0; i < narrow.size(); i++) {
        fixed += narrow[i];
        if (narrow[i] == '%') {
            size_t j = i + 1;
            while (j < narrow.size() && strchr("-+ #0'.*0123456789hlLqjzt", narrow[j])) j++;
            if (j < narrow.size() && narrow[j] == 's') {
                fixed += narrow.substr(i + 1, j - i - 1) + "S";
                i = j;
            }
        }
    }
    return format(fixed.c_str(), args);
}

int scan(const char* input, const char* fmt, Args& a) {
    int assigned = 0;
    const char* in = input;
    const char* p = fmt;
    while (*p) {
        if (isspace((u8)*p)) {
            while (isspace((u8)*in)) in++;
            p++;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            char want = *p == '%' ? '%' : *p;
            p += (*p == '%') ? 2 : 1;
            if (*in != want) return assigned;
            in++;
            continue;
        }
        // Build a host format for just this conversion and use host sscanf with %n.
        const char* start = p;
        p++;
        bool suppress = false;
        if (*p == '*') { suppress = true; p++; }
        while (isdigit((u8)*p)) p++;
        std::string len;
        while (*p && strchr("hlLqjzt", *p)) len += *p++;
        char conv = *p;
        std::string set;
        if (conv == '[') {
            const char* q = p + 1;
            if (*q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') q++;
            set.assign(p, q + 1 - p);
            p = q + 1;
        } else {
            p++;
        }
        std::string hf(start, p - start);
        if (!*in && conv != 'n') return assigned ? assigned : -1;
        int consumed = 0;
        if (conv == 'n') {
            if (!suppress) mem::w32(a.u32v(), (u32)(in - input));
            continue;
        }
        if (suppress) {
            int r = sscanf(in, (hf + "%n").c_str(), &consumed);
            if (r < 0 || consumed == 0) return assigned;
            in += consumed;
            continue;
        }
        u32 dst = a.u32v();
        int r;
        // Pointer targets live in guest memory which is directly addressable; sizes match (ints 4 bytes,
        // long 4 bytes on ARM vs 4 on Win64 - OK; long long 8 both). Wide targets need conversion.
        if ((conv == 's' || conv == 'c' || conv == '[') && (len == "l")) {
            char tmp[1024];
            std::string nf = hf;
            nf.erase(nf.find('l'), 1);
            r = sscanf(in, (nf + "%n").c_str(), tmp, &consumed);
            if (r == 1) {
                size_t n = strlen(tmp);
                for (size_t i = 0; i < n; i++) mem::w32(dst + (u32)i * 4, (u8)tmp[i]);
                if (conv != 'c') mem::w32(dst + (u32)n * 4, 0);
            }
        } else if (conv == 'p') {
            unsigned int v;
            r = sscanf(in, "%x%n", &v, &consumed);
            if (r == 1) mem::w32(dst, v);
        } else {
            r = sscanf(in, (hf + "%n").c_str(), mem::ptr(dst), &consumed);
        }
        if (r != 1) return assigned ? assigned : (r == EOF ? -1 : 0);
        assigned++;
        in += consumed;
    }
    return assigned;
}

}  // namespace hle

// ---------------------------------------------------------------------------
// Guest-visible printf/scanf family
// ---------------------------------------------------------------------------
using namespace hle;

static u32 write_bounded(u32 dst, u32 size, const std::string& s) {
    if (dst && size) {
        u32 n = std::min<u32>((u32)s.size(), size - 1);
        memcpy(mem::ptr(dst), s.data(), n);
        mem::w8(dst + n, 0);
    }
    return (u32)s.size();
}

HLE(sprintf) {
    RegArgs a(c, 2);
    std::string s = format(mem::str(c.r(1)), a);
    memcpy(mem::ptr(c.r(0)), s.c_str(), s.size() + 1);
    c.ret((u32)s.size());
}
HLE(vsprintf) {
    VaArgs a(c.r(2));
    std::string s = format(mem::str(c.r(1)), a);
    memcpy(mem::ptr(c.r(0)), s.c_str(), s.size() + 1);
    c.ret((u32)s.size());
}
HLE(snprintf) {
    RegArgs a(c, 3);
    c.ret(write_bounded(c.r(0), c.r(1), format(mem::str(c.r(2)), a)));
}
HLE(vsnprintf) {
    VaArgs a(c.r(3));
    c.ret(write_bounded(c.r(0), c.r(1), format(mem::str(c.r(2)), a)));
}
HLE(sscanf) {
    RegArgs a(c, 2);
    c.ret((u32)scan(mem::str(c.r(0)), mem::str(c.r(1)), a));
}
HLE(vsscanf) {
    VaArgs a(c.r(2));
    c.ret((u32)scan(mem::str(c.r(0)), mem::str(c.r(1)), a));
}
HLE(swscanf) {
    RegArgs a(c, 2);
    std::string in = mem::wstr_utf8(c.r(0)), f = mem::wstr_utf8(c.r(1));
    c.ret((u32)scan(in.c_str(), f.c_str(), a));
}
HLE(printf) {
    RegArgs a(c, 1);
    std::string s = format(mem::str(c.r(0)), a);
    fputs(s.c_str(), stdout);
    c.ret((u32)s.size());
}
