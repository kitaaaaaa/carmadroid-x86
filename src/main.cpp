// carmadroid-x86: runs the Android build of Carmageddon (libParsons.so) on Windows.
#include "common.h"
#include "cpu.h"
#include "elf_loader.h"
#include "game_java.h"
#include "platform.h"
#include "debug_tools.h"
#include "controller.h"
#include "audio.h"
#include "content_patches.h"
#include "roster.h"
#include "camera_look.h"
#include "pc_content.h"
#include "pc_import.h"
#include "pcimport/cars.h"
#include "hle/android.h"
#include "hle/hle_common.h"
#include "apk.h"
#include "gamedata.h"
#include <windows.h>
#include <timeapi.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

int g_log_level = 1;
static std::mutex g_log_lock;
static FILE* g_log_file = nullptr;
static auto g_start = std::chrono::steady_clock::now();

void log_line(const char* tag, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    u32 tid = Cpu::current ? Cpu::current->thread_id : 0;
    std::lock_guard<std::mutex> l(g_log_lock);
    fprintf(stdout, "%8.3f [%u] %s %s\n", t, tid, tag, buf);
    fflush(stdout);
    if (g_log_file) {
        fprintf(g_log_file, "%8.3f [%u] %s %s\n", t, tid, tag, buf);
        fflush(g_log_file);
    }
}

static void error_box(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    MessageBoxA(nullptr, msg.c_str(), "carmadroid-x86", MB_OK | MB_ICONERROR);
}

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line("FATAL", "%s", buf);
    hle::dump_stats();
    fflush(stdout);
    error_box(std::string("Fatal error:\n\n") + buf + "\n\nSee userdata\\carmadroid.log for details.");
    ExitProcess(1);
}

static fs::path exe_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// Finds a file next to the exe (or in the working directory) whose name matches.
static fs::path find_file(const std::string& ext, const std::string& prefer) {
    fs::path best;
    for (const fs::path& dir : {exe_dir(), fs::current_path()}) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string name = e.path().filename().string();
            std::string lower = name;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            if (lower.size() < ext.size() || lower.compare(lower.size() - ext.size(), ext.size(), ext) != 0) continue;
            if (lower.find(prefer) != std::string::npos) return e.path();
            if (best.empty()) best = e.path();
        }
        if (!best.empty()) return best;
    }
    return best;
}

// CRC32 of lib/armeabi-v7a/libParsons.so in Carmageddon 1.8.507 (the only supported build: all
// patches and hooks use fixed code offsets).
constexpr u32 kSupportedLibCrc = 0xA6646F8D;

struct ScriptedTap { double t; float x, y; bool done; int key = 0; bool pad = false; bool button = false; bool look = false;
                     bool drag = false; float x1 = 0, y1 = 0; int steps = 12, step_ms = 25; };

