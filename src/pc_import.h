#pragma once
// Installing content from the original PC Carmageddon into an unpacked game folder (see gamedata.h).
// Nothing from the PC game is included: the user's own copy is converted on their machine.
//
//   - Splat Pack cars (needs the PC game's CARMA and CARSPLAT folders): converted to the Android formats,
//     with menu pictures, driver portraits, damage HUD and descriptions, and added to the car roster and
//     the opponents.
//   - Tracks: not yet.
// The in-car cockpit views ("interiors") are drawn straight from the PC files at run time (pc_content).
// Also, with or without the PC game: the controls options screen gets a steering deadzone slider.
//
// The install is done once and recorded in <game folder>/pcimport: a journal of the files it created
// and backups of the game files it changed. It is undone and redone when the PC folders or the
// converter change.
#include "common.h"
#include <string>

namespace pc_import {

// carma / carsplat: the PC game's CARMA and CARSPLAT folders (the ones containing DATA); either may be
// empty. With neither, an earlier install is left as it is (a first install only adds the slider).
void install(const std::string& game_dir, const std::string& carma, const std::string& carsplat);

// Removes an earlier install: deletes the files it created and restores the game files it changed.
void uninstall(const std::string& game_dir);

}  // namespace pc_import
