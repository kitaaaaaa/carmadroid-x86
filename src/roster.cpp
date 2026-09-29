// Raises the game's 40-car limit to 64 (for added cars, see tools/pc2android).
//
// libParsons keeps the car roster in fixed 40-entry tables:
//  - the quick-race car tables filled from QUICKRACECARS.TXT and the AI opponent tables filled from
//    OPPONENTLIST.TXT (exported globals, reached only through the GOT): bigger copies are allocated and
//    their GOT entries repointed;
//  - the vehicle module's private CARSPECS.TXT table (count + 32-byte entries at +0xb0 of a static
//    block), used by three functions: their PC-relative address constants are repointed;
//  - a 40-entry stack buffer in the function that picks a random opponent: its stack frame is enlarged;
//  - the save slot's list of owned cars (40 names): kept as is, so saves stay compatible. Cars owned
//    beyond that are kept in userdata/extra_cars.txt via hooks on Structure_UnlockCar/IsCarUnlocked.
#include "roster.h"
#include "cpu.h"
#include "elf_loader.h"
#include "memory.h"
#include "hle/hle_common.h"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace roster {
namespace {

constexpr int kOldMax = 40;

// libParsons.so v1.8.507
struct Table { const char* name; u32 got, var, entry_size; };
const Table kTables[] = {
    {"gQuick_race_car_masses", 0x670470, 0x842db4, 4},
    {"gQuick_race_car_to_sixty", 0x670078, 0x842e54, 4},
    {"gQuick_race_car_powers", 0x67030c, 0x842ef4, 4},
    {"gQuick_race_car_softnesses", 0x66ff2c, 0x842f94, 4},
    {"gQuick_race_car_top_ends", 0x670178, 0x843034, 4},
    {"gQuick_race_car_attributes", 0x6707ec, 0x8430d4, 5},
    {"gQuick_race_driver_names", 0x66f604, 0x84319c, 48},
    {"gQuick_race_car_names", 0x66f820, 0x84391c, 48},
    {"gQuick_race_car_file_names", 0x67015c, 0x8440ac, 4},
    {"gAI_opponent_strength", 0x66fb74, 0x8bf2c4, 1},
    {"gAI_opponent_list", 0x66f1d0, 0x8bf2ec, 4},
};

// Car specs table: static block at 0x8c19c0, count at +0xb0, entries (32 bytes) at +0xb4.
constexpr u32 kVehicleBlock = 0x8c19c0, kSpecsOffset = 0xb0, kSpecEntry = 32;
struct PcRel { u32 literal, add; };  // "ldr rN, =literal" ... "add rX, pc, rN" at add
const PcRel kSpecsRefs[] = {
    {0x575494, 0x575404},  // Vehicle_DestroyModule
    {0x57a934, 0x57a788},  // Vehicle_LoadAttributes
    {0x57a93c, 0x57a7c0},
    {0x57acb8, 0x57ab68},  // Vehicle_GetAttributes
    {0x57acbc, 0x57ac60},
};

// Random opponent picker (no symbol, after bzCloudTamperProtection::GetData): candidate indices go in
// a 40-int buffer at sp+0x18; its stack frame is grown by 24 ints.
struct Insn { u32 at, old_code, new_code; };
const Insn kPickerPatches[] = {
    {0x569cc4, 0xE24DD0BC, 0xE24DDF47},  // sub sp, sp, #0xbc    -> #0x11c
    {0x569df8, 0xE28D30B8, 0xE28D3F46},  // add r3, sp, #0xb8    -> #0x118
    {0x569e04, 0xE50360A0, 0xE5036100},  // str r6, [r3, #-0xa0] -> #-0x100
    {0x569e20, 0xE28DE0B8, 0xE28DEF46},  // add lr, sp, #0xb8    -> #0x118
    {0x569e38, 0xE51000A0, 0xE5100100},  // ldr r0, [r0, #-0xa0] -> #-0x100
    {0x569e6c, 0xE28DD0BC, 0xE28DDF47},  // add sp, sp, #0xbc    -> #0x11c
};

// Save slot: GOT entry of the pointer to the current slot; owned car names at +0x7c, 40 x 24 bytes.
constexpr u32 kSaveSlotPtrGot = 0x67054c;
constexpr u32 kOwnedCars = 0x7c, kOwnedCarSize = 24;

u32 g_lib = 0;
u32 g_is_unlocked_orig = 0, g_unlock_orig = 0;
std::mutex g_lock;

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string extras_path() { return hle::g_config.root + "/extra_cars.txt"; }

// "<slot key> <car>" lines; the slot key identifies the save slot (its address, stable between runs).
std::set<std::string> load_extras() {
    std::set<std::string> s;
    std::ifstream f(extras_path());
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) s.insert(lower(line));
    return s;
}

void save_extras(const std::set<std::string>& s) {
    std::ofstream f(extras_path(), std::ios::trunc);
    for (const auto& l : s) f << l << "\n";
}

