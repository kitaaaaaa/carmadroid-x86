#pragma once
// Raises the game's 40-car roster limit (quick race cars, opponents, car specs, owned cars) to 64.
#include "common.h"

namespace roster {
constexpr int kMaxCars = 64;
void apply();  // after libParsons.so is loaded, before its initializers run
}
