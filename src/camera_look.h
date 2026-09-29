#pragma once
#include "common.h"

namespace camera_look {
extern float g_yaw_sign, g_pitch_sign;  // direction conventions (1 or -1)
void apply_patches();   // after libParsons.so is loaded
float yaw_deg();        // current head turn (negative = left)
bool bonnet_view();     // local player's camera is the in-car (bonnet) view
}
