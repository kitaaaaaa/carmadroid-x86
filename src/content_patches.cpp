// Restores content the Android build hides.
#include "content_patches.h"
#include "elf_loader.h"
#include "memory.h"

namespace content {

bool g_restore = true;

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

}  // namespace content