int main(int argc, char** argv) {
    int exit_after = 0;
    bool pc_data_off = false;
    double prof_from = 0, prof_to = 0;
    platform::Options popt;
    std::string shots_dir;
    bool pad_sim = false;
    float pad_steer = 0, pad_throttle = 0;
    std::vector<ScriptedTap> taps;
    bool skip_version_check = false;
    std::string apk_arg, obb_arg, extract_dir, game_dir, splat_dir;
    hle::g_config.root = (exe_dir() / "userdata").string();

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") g_log_level = 2;
        else if (a == "-vv") g_log_level = 3;
        else if (a == "-q") g_log_level = 0;
        else if (a == "--apk" && i + 1 < argc) apk_arg = argv[++i];
        else if (a == "--obb" && i + 1 < argc) obb_arg = argv[++i];
        else if (a == "--extract-data")  // optional folder; default: gamedata next to the exe
            extract_dir = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "*default*";
        else if (a == "--game-dir" && i + 1 < argc) game_dir = argv[++i];
        else if ((a == "--data" || a == "--root") && i + 1 < argc) hle::g_config.root = argv[++i];
        else if (a == "--skip-version-check") skip_version_check = true;
        else if (a == "--shot-every" && i + 1 < argc) platform::g_shot_every = atoi(argv[++i]);
        else if (a == "--exit-after" && i + 1 < argc) exit_after = atoi(argv[++i]);
        else if (a == "--dump-zones") debug::g_dump_zones = true;
        else if (a == "--lua-source" && i + 2 < argc) {  // --lua-source <compiled .LOL> <out .lua> (developer)
            try {
                const std::string s = pcimport::lua_source(pcimport::read_file(argv[i + 1]));
                FILE* f = fopen(argv[i + 2], "wb");
                if (f) fwrite(s.data(), 1, s.size(), f), fclose(f);
                return f ? 0 : 1;
            } catch (const std::exception& e) {
                fprintf(stderr, "%s\n", e.what());
                return 1;
            }
        }
        else if (a == "--censored") content::g_restore = false;
        else if (a == "--unlock-all-cars") content::g_unlock_all_cars = true;
        else if (a == "--car" && i + 1 < argc) content::g_force_car = argv[++i];
        else if (a == "--pc-data" && i + 1 < argc) pc_content::g_dir = argv[++i];
        else if (a == "--splat-data" && i + 1 < argc) splat_dir = argv[++i];
        else if (a == "--no-pc-data") pc_data_off = true;
        else if (a == "--no-cockpit") pc_content::g_cockpit = false;
        else if (a == "--look-invert-x") camera_look::g_yaw_sign = -1;
        else if (a == "--look-invert-y") camera_look::g_pitch_sign = -1;
        else if (a == "--record-audio") {  // optional file name; default userdataudio.wav
            if (i + 1 < argc && argv[i + 1][0] != '-') audio::g_record_path = argv[++i];
            else audio::g_record_path = "*default*";
        }
        else if (a == "--jit-opt" && i + 1 < argc) g_jit_optimizations = (u32)strtoul(argv[++i], nullptr, 0);
        else if (a == "--jit-opt-audio" && i + 1 < argc) g_jit_optimizations_audio = (u32)strtoul(argv[++i], nullptr, 0);
        else if (a == "--audio-rate" && i + 1 < argc) audio::g_sample_rate = atoi(argv[++i]);
        else if (a == "--dump-file" && i + 1 < argc) debug::g_dump_file = argv[++i];
        else if (a == "--fullscreen") popt.fullscreen = true;
        else if (a == "--profile" && i + 1 < argc) {  // --profile from_seconds,to_seconds
            profiler::g_enabled = true;
            sscanf(argv[++i], "%lf,%lf", &prof_from, &prof_to);
        }
        else if (a == "--vsync") popt.vsync = true;
        else if (a == "--menu-fps" && i + 1 < argc) platform::g_menu_fps = atoi(argv[++i]);
        else if (a == "--race-fps" && i + 1 < argc) platform::g_race_fps = atoi(argv[++i]);
        else if (a == "--hidden") popt.hidden = true;
        else if (a == "--render" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &popt.render_w, &popt.render_h);
        else if (a == "--max-res" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &popt.max_w, &popt.max_h);
        else if (a == "--shots" && i + 1 < argc) shots_dir = argv[++i];
        else if (a == "--pad-sim" && i + 1 < argc) {  // --pad-sim steer,throttle
            float st, th;
            if (sscanf(argv[++i], "%f,%f", &st, &th) == 2) pad_sim = true, pad_steer = st, pad_throttle = th;
        }
        else if (a == "--accel" && i + 1 < argc) {
            float x, y, z;
            if (sscanf(argv[++i], "%f,%f,%f", &x, &y, &z) == 3) android::set_accelerometer(x, y, z);
        }
        else if (a == "--dump-wad" && i + 1 < argc) debug::g_dump_wad_dir = argv[++i];
        else if (a == "--pad-at" && i + 1 < argc) {  // --pad-at seconds,steer,throttle
            ScriptedTap t{};
            t.pad = true;
            if (sscanf(argv[++i], "%lf,%f,%f", &t.t, &t.x, &t.y) == 3) taps.push_back(t);
        }
        else if (a == "--pad-look-at" && i + 1 < argc) {  // --pad-look-at seconds,x,y (right stick)
            ScriptedTap t{};
            t.look = true;
            if (sscanf(argv[++i], "%lf,%f,%f", &t.t, &t.x, &t.y) == 3) taps.push_back(t);
        }
        else if (a == "--button-at" && i + 1 < argc) {  // --button-at seconds,action
            ScriptedTap t{};
            t.button = true;
            if (sscanf(argv[++i], "%lf,%d", &t.t, &t.key) == 2) taps.push_back(t);
        }
        else if (a == "--key" && i + 1 < argc) {  // --key seconds,androidKeycode
            ScriptedTap t{};
            if (sscanf(argv[++i], "%lf,%d", &t.t, &t.key) == 2) taps.push_back(t);
        }
        else if (a == "--drag-at" && i + 1 < argc) {  // --drag-at seconds,x0,y0,x1,y1 (mouse drag, testing)
            ScriptedTap t{};
            t.drag = true;
            if (sscanf(argv[++i], "%lf,%f,%f,%f,%f,%d,%d", &t.t, &t.x, &t.y, &t.x1, &t.y1, &t.steps, &t.step_ms) >= 5)
                taps.push_back(t);
        }
        else if (a == "--tap" && i + 1 < argc) {  // --tap seconds,x,y
            ScriptedTap t{};
            if (sscanf(argv[++i], "%lf,%f,%f", &t.t, &t.x, &t.y) == 3) taps.push_back(t);
        }
    }
    fs::create_directories(hle::g_config.root);
    g_log_file = fopen((hle::g_config.root + "/carmadroid.log").c_str(), "w");

    // --- PC Carmageddon (optional) --------------------------------------------------------------
    // --pc-data / --splat-data, or the PC game's CARMA / CARSPLAT folders next to the exe (CARSPLAT is
    // also looked for next to CARMA, as in the PC game's install folder).
    if (pc_data_off) {
        pc_content::g_dir.clear();
        splat_dir.clear();
    } else {
        if (pc_content::g_dir.empty())
            for (const fs::path& dir : {exe_dir() / "CARMA", exe_dir()})
                if (fs::exists(dir / "DATA" / "64X48X8" / "CARS")) { pc_content::g_dir = dir.string(); break; }
        if (splat_dir.empty()) {
            std::vector<fs::path> dirs{exe_dir() / "CARSPLAT"};
            if (!pc_content::g_dir.empty()) dirs.push_back(fs::path(pc_content::g_dir).parent_path() / "CARSPLAT");
            for (const fs::path& dir : dirs)
                if (fs::exists(dir / "DATA" / "CARS")) { splat_dir = dir.string(); break; }
        }
    }
    pc_content::g_splat_dir = splat_dir;
    pc_content::g_bonnet_fits_path = (exe_dir() / "bonnet_fits.txt").string();

    // --- Game files -------------------------------------------------------------------------
    // Unpacked game data (--extract-data) in the gamedata folder next to the exe is used automatically.
    const fs::path default_game_dir = exe_dir() / "gamedata";
    if (extract_dir == "*default*") extract_dir = default_game_dir.string();
    if (game_dir.empty() && extract_dir.empty() && fs::exists(default_game_dir / "lib" / "armeabi-v7a" / "libParsons.so") &&
        fs::is_directory(default_game_dir / "DATA"))
        game_dir = default_game_dir.string();
    // The code patches use fixed offsets in libParsons.so 1.8.507: check it (in the APK or folder opened
    // with apk::open/open_dir) before anything runs or changes the game files.
    auto version_ok = [&](const std::string& source) {
        const u32 crc = apk::crc32_of("lib/armeabi-v7a/libParsons.so");
        if (crc == kSupportedLibCrc || skip_version_check) return true;
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Unsupported APK: %s\n\nThis port needs Carmageddon for Android version 1.8.507 (armeabi-v7a).\n"
                 "(libParsons.so CRC %08X, expected %08X.) Use --skip-version-check to try anyway.",
                 source.c_str(), crc, kSupportedLibCrc);
        error_box(msg);
        return false;
    };
    fs::path apk_path = apk_arg.empty() ? find_file(".apk", "carmageddon") : fs::path(apk_arg);
    fs::path obb_path = obb_arg.empty() ? find_file(".obb", "carmageddon") : fs::path(obb_arg);
    if (!game_dir.empty()) {
        // Run from an unpacked folder (see --extract-data): libraries and assets from the folder, and
        // the DATA tree packed into an OBB (only repacked when files changed).
        if (!apk::open_dir(game_dir)) {
            error_box("Game folder not found: " + game_dir);
            return 1;
        }
        LOGI("running from the unpacked game data in %s", game_dir.c_str());
        if (!version_ok(game_dir)) return 1;
        // Convert and install the PC content (only when it isn't installed yet, or changed).
        pc_import::install(game_dir, pc_content::g_dir, splat_dir);
        std::string err;
        const std::string packed = hle::g_config.root + "/gamedata.obb";
        if (!gamedata::build_obb(game_dir, packed, err)) {
            error_box("Could not pack the game data in " + game_dir + ":\n" + err);
            return 1;
        }
        apk_path = fs::u8path(game_dir);
        obb_path = fs::u8path(packed);
    } else if (apk_path.empty() || !fs::exists(apk_path) || obb_path.empty() || !fs::exists(obb_path)) {
        error_box("Game files not found.\n\n"
                  "Put these two files next to carmadroid.exe:\n"
                  "  - the Carmageddon APK (version 1.8.507, armeabi-v7a)\n"
                  "  - main.507.com.stainlessgames.carmageddon.obb\n\n"
                  "or pass them with --apk <file> --obb <file>.");
        return 1;
    } else if (!extract_dir.empty()) {
        std::string err;
        pc_import::uninstall(extract_dir);  // unpacking again: start from the original files
        if (!gamedata::extract(apk_path.string(), obb_path.string(), extract_dir, err)) {
            error_box("Extracting the game data failed:\n" + err);
            return 1;
        }
        const std::string done =
            "Game data extracted to " + extract_dir + "\n\n" +
            (fs::path(extract_dir) == default_game_dir ? std::string("It is used automatically from now on (delete the folder to go back to the APK and OBB).")
                                                       : "Run it with --game-dir \"" + extract_dir + "\"");
        fprintf(stderr, "%s\n", done.c_str());
        if (!popt.hidden) MessageBoxA(nullptr, done.c_str(), "carmadroid-x86", MB_OK | MB_ICONINFORMATION);
        return 0;
    } else if (!apk::open(apk_path.string())) {
        error_box("Could not open " + apk_path.string() + " as an APK (zip) file.");
        return 1;
    }
    if (game_dir.empty() && !version_ok(apk_path.filename().string())) return 1;  // (unpacked data: checked above)
    hle::g_config.apk = apk_path.string();
    hle::g_config.obb_host = obb_path.string();
    {
        // FMOD reads /proc/cpuinfo to detect VFP/NEON before enabling its Android output.
        fs::create_directories(hle::g_config.root + "/fs/proc");
        FILE* f = fopen((hle::g_config.root + "/fs/proc/cpuinfo").c_str(), "wb");
        fputs(
            "Processor\t: ARMv7 Processor rev 10 (v7l)\n"
            "processor\t: 0\n"
            "BogoMIPS\t: 1990.65\n"
            "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls\n"
            "CPU implementer\t: 0x41\n"
            "CPU architecture: 7\n"
            "CPU part\t: 0xc09\n"
            "Hardware\t: carmadroid\n"
            , f);
        fclose(f);
    }
    platform::g_shot_dir = shots_dir.empty() ? hle::g_config.root + "/shots" : shots_dir;
    if (platform::g_shot_every) fs::create_directories(platform::g_shot_dir);
    if (audio::g_record_path == "*default*") audio::g_record_path = hle::g_config.root + "/audio.wav";
    if (!audio::g_record_path.empty()) LOGI("recording audio to %s", audio::g_record_path.c_str());
    LOGI("APK: %s", hle::g_config.apk.c_str());
    LOGI("OBB: %s", hle::g_config.obb_host.c_str());
    LOGI("user data: %s", hle::g_config.root.c_str());

    timeBeginPeriod(1);  // 1 ms sleep granularity for guest usleep/nanosleep
    if (!platform::init(popt)) fatal("could not create the game window");
    if (pad_sim) controller::simulate(pad_steer, pad_throttle);
    mem::init();
    hle::init();
    hle::init_libc_data();
    hle::init_stdio_data();

    Cpu main_cpu(1, 2 * 1024 * 1024);
    Cpu::current = &main_cpu;

    // Dependencies first so their exports satisfy libParsons' imports.
    auto load_from_apk = [](const char* name, u32 base) {
        std::vector<u8> image;
        if (!apk::read(std::string("lib/armeabi-v7a/") + name, image)) fatal("%s is missing from the APK", name);
        return loader::load_image(name, image, base);
    };
    Module* fmodex = load_from_apk("libfmodex.so", 0x11000000);
    Module* fmodevent = load_from_apk("libfmodevent.so", 0x11400000);
    Module* parsons = load_from_apk("libParsons.so", 0x10000000);
    for (Module* m : {fmodex, fmodevent, parsons}) {
        if (!m->unresolved.empty()) {
            std::string list;
            for (auto& n : m->unresolved) if (!hle::is_implemented(n)) list += n + " ";
            if (!list.empty()) LOGI("%s: imports without host implementation: %s", m->name.c_str(), list.c_str());
        }
    }
    controller::apply_patches();
    content::apply();
    roster::apply();
    camera_look::apply_patches();
    pc_content::apply_patches();
    audio::apply_patches();
    for (Module* m : {fmodex, fmodevent, parsons}) loader::run_initializers(m);

    game_java::on_create(main_cpu);
    game_java::on_start(main_cpu);
    game_java::on_resume(main_cpu);
    game_java::on_window_created(main_cpu);
    game_java::on_input_queue_created(main_cpu);
    game_java::on_focus(main_cpu, true);

    LOGI("lifecycle done; running");
    auto t0 = std::chrono::steady_clock::now();
    while (platform::pump_events()) {
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (exit_after && el > exit_after) break;
        if (profiler::g_enabled) {
            bool want = el >= prof_from && el < prof_to;
            if (want != profiler::g_active) {
                profiler::g_active = want;
                LOGI("profiler %s", want ? "started" : "stopped");
                if (!want) profiler::report();
            }
        }
        for (auto& t : taps)
            if (!t.done && el >= t.t) {
                t.done = true;
                if (t.look) controller::simulate_look(t.x, t.y);
                else if (t.button) controller::simulate_button(t.key);
                else if (t.pad) controller::simulate(t.x, t.y);
                else if (t.key) platform::inject_key(t.key);
                else if (t.drag) {
                    platform::touch_down(0, t.x, t.y);
                    for (int k = 1; k <= t.steps; k++) {
                        SDL_Delay(t.step_ms);
                        platform::touch_move(0, t.x + (t.x1 - t.x) * k / t.steps, t.y + (t.y1 - t.y) * k / t.steps);
                    }
                    SDL_Delay(t.step_ms);
                    platform::touch_up(0);
                    LOGI("injected drag %.0f,%.0f -> %.0f,%.0f", t.x, t.y, t.x1, t.y1);
                }
                else platform::inject_tap(t.x, t.y);
            }
    }
    LOGI("window closed");
    hle::dump_stats();
    ExitProcess(0);
}
