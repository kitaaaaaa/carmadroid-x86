#pragma once
// Unpacking the game's APK/OBB into a folder, and running from such a folder (for modding).
//
// Folder layout:
//   lib/armeabi-v7a/*.so   the game's native libraries (from the APK)
//   assets/...             the APK's assets (music, etc.)
//   DATA/...               the contents of the OBB (DATA_ANDROID.WAD): CONTENT, SETUP
//
// The game only reads its data from the OBB, so when running from a folder the DATA tree is packed
// back into an OBB (uncompressed) in the user data folder, and repacked whenever the folder changes.
#include "common.h"
#include <string>

namespace gamedata {

// Writes the APK's libraries and assets and every file in the OBB to out_dir.
bool extract(const std::string& apk_path, const std::string& obb_path, const std::string& out_dir, std::string& err);

// Packs dir/DATA into an OBB at obb_out, unless one built from identical files is already there.
bool build_obb(const std::string& dir, const std::string& obb_out, std::string& err);

}  // namespace gamedata
