#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Log levels: 0 = errors only, 1 = info, 2 = verbose (every unimplemented call), 3 = trace
extern int g_log_level;

void log_line(const char* tag, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);

#define LOGE(...) log_line("E", __VA_ARGS__)
#define LOGI(...) do { if (g_log_level >= 1) log_line("I", __VA_ARGS__); } while (0)
#define LOGV(...) do { if (g_log_level >= 2) log_line("V", __VA_ARGS__); } while (0)
#define LOGT(...) do { if (g_log_level >= 3) log_line("T", __VA_ARGS__); } while (0)
