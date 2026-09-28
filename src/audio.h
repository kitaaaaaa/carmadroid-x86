#pragma once
// Replacement for org.fmod.FMODAudioDevice: pulls mixed PCM from FMOD (guest) and plays it with SDL.
namespace audio {
extern int g_sample_rate;  // FMOD mix rate (--audio-rate; 0 = game default)
void apply_patches();      // after libfmodex.so is loaded
void start();
void stop();
}
