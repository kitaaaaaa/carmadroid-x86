#pragma once
// SDL window, GL context and host input.
#include "common.h"

#include <string>

namespace platform {

extern int g_shot_every;      // save a screenshot every N frames (0 = off)
extern std::string g_shot_dir;

struct Options {
    int max_w = 1920, max_h = 1080;   // render resolution cap (aspect follows the desktop)
    int render_w = 0, render_h = 0;   // exact render resolution override
    bool fullscreen = false;
    bool vsync = false;
    bool hidden = false;              // for automated test runs
};

bool init(const Options& opt);

// Called from the game's per-frame race input routine. Outside races (menus, loading screens)
// presentation is capped at menu_fps so the game thread spends its time loading instead of drawing.
void note_race_frame();
bool in_race();  // a race frame ran in the last 250 ms
extern int g_menu_fps;   // 0 = uncapped
extern int g_race_fps;   // 0 = uncapped
void make_current(bool current);  // on the calling thread
void swap();
void drawable_size(int& w, int& h);  // the game's render resolution
void* gl_proc(const char* name);

// Pumps SDL events on the main thread; returns false when the user closes the window.
bool pump_events();

// Debug: synthesize a tap (touch down+up) at drawable coordinates.
void inject_tap(float x, float y);
void inject_key(int android_keycode);  // press + release

// Multi-touch: pointer ids are arbitrary (mouse uses 0). Coordinates are drawable pixels. Main thread only.
void touch_down(int id, float x, float y);
void touch_move(int id, float x, float y);
void touch_up(int id);
void send_key(int android_keycode, bool down);

}  // namespace platform
