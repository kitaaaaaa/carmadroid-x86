#pragma once
#include "cpu.h"
#include <string>

namespace debug {
extern std::string g_dump_wad_dir;  // --dump-wad DIR
extern bool g_dump_zones;
extern std::string g_dump_file;
void on_frame(Cpu& c);              // called on the render thread at each eglSwapBuffers
}
