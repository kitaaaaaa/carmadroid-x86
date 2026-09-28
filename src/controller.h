#pragma once
// Gamepad support for races.
//
// Steering and throttle/brake are analog: they go through the game's own TILT
// control modes by synthesizing a gravity vector (see Input_ReadTiltValues in
// libParsons). Buttons press the game's on-screen touch zones with virtual fingers.
#include "cpu.h"
#include <SDL.h>

namespace controller {

void init();
void apply_patches();                    // after libParsons.so is loaded
void handle_event(const SDL_Event& ev);  // main thread
void update();                           // main thread, every pump
void on_game_frame(Cpu& c);              // game thread, every frame (eglSwapBuffers)

// Debug: pretend a pad is connected with fixed stick/trigger values.
void simulate(float steer, float throttle);
// Debug: trigger an action (-1 toggles handbrake, 0 repair, 1 camera, 2 recover, 3 pratcam, 4 pause).
void simulate_button(int action);

}  // namespace controller
