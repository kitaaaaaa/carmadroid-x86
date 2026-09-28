#include "controller.h"
#include "elf_loader.h"
#include "hle/android.h"
#include "platform.h"
#include <atomic>
#include <cmath>

namespace controller {
namespace {

// --- libParsons.so v1.8.507 layout (offsets from the library base) ------------
constexpr u32 kInputState = 0x84A53C;      // touch zones + control modes
constexpr u32 kThrottleModeOff = 0x5F0;
constexpr u32 kSteerModeOff = 0x5F4;
constexpr u32 kZoneStride = 88;
constexpr u32 kSensitivity = 0x67CBC8;     // float brake (0..500), float steering (0..1.5)
constexpr u32 kModeTilt = 3;               // the value Vehicle_PreDynamicsProcess treats as tilt
constexpr u32 kCameraRollLoad = 0x578590;  // "ldr r0,[r3,#8]" feeding Camera_SetRollAngle in the tilt-steer path
constexpr u32 kCameraStatesGot = 0x66F73C; // GOT slot -> per-player camera state (292 bytes each, mode at +0xc)

enum Zone { ZONE_HANDBRAKE = 4, ZONE_DAMAGE = 7, ZONE_PRATCAM = 8, ZONE_PAUSE = 9 };

// Edge-triggered actions requested by the main thread, run on the game thread.
enum Action { ACT_REPAIR, ACT_CAMERA, ACT_RECOVER, ACT_PRATCAM, ACT_PAUSE, ACT_COUNT };

struct ButtonMap {
    SDL_GameControllerButton button;
    int action;  // Action, or -1 = handbrake (held), -2 = Android BACK key
};
const ButtonMap kButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, -1},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1},
    {SDL_CONTROLLER_BUTTON_B, ACT_REPAIR},
    {SDL_CONTROLLER_BUTTON_X, ACT_CAMERA},
    {SDL_CONTROLLER_BUTTON_Y, ACT_RECOVER},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ACT_PRATCAM},
    {SDL_CONTROLLER_BUTTON_START, ACT_PAUSE},  // when already paused: BACK (resume)
    {SDL_CONTROLLER_BUTTON_BACK, -2},
};
constexpr int kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);

SDL_GameController* g_pad = nullptr;
bool g_held[kNumButtons] = {};

std::atomic<bool> g_active{false};
std::atomic<bool> g_handbrake{false};
std::atomic<int> g_pending[ACT_COUNT];
std::atomic<float> g_steer{0}, g_throttle{0};
bool g_simulated = false;

// Mode override bookkeeping (game thread only)
bool g_overriding = false;
u32 g_saved_steer_mode = 0, g_saved_throttle_mode = 0;

u32 g_process_touch_zones_orig = 0;  // trampoline to the original Input_ProcessTouchZones

u32 lib_base() {
    static u32 base = [] {
        for (Module* m : loader::modules())
            if (m->name == "libParsons.so") return m->base;
        return 0u;
    }();
    return base;
}

u32 sym(const char* name) {
    u32 a = loader::find_symbol(name);
    if (!a) fatal("controller: missing symbol %s", name);
    return a;
}

bool is_paused() {
    static u32 addr = loader::find_symbol("gPaused");
    return addr && mem::r8(addr) != 0;
}

float axis(SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(g_pad, a) / 32767.0f; }

float deadzone(float v, float dz) {
    float m = std::fabs(v);
    if (m < dz) return 0;
    return std::copysign(std::fmin((m - dz) / (1 - dz), 1.0f), v);
}

void open_first_pad() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (g_pad) {
            LOGI("controller connected: %s", SDL_GameControllerName(g_pad));
            return;
        }
    }
}

