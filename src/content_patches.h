#pragma once
#include "common.h"
#include <string>

namespace content {
extern bool g_restore;          // --censored turns this off
extern bool g_unlock_all_cars;  // --unlock-all-cars
extern std::string g_force_car;  // --car NAME: select this car (testing)
void apply();                   // call after libParsons.so is loaded, before it runs
void on_game_frame();           // game thread, every frame
}
