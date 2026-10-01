#pragma once
// The GL ES -> desktop GL layer: settings the port adds on top of the game's rendering.

namespace gles {

// Texture filtering (the graphics options screen). Off: nearest-neighbour texels like the PC game (mipmap
// levels still blend, so distant textures don't shimmer). Applied to every texture the game has made.
// Call on the game's GL thread.
void set_texture_filtering(bool smooth);
bool texture_filtering();

}  // namespace gles