// Converts steering/throttle in [-1,1] into the gravity vector that makes the game's tilt
// code produce exactly those values:
//   roll  = atan2(X, -Y)        steer    = sqrt(steerSens/1.5) * roll / 30deg
//   pitch = atan2(-Z, -Y)       throttle = sqrt(brakeSens/500) * (pitch - 30deg) / 15deg
// where (X,Y,Z) = -accel/g (display rotation 0). |Z| must stay below 0.97.
void feed_accelerometer(float steer, float throttle) {
    const float* sens = mem::ptr<float>(lib_base() + kSensitivity);
    float brake_gain = sens[0] > 0 ? std::sqrt(sens[0] / 500.0f) : 1.0f;
    float steer_gain = sens[1] > 0 ? std::sqrt(sens[1] / 1.5f) : 1.0f;
    float s = std::fmax(std::fmin(steer / std::fmax(steer_gain, 0.05f), 2.5f), -2.5f);
    float t = std::fmax(std::fmin(throttle / std::fmax(brake_gain, 0.05f), 2.5f), -2.5f);
    const float deg = 3.14159265f / 180.0f;
    float roll = s * 30.0f * deg;             // up to 75 degrees
    float pitch = (30.0f + t * 15.0f) * deg;  // -7.5 .. 67.5 degrees
    float X = std::tan(roll), Y = -1.0f, Z = -std::tan(pitch);
    float n = std::sqrt(X * X + Y * Y + Z * Z);
    const float g = 9.80665f;
    android::set_accelerometer(-X / n * g, -Y / n * g, -Z / n * g);
}

// Runs the game's own action for a HUD button, from inside Input_ProcessTouchZones, i.e. at the
// point in the frame where a finger on the HUD would have triggered it.
void call_zone_handler(Cpu& c, const char* handler, int zone, u32 bool_ptr, u32 repair_mode, u32 f0, u32 f1) {
    u32 zone_rec = lib_base() + kInputState + zone * kZoneStride;
    // handler(TouchZone*, pressed, x, y, RepairMode*, bool*, float, float)
    c.call(sym(handler), {zone_rec, 1, 0, 0, repair_mode, bool_ptr, f0, f1});
}

void toggle_camera(Cpu& c, u32 vehicle) {
    // Same as tapping the car in Camera_CheckTouchControls: flip between chase (0) and bonnet (1).
    u32 states = mem::r32(lib_base() + kCameraStatesGot);
    u32 player = mem::r32(vehicle + 0x14);
    u32 mode = mem::r32(states + player * 292 + 0xc);
    c.call(sym("_Z16Camera_ChangedToP7Vehicle10CameraModeb"), {vehicle, mode == 0 ? 1u : 0u, 0});
}

void process_touch_zones_hook(Cpu& c) {
    platform::note_race_frame();
    const u32 bool_ptr = c.r(0), repair_mode = c.r(1), f0 = c.r(2), f1 = c.r(3);
    u64 ret = c.call64(g_process_touch_zones_orig, {bool_ptr, repair_mode, f0, f1});

    if (g_active) {
        u32 vehicle = c.call(sym("_Z19Vehicle_GetNthHumani"), {0});
        if (vehicle) {
            if (g_handbrake)
                call_zone_handler(c, "_Z19TouchZone_HandbrakeP9TouchZonebffP10RepairModePbff", ZONE_HANDBRAKE,
                                  bool_ptr, repair_mode, f0, f1);
            if (g_pending[ACT_REPAIR].exchange(0))
                call_zone_handler(c, "_Z16TouchZone_DamageP9TouchZonebffP10RepairModePbff", ZONE_DAMAGE, bool_ptr,
                                  repair_mode, f0, f1);
            if (g_pending[ACT_PRATCAM].exchange(0))
                call_zone_handler(c, "_Z17TouchZone_PratCamP9TouchZonebffP10RepairModePbff", ZONE_PRATCAM, bool_ptr,
                                  repair_mode, f0, f1);
            if (g_pending[ACT_CAMERA].exchange(0)) { LOGI("pad: camera"); toggle_camera(c, vehicle); }
            if (g_pending[ACT_RECOVER].exchange(0)) { LOGI("pad: recover"); c.call(sym("_Z15Recover_RequestP7Vehicle"), {vehicle}); }
            if (g_pending[ACT_PAUSE].exchange(0))
                call_zone_handler(c, "_Z15TouchZone_PauseP9TouchZonebffP10RepairModePbff", ZONE_PAUSE, bool_ptr,
                                  repair_mode, f0, f1);
        }
    }
    c.ret64(ret);
}

}  // namespace

