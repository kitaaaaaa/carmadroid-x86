#pragma once
// Optional content from the original PC Carmageddon (not included; the user points us at their copy).
// Currently: the in-car cockpit overlay (dashboard, side views when looking around, animated hands).
#include "common.h"
#include <string>

namespace pc_content {

// Directory containing the PC game's DATA folder (e.g. ...\Carmageddon1\CARMA). Empty = off.
extern std::string g_dir;
// The Splat Pack's folder (...\Carmageddon1\CARSPLAT), for its cars' cockpits. Optional.
extern std::string g_splat_dir;
extern bool g_cockpit;  // draw the cockpit overlay in the in-car view

void apply_patches();   // after libParsons.so is loaded

// After the game has placed its cameras: in the in-car view of a car with a PC cockpit, puts the camera
// at the PC driver's head (the PC car's bonnet model is then drawn from there, under the dashboard).
void place_bonnet_camera();

}  // namespace pc_content
