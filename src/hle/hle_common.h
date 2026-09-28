#pragma once
#include "../cpu.h"
#include "../memory.h"
#include <string>

namespace hle {

// printf-style formatting reading guest arguments.
std::string format(const char* fmt, Args& args);
std::string wformat(u32 wfmt, Args& args);  // UTF-32 format string, returns UTF-8

// scanf-style parsing writing to guest pointers; returns number of assigned fields (or -1 on input failure).
int scan(const char* input, const char* fmt, Args& args);

void set_errno(Cpu& c, int e);

// Paths: guest (Android) path -> host path.
std::string host_path(const char* guest_path);

struct Config {
    std::string root;        // runtime folder (saves, config)
    std::string obb_host;    // host path of the OBB
    std::string apk;         // host path of the game's APK
    std::string package = "com.stainlessgames.carmageddon";
    int version_code = 507;
};
extern Config g_config;

// Data objects imported by the guest (call before loading libraries)
void init_stdio_data();
void init_libc_data();

// Android paths the game uses
std::string guest_obb_path();
std::string guest_files_dir();
std::string guest_external_dir();

}  // namespace hle
