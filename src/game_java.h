#pragma once
#include "cpu.h"

// Drives the Android activity lifecycle the way ParsonsLoader (Java) + the framework would.
namespace game_java {

void on_create(Cpu& c);
void on_start(Cpu& c);
void on_resume(Cpu& c);
void on_pause(Cpu& c);
void on_stop(Cpu& c);
void on_window_created(Cpu& c);
void on_input_queue_created(Cpu& c);
void on_focus(Cpu& c, bool focused);

}  // namespace game_java