void init() {
    SDL_GameControllerEventState(SDL_ENABLE);
    open_first_pad();
}

void apply_patches() {
    // Tilt steering rolls the camera to keep the horizon level on a tilted phone. With a stick that
    // just leans the camera whenever you steer, so load 0 instead ("mov r0, #0").
    u32 at = lib_base() + kCameraRollLoad;
    if (mem::r32(at) == 0xE5930008) mem::w32(at, 0xE3A00000);
    else LOGE("controller: unexpected code at camera roll patch site; not patched");

    g_process_touch_zones_orig = hle::hook_function(sym("_Z23Input_ProcessTouchZonesPbP10RepairModeff"),
                                                    "Input_ProcessTouchZones", process_touch_zones_hook);
}

void simulate(float steer, float throttle) {
    g_simulated = true;
    g_active = true;
    g_steer = steer;
    g_throttle = throttle;
    LOGI("controller simulation: steer %.2f throttle %.2f", steer, throttle);
}

void simulate_button(int action) {
    if (action < 0) g_handbrake = !g_handbrake;
    else if (action < ACT_COUNT) g_pending[action]++;
    LOGI("controller simulation: button action %d", action);
}

void handle_event(const SDL_Event& ev) {
    switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open_first_pad();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                LOGI("controller disconnected");
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                g_handbrake = false;
                for (bool& h : g_held) h = false;
                open_first_pad();
            }
            break;
    }
}

void update() {
    if (g_simulated) {
        feed_accelerometer(g_steer, g_throttle);
        return;
    }
    if (!g_pad) {
        if (g_active.exchange(false)) android::set_accelerometer(0, 0, 9.80665f);
        return;
    }
    g_active = true;
    float steer = deadzone(axis(SDL_CONTROLLER_AXIS_LEFTX), 0.12f);
    if (steer == 0) {  // D-pad as digital steering
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) steer = -1;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) steer = 1;
    }
    float throttle = deadzone(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT), 0.05f) -
                     deadzone(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), 0.05f);
    g_steer = steer;
    g_throttle = throttle;
    feed_accelerometer(steer, throttle);

    bool handbrake = false;
    for (int i = 0; i < kNumButtons; i++) {
        bool down = SDL_GameControllerGetButton(g_pad, kButtons[i].button) != 0;
        if (kButtons[i].action == -1) handbrake |= down;
        if (down == g_held[i]) continue;
        g_held[i] = down;
        if (kButtons[i].action == -2) {
            platform::send_key(4, down);  // Android BACK
        } else if (kButtons[i].action == ACT_PAUSE && is_paused()) {
            platform::send_key(4, down);  // BACK closes the pause menu
        } else if (down && kButtons[i].action >= 0) {
            g_pending[kButtons[i].action]++;
        }
    }
    g_handbrake = handbrake && !is_paused();
}

void on_game_frame(Cpu&) {
    u32 base = lib_base();
    if (!base) return;
    u32 steer_mode = base + kInputState + kSteerModeOff;
    u32 throttle_mode = base + kInputState + kThrottleModeOff;
    if (g_active) {
        // Remember whatever the game itself last set (e.g. from the profile) so unplugging restores it.
        u32 cur_steer = mem::r32(steer_mode), cur_throttle = mem::r32(throttle_mode);
        if (cur_steer != kModeTilt) g_saved_steer_mode = cur_steer;
        if (cur_throttle != kModeTilt) g_saved_throttle_mode = cur_throttle;
        if (!g_overriding) {
            g_overriding = true;
            LOGI("controller: steering/throttle now driven by the pad");
        }
        mem::w32(steer_mode, kModeTilt);
        mem::w32(throttle_mode, kModeTilt);
    } else if (g_overriding) {
        mem::w32(steer_mode, g_saved_steer_mode);
        mem::w32(throttle_mode, g_saved_throttle_mode);
        g_overriding = false;
        LOGI("controller: restored touch controls");
    }
}

}  // namespace controller
