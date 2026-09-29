#pragma once
// Optional content from the original PC Carmageddon (not included; the user points us at their copy).
// Currently: the in-car cockpit overlay (dashboard, side views when looking around, animated hands).
#include "common.h"
#include <string>

namespace pc_content {

// Directory containing the PC game's DATA folder (e.g. ...\Carmageddon1\CARMA). Empty = off.
extern std::string g_dir;
extern bool g_cockpit;  // draw the cockpit overlay in the in-car view

void apply_patches();   // after libParsons.so is loaded

}  // namespace pc_content
