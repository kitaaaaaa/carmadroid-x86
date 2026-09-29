# carmadroid-x86

Runs the **Android version of Carmageddon** (Stainless Games, 2013, v1.8.507) natively on 64-bit Windows,
with analog gamepad support, up to 1080p rendering, an unlocked frame rate and restored content. It can
also use the original PC Carmageddon's cockpit graphics for the in-car view, and unpack the game data for
modding.

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

## Download

Prebuilt Windows builds are on the [Releases page](https://github.com/kitaaaaaa/carmadroid-x86/releases).
Every push to `main` is also built automatically (see the Actions tab for the latest build artifact).

### What's new in 0.2.0

- **Look around with the right stick.** In the in-car view it snaps to 80 degrees left or right, and pulling
  back looks behind you.
- **PC cockpit (optional).** With a copy of the PC game, the in-car view gets the original dashboard, with a
  working speedo, rev counter, gear display and damage lights, and hands that turn the wheel.
- **Map and action replay on the D-pad** (up and down), instead of swiping from the screen edges.
- **On-screen touch buttons are hidden** while a pad is connected.
- **Modding:** `--extract-data` unpacks the game data into a folder, and `--game-dir` runs from it.

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
| Right stick | Look around. In-car view: left/right snaps to 80 degrees, pull back to look behind |
| D-pad up | Track map on/off |
| D-pad down | Action replay on/off |
| LB | Pratcam on/off |
| Start | Pause / resume |
| Back | Android back |

While a pad is connected, the game's control settings show TILT: that is how the analog input is fed in.
If looking around goes the wrong way, use `--look-invert-x` / `--look-invert-y`.
Unplugging the pad restores your touch control settings. The on-screen touch buttons are hidden while a pad
is connected.

### Options

| Option | Effect |
| --- | --- |
| `--fullscreen` | Start in fullscreen |
| `--render WxH` | Render at an exact resolution (default: desktop aspect ratio, capped at 1920x1080) |
| `--max-res WxH` | Change the render resolution cap |
| `--vsync` | Enable vsync (default off: unlocked frame rate in races) |
| `--race-fps N` / `--menu-fps N` | Frame rate caps (defaults: races uncapped, menus 120) |
| `--audio-rate N` | FMOD mix rate in Hz (default 48000; the game's original is 24000) |
| `--pc-data DIR` | Use content from the PC Carmageddon (the folder containing `DATA`, e.g. `...\Carmageddon1\CARMA`); see below |
| `--no-pc-data` / `--no-cockpit` | Don't use PC content / don't draw the PC cockpit |
| `--look-invert-x` / `--look-invert-y` | Invert right-stick look-around |
| `--unlock-all-cars` | Unlock every car (the game's own cheat; not saved) |
| `--censored` | Keep the Android version's pickup substitutions (see below) |
| `--jit-opt MASK` | dynarmic JIT optimization flags (default `0`, off; `0xFFFF` loads faster but garbles some sounds) |
| `--record-audio [FILE]` | Record the audio output to a WAV file (default `userdata\audio.wav`) |
| `--extract-data DIR` | Unpack the APK and OBB into a folder for modding, then exit (see below) |
| `--game-dir DIR` | Run from an unpacked folder instead of the APK and OBB |
| `--data DIR` | Use a different user data folder |
| `-v` / `-vv` | More logging |

### Optional: PC Carmageddon content

If you own the original PC Carmageddon, the port can use some of its assets for things the Android version
lacks. Either pass `--pc-data <folder>` or put the PC game's `CARMA` folder next to `carmadroid.exe`.
Currently this adds the **in-car cockpit** to the Android game's in-car camera: the car's dashboard with a
working speedo, rev counter, gear display and damage lights, side views when looking around with the right
stick, and hands that turn the wheel. While the dashboard is shown, the HUD's own damage graph and speedo are
hidden, since the dashboard shows them. As in the PC game, a damage light only comes on once that part is more
than 20% damaged. Cars that don't exist in the PC game keep the normal Android view.

Without the PC files, the in-car view is exactly as in the Android game. Nothing from the PC game is included.

### Modding: running from unpacked files

`carmadroid.exe --extract-data <folder>` unpacks everything the game uses into a folder:

| Folder | Contents |
| --- | --- |
| `lib/armeabi-v7a/` | The game's native libraries |
| `assets/` | APK assets (the music) |
| `DATA/` | The contents of the OBB: `CONTENT` (cars, tracks, UI, sounds, text) and `SETUP` |

`carmadroid.exe --game-dir <folder>` then runs the game from that folder, with no APK or OBB needed. The game only
reads its data from an OBB, so at startup the `DATA` folder is packed back into one (`userdata\gamedata.obb`,
uncompressed). That only happens when files in the folder have changed, and takes a few seconds. File and
folder names are upper-cased when packing, as in the original. The libraries must stay the 1.8.507 ones.

The data formats are Stainless's own (the engine is an early version of the one used in Carmageddon:
Reincarnation): `.CNT` (model hierarchy), `.MDL` (meshes), `.MTL` (materials), `.IMG` (textures), plus plain
text files for car and track setup and compiled Lua (`.LOL`) for the UI.

## Differences from the original Android game

- **Gamepad support** in races (the Android game only has touch and tilt controls), including looking
  around with the right stick.
- **PC cockpit** in the in-car view, if you have the PC game (optional).
- **Resolution**: renders up to 1920x1080 off-screen and scales to any window size.
- **Frame rate**: unlocked in races.
- **Audio**: mixed at 48 kHz with spline resampling instead of 24 kHz linear.
- **Restored pickups**: the Android build silently swaps three pickup types for others
  (including *Drugs!!!*, which becomes Grip-o-matic tyres). This is undone unless `--censored` is given.
- **Modding**: the game can be run from unpacked, editable files, and the car roster limit is raised from
  40 to 64 (`tools/pc2android` converts PC Carmageddon cars).
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
- [miniz](https://github.com/richgel999/miniz), APK (zip) and WAD reading (MIT), vendored in `third_party/`

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
| Right-stick look-around | `src/camera_look.cpp` |
| PC cockpit overlay (reads the PC game's PIX images and car files) | `src/pc_content.cpp` |
| Content restoration | `src/content_patches.cpp` |
| Unpacking and repacking the game data (Stainless WAD) | `src/gamedata.cpp` |
| Car roster limit (40 to 64) | `src/roster.cpp` |
| Window, rendering, frame pacing | `src/platform.cpp` |

Analog steering and throttle use the game's own tilt-control code: the stick and triggers are converted into
the exact accelerometer vector that produces the wanted steering and throttle values. The pad buttons call
the game's own handbrake, repair, camera, recover, pratcam, pause, map and action replay functions directly.

## Legal

This project contains no code or data from Carmageddon. Carmageddon is a trademark of Stainless Games Ltd.
You must own the game to use this project. This is an unofficial fan project, not affiliated with or
endorsed by Stainless Games.

Licensed under the GNU General Public License v2.0 (see `LICENSE`).
