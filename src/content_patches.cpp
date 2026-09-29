// Restores content the Android build hides.
#include "content_patches.h"
#include "elf_loader.h"
#include "memory.h"
#include "platform.h"

namespace content {

bool g_restore = true;
bool g_unlock_all_cars = false;
std::string g_force_car;

namespace {
// Structure_CheatUnlockAllCars() sets this byte; Structure_IsCarUnlocked() then reports every car as
// owned. It is not saved, so it is simply kept set.
constexpr u32 kUnlockAllCarsFlag = 0x8BD9B6;
constexpr u32 kCarFileNamesGot = 0x67015c;  // -> char* per quick-race car
constexpr u32 kNumQuickCars = 0x84414c;
constexpr u32 kSaveSlotPtrGot = 0x67054c;   // -> pointer to the current save slot
constexpr u32 kSlotSelectedCar = 0x78;      // index into the quick-race car list
u32 g_lib = 0;
}  // namespace

void apply() {
    if (!g_restore) return;
    // Pickup_Find() looks a pickup up by name, then silently swaps three pickup types for others:
    //   type 0x1F (Drugs) -> GripTyres, type 0x05 -> PedGiant, type 0x29 -> Pinball.
    // Turning each "cmp r3, #type" into "cmp r3, #0xFF" (no such type) disables the swaps.
    const u32 find = loader::find_symbol("_Z11Pickup_FindPc");
    if (!find) { LOGE("content: Pickup_Find not found"); return; }
    struct Site { u32 offset, expected; } sites[] = {
        {0xC0, 0xE353001F},  // cmp r3, #0x1f
        {0xC8, 0xE3530005},  // cmp r3, #5
        {0xD4, 0xE3530029},  // cmp r3, #0x29
    };
    int patched = 0;
    for (const Site& s : sites) {
        u32 at = (find & ~1u) + s.offset;
        if (mem::r32(at) == s.expected) {
            mem::w32(at, (s.expected & ~0xFFu) | 0xFF);
            patched++;
        } else {
            LOGE("content: unexpected instruction at Pickup_Find+0x%x (0x%08x)", s.offset, mem::r32(at));
        }
    }
    LOGI("content: restored %d censored pickup(s) (Drugs and friends)", patched);
}

static void force_car();

void on_game_frame() {
    if (!g_unlock_all_cars && g_force_car.empty()) return;
    if (!g_lib)
        for (Module* m : loader::modules())
            if (m->name == "libParsons.so") g_lib = m->base;
    if (!g_lib) return;
    // Keep re-selecting until the first race: loading the save would otherwise overwrite it.
    if (!g_force_car.empty()) {
        if (platform::in_race()) g_force_car.clear();
        else force_car();
    }
    if (g_unlock_all_cars) mem::w8(g_lib + kUnlockAllCarsFlag, 1);
}

// --car: make NAME the selected car in the current save slot.
static void force_car() {
    const int n = (int)mem::r32(g_lib + kNumQuickCars);
    const u32 holder = mem::r32(g_lib + kSaveSlotPtrGot);
    const u32 slot = holder ? mem::r32(holder) : 0;
    const u32 names = mem::r32(g_lib + kCarFileNamesGot);
    if (n <= 0 || !slot || !names) return;
    for (int i = 0; i < n; i++) {
        const char* s = mem::str(mem::r32(names + i * 4));
        if (s && _stricmp(s, g_force_car.c_str()) == 0) {
            if (mem::r32(slot + kSlotSelectedCar) != (u32)i) {
                mem::w32(slot + kSlotSelectedCar, (u32)i);
                LOGI("content: selected car %s (#%d)", s, i);
            }
            return;
        }
    }
}


}  // namespace content
