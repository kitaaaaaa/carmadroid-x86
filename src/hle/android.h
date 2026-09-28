#pragma once
// Host-side Android NDK emulation shared between modules.
#include "../common.h"
#include <functional>

namespace android {

// Input events queued by the host (SDL) and consumed by the guest via AInputQueue.
struct Pointer {
    s32 id;
    float x, y;
};
struct InputEvent {
    s32 type;     // 1 = key, 2 = motion
    s32 source;   // AINPUT_SOURCE_*
    s32 action;   // AKEY_EVENT_ACTION_* or AMOTION_EVENT_ACTION_* (with pointer index bits)
    s32 keycode;
    s32 meta;
    s32 pointer_count;
    Pointer pointers[10];
};

void push_input(const InputEvent& e);

// Accelerometer vector (m/s^2, Android device axes) reported to the game.
void set_accelerometer(float x, float y, float z);

// The guest's input queue handle (passed to onInputQueueCreated)
u32 input_queue_handle();

// Window size reported through ANativeWindow / EGL
void set_window_size(int w, int h);
void get_window_size(int& w, int& h);
u32 native_window_handle();

}  // namespace android
