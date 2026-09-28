# carmadroid-x86

Runs the **Android version of Carmageddon** (Stainless Games, 2013, v1.8.507) natively on 64-bit Windows,
with analog gamepad support, up to 1080p rendering, an unlocked frame rate and restored content.

The Android release is a 32-bit ARM-only app that no longer runs on modern 64-bit-only phones. This project
loads the game's original ARM libraries from the APK, runs them through an ARM-to-x86-64 JIT, and replaces
every Android system service the game uses (graphics, audio, input, files, Java calls) with Windows
equivalents.

**No game data is included.** You need your own copy of the game's APK and OBB files.

## What you need

| File | Notes |
| --- | --- |
| `carmageddon.apk` (any file name ending in `.apk`) | Carmageddon for Android **version 1.8.507**, `armeabi-v7a` |
| `main.507.com.stainlessgames.carmageddon.obb` | The game's expansion data file (about 78 MB) |

Only version 1.8.507 is supported: the port patches the game's code at fixed offsets. The APK is checked at
startup and other versions are rejected (`--skip-version-check` overrides this, at your own risk).

## Running

1. Put `carmadroid.exe`, `SDL2.dll`, the APK and the OBB in the same folder.
2. Run `carmadroid.exe`.

Saves, settings and a log file (`carmadroid.log`) are kept in the `userdata` folder next to the exe.
The APK and OBB can also be passed explicitly: `carmadroid.exe --apk <file> --obb <file>`.

### Controls

Menus are driven with the **mouse** (it acts as your finger on the touchscreen). **Esc** is the Android back
button. **F11** or **Alt+Enter** toggles fullscreen.

In races, a gamepad (anything SDL recognises: Xbox, PlayStation, and so on) drives the car with analog
steering and throttle:

| Pad | Action |
| --- | --- |
| Left stick / D-pad left-right | Steer (analog) |
| Right trigger | Accelerate (analog) |
| Left trigger | Brake / reverse (analog) |
| A or RB (hold) | Handbrake |
| B | Repair |
| X | Change camera |
| Y | Recover car |
| LB | Pratcam on/off |
| Start | Pause / resume |
| Back | Android back |

While a pad is connected, the game's control settings show TILT: that is how the analog input is fed in.
Unplugging the pad restores your touch control settings.

### Options

| Option | Effect |
| --- | --- |
| `--fullscreen` | Start in fullscreen |
| `--render WxH` | Render at an exact resolution (default: desktop aspect ratio, capped at 1920x1080) |
| `--max-res WxH` | Change the render resolution cap |
| `--vsync` | Enable vsync (default off: unlocked frame rate in races) |
| `--race-fps N` / `--menu-fps N` | Frame rate caps (defaults: races uncapped, menus 120) |
| `--audio-rate N` | FMOD mix rate in Hz (default 48000; the game's original is 24000) |
| `--censored` | Keep the Android version's pickup substitutions (see below) |
| `--data DIR` | Use a different user data folder |
| `-v` / `-vv` | More logging |

## Differences from the original Android game

- **Gamepad support** in races (the Android game only has touch and tilt controls).
- **Resolution**: renders up to 1920x1080 off-screen and scales to any window size.
- **Frame rate**: unlocked in races.
- **Audio**: mixed at 48 kHz with spline resampling instead of 24 kHz linear.
- **Restored pickups**: the Android build silently swaps three pickup types for others
  (including *Drugs!!!*, which becomes Grip-o-matic tyres). This is undone unless `--censored` is given.
- Online features (store, Facebook, ads, analytics) are disabled.

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 (C++ workload), CMake 3.20+.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build/Release/carmadroid.exe` plus `SDL2.dll`. CMake downloads the dependencies automatically:

- [dynarmic](https://github.com/lioncash/dynarmic), ARM to x86-64 JIT (0BSD), built from source (A32 frontend only)
- [Boost](https://www.boost.org/) 1.86 headers, needed by dynarmic (Boost Software License)
- [SDL2](https://github.com/libsdl-org/SDL) 2.32.10, window, OpenGL, audio and gamepads (zlib license), prebuilt
- [miniz](https://github.com/richgel999/miniz), APK (zip) reading (MIT), vendored in `third_party/`

## How it works

| Part | Source |
| --- | --- |
| ELF loader: maps the `.so` files from the APK, applies relocations, links imports | `src/elf_loader.cpp` |
| ARM CPU (dynarmic JIT), host-to-guest calls, import thunks, function hooks | `src/cpu.cpp` |
| C library, pthreads, stdio, time, wide chars | `src/hle/libc_*.cpp`, `src/hle/pthread.cpp` |
| Android NDK: looper, input queue, sensors, assets, config | `src/hle/android.cpp` |
| Fake JVM for the game's Java calls | `src/hle/jni.cpp`, `src/game_java.cpp` |
| OpenGL ES 1.1 to desktop OpenGL, EGL | `src/hle/gles.cpp`, `src/hle/egl.cpp` |
| FMOD audio output (replaces the Java AudioTrack device) | `src/audio.cpp` |
| Gamepad input | `src/controller.cpp` |
| Content restoration | `src/content_patches.cpp` |
| Window, rendering, frame pacing | `src/platform.cpp` |

Analog steering and throttle use the game's own tilt-control code: the stick and triggers are converted into
the exact accelerometer vector that produces the wanted steering and throttle values. The pad buttons call
the game's own handbrake, repair, camera, recover, pratcam and pause functions directly.

## Legal

This project contains no code or data from Carmageddon. Carmageddon is a trademark of Stainless Games Ltd.
You must own the game to use this project. This is an unofficial fan project, not affiliated with or
endorsed by Stainless Games.

Licensed under the GNU General Public License v2.0 (see `LICENSE`).
