#pragma once
// Read-only access to files inside the game's APK (a zip archive), or an unpacked copy of it.
#include "common.h"
#include <string>
#include <vector>

namespace apk {

bool open(const std::string& path);
bool open_dir(const std::string& dir);  // a folder laid out like the APK (lib/..., assets/...)
bool read(const std::string& name, std::vector<u8>& out);  // e.g. "lib/armeabi-v7a/libParsons.so"
bool exists(const std::string& name);
u32 crc32_of(const std::string& name);                     // CRC stored in the zip directory (0 if missing)
std::vector<std::string> list(const std::string& dir);     // file names directly inside dir (no trailing '/')

}  // namespace apk