u32 save_slot() {
    const u32 holder = mem::r32(g_lib + kSaveSlotPtrGot);
    return holder ? mem::r32(holder) : 0;
}

std::string slot_key(u32 slot) {
    char b[16];
    snprintf(b, sizeof b, "%x", slot - g_lib);
    return b;
}

bool slot_list_full(u32 slot) {
    for (int i = 0; i < kOldMax; i++)
        if (mem::r8(slot + kOwnedCars + i * kOwnedCarSize) == 0) return false;
    return true;
}

bool in_slot_list(u32 slot, const std::string& car) {
    for (int i = 0; i < kOldMax; i++) {
        const char* s = mem::ptr<const char>(slot + kOwnedCars + i * kOwnedCarSize);
        if (lower(std::string(s, strnlen(s, kOwnedCarSize))) == lower(car)) return true;
    }
    return false;
}

bool relocate_tables() {
    for (const Table& t : kTables)
        if (mem::r32(g_lib + t.got) != g_lib + t.var) {
            LOGE("roster: unexpected GOT entry for %s; car limit stays at 40", t.name);
            return false;
        }
    for (const Table& t : kTables) {
        const u32 buf = mem::calloc(kMaxCars, t.entry_size);
        memcpy(mem::ptr(buf), mem::ptr(g_lib + t.var), kOldMax * t.entry_size);
        mem::w32(g_lib + t.got, buf);
    }
    return true;
}

bool relocate_specs() {
    for (const PcRel& r : kSpecsRefs)
        if (mem::r32(g_lib + r.literal) + g_lib + r.add + 8 != g_lib + kVehicleBlock) {
            LOGE("roster: unexpected car specs reference at 0x%x", r.literal);
            return false;
        }
    const u32 buf = mem::calloc(1, 4 + kMaxCars * kSpecEntry);
    const u32 base = buf - kSpecsOffset;  // so that base+0xb0 is the count and base+0xb4 the entries
    for (const PcRel& r : kSpecsRefs) mem::w32(g_lib + r.literal, base - (g_lib + r.add + 8));
    return true;
}

bool patch_opponent_picker() {
    for (const Insn& p : kPickerPatches)
        if (mem::r32(g_lib + p.at) != p.old_code) {
            LOGE("roster: unexpected code at 0x%x (0x%08x) in the opponent picker", p.at, mem::r32(g_lib + p.at));
            return false;
        }
    for (const Insn& p : kPickerPatches) mem::w32(g_lib + p.at, p.new_code);
    return true;
}

void is_unlocked_hook(Cpu& c) {
    const u32 name = c.r(0);
    u32 r = c.call(g_is_unlocked_orig, {name}) & 0xFF;
    if (!r && name) {
        const u32 slot = save_slot();
        if (slot) {
            std::lock_guard<std::mutex> l(g_lock);
            auto extras = load_extras();
            const std::string key = slot_key(slot) + " " + lower(mem::str(name));
            if (extras.count(key)) {
                if (slot_list_full(slot)) {
                    r = 1;
                } else {  // a new career on this slot: forget the extra cars of the old one
                    for (auto it = extras.begin(); it != extras.end();)
                        it = it->rfind(slot_key(slot) + " ", 0) == 0 ? extras.erase(it) : std::next(it);
                    save_extras(extras);
                }
            }
        }
    }
    c.set_r(0, r);
}

void unlock_hook(Cpu& c) {
    const u32 name = c.r(0);
    u32 r = c.call(g_unlock_orig, {name}) & 0xFF;
    if (!r && name) {
        const u32 slot = save_slot();
        const std::string car = mem::str(name);
        if (slot && slot_list_full(slot) && !in_slot_list(slot, car) && lower(car) != "bigapc") {
            std::lock_guard<std::mutex> l(g_lock);
            auto extras = load_extras();
            extras.insert(slot_key(slot) + " " + lower(car));
            save_extras(extras);
            LOGI("roster: %s owned (beyond the save's 40-car list)", car.c_str());
            r = 1;
        }
    }
    c.set_r(0, r);
}

}  // namespace

void apply() {
    for (Module* m : loader::modules())
        if (m->name == "libParsons.so") g_lib = m->base;
    if (!g_lib) return;
    if (!relocate_tables() || !relocate_specs() || !patch_opponent_picker()) return;
    const u32 is_unlocked = loader::find_symbol("_Z23Structure_IsCarUnlockedPKc");
    const u32 unlock = loader::find_symbol("_Z19Structure_UnlockCarPc");
    if (is_unlocked && unlock) {
        g_is_unlocked_orig = hle::hook_function(is_unlocked, "Structure_IsCarUnlocked", is_unlocked_hook);
        g_unlock_orig = hle::hook_function(unlock, "Structure_UnlockCar", unlock_hook);
    }
    LOGI("roster: car and opponent limit raised to %d", kMaxCars);
}

}  // namespace roster
