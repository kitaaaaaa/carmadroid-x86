#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <unordered_map>

struct Module {
    std::string name;
    u32 base = 0;
    u32 size = 0;
    u32 text_end = 0;  // end of the first (executable) PT_LOAD, relative to base
    std::vector<u32> init_array;
    u32 dt_init = 0;
    u32 exidx = 0, exidx_count = 0;
    std::unordered_map<std::string, u32> exports;          // name -> absolute address (thumb bit kept)
    std::vector<std::pair<u32, std::string>> sorted_syms;  // for symbolization (thumb bit stripped)
    std::unordered_map<u32, u32> sym_sizes;                  // function start -> size
    std::vector<std::string> unresolved;
};

namespace loader {

// Loads an ELF shared object at `base`. Imports resolve against already-loaded
// modules first (in load order), then the HLE registry.
Module* load(const std::string& path, u32 base);
Module* load_image(const std::string& name, const std::vector<u8>& file, u32 base);  // from memory
void run_initializers(Module* m);
u32 find_symbol(const std::string& name);  // across all modules, 0 if missing
Module* module_at(u32 addr);
// "function" if addr lies inside a known function, else "lib+0xPAGE" (4 KB bucket) for unnamed code.
std::string function_of(u32 addr);
const std::vector<Module*>& modules();

}  // namespace loader
