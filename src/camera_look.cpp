// Right-stick look-around: after the game updates its cameras each frame, turn the local player's
// camera by the stick (like turning your head). Done before rendering, so the game's own visibility
// culling uses the rotated view.
#include "camera_look.h"
#include "controller.h"
#include "elf_loader.h"
#include "memory.h"
#include "pc_content.h"
#include <SDL.h>
#include <atomic>
#include <cmath>

namespace camera_look {
namespace {

// libParsons.so v1.8.507 offsets
constexpr u32 kCameraStatesGot = 0x66F73C;  // GOT slot -> per-player camera state (292 bytes each)
constexpr u32 kStateModeOff = 0xC;           // CameraMode: 0 chase, 1 bonnet
constexpr u32 kLumpMatrixOff = 8;            // bzM34 (3 axis rows + position) in the camera lump

constexpr float kMaxYawDeg = 80.0f;    // how far the head turns left/right
constexpr float kMaxPitchDeg = 30.0f;  // up/down

u32 g_orig = 0;
u32 g_lib = 0;
std::atomic<float> g_yaw_deg{0}, g_pitch_deg{0};

u32 lib_base() {
    if (!g_lib)
        for (Module* m : loader::modules())
            if (m->name == "libParsons.so") g_lib = m->base;
    return g_lib;
}

// Rotates axis rows a and b of a 3x3 matrix (row-major, one axis per row) within their plane.
void rotate_rows(float* m, int a, int b, float ang) {
    const float c = std::cos(ang), s = std::sin(ang);
    for (int k = 0; k < 3; k++) {
        const float ra = m[a * 3 + k], rb = m[b * 3 + k];
        m[a * 3 + k] = c * ra - s * rb;
        m[b * 3 + k] = s * ra + c * rb;
    }
}

void after_camera_update() {
    static u64 last = SDL_GetTicks64();
    const u64 now = SDL_GetTicks64();
    const float dt = std::fmin((now - last) / 1000.0f, 0.1f);
    last = now;
    float sx, sy;
    controller::look_input(sx, sy);
    float yaw, pitch;
    if (bonnet_view()) {
        // In the car: snap like turning your head. Left/right = 80 degrees, pulled back = look behind,
        // pushed forward = nothing. No up/down.
        yaw = pitch = 0;
        if (std::fabs(sx) >= 0.5f && std::fabs(sx) >= sy) yaw = std::copysign(kMaxYawDeg, sx);
        else if (sy >= 0.5f) yaw = 180.0f;
    } else {
        // Chase camera: smoothly follow the stick (springs back to straight ahead when released).
        const float k = 1.0f - std::exp(-dt * 14.0f);
        yaw = g_yaw_deg + (sx * kMaxYawDeg - g_yaw_deg) * k;
        pitch = g_pitch_deg + (-sy * kMaxPitchDeg - g_pitch_deg) * k;
        if (std::fabs(yaw) < 0.05f) yaw = 0;
        if (std::fabs(pitch) < 0.05f) pitch = 0;
    }
    g_yaw_deg = yaw;
    g_pitch_deg = pitch;
    if (yaw == 0 && pitch == 0) return;

    const u32 states = mem::r32(lib_base() + kCameraStatesGot);
    if (!states) return;
    const u32 lump = mem::r32(states);  // player 0's camera lump
    if (!mem::valid(lump + kLumpMatrixOff, 48)) return;
    float* m = mem::ptr<float>(lump + kLumpMatrixOff);
    const float rad = 3.14159265f / 180.0f;
    // Looking behind is the same whichever way the stick is inverted.
    rotate_rows(m, 0, 2, yaw * rad * (yaw == 180.0f ? 1.0f : g_yaw_sign));  // turn about the camera's up axis
    if (pitch != 0) rotate_rows(m, 1, 2, pitch * rad * g_pitch_sign);        // tilt about the camera's right axis
}

}  // namespace

float g_yaw_sign = 1.0f;
float g_pitch_sign = 1.0f;

void apply_patches() {
    const u32 fn = loader::find_symbol("_Z16Camera_UpdateAllff");
    if (!fn) { LOGE("camera: Camera_UpdateAll not found"); return; }
    g_orig = hle::hook_function(fn, "Camera_UpdateAll", [](Cpu& c) {
        c.call(g_orig, {c.r(0), c.r(1)});  // (float, float) in r0/r1 (softfp)
        pc_content::place_bonnet_camera();  // (before the head turn, which keeps the position)
        after_camera_update();
    });
}

float yaw_deg() { return g_yaw_deg; }

bool bonnet_view() {
    const u32 lib = lib_base();
    if (!lib) return false;
    const u32 states = mem::r32(lib + kCameraStatesGot);
    return states && mem::r32(states + kStateModeOff) == 1;
}

}  // namespace camera_look
