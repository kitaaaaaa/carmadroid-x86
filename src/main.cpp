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
#include "hle/android.h"
#include "hle/hle_common.h"
#include "apk.h"
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

struct ScriptedTap { double t; float x, y; bool done; int key = 0; bool pad = false; bool button = false; };

int main(int argc, char** argv) {
    int exit_after = 0;
    double prof_from = 0, prof_to = 0;
    platform::Options popt;
    std::string shots_dir;
    bool pad_sim = false;
    float pad_steer = 0, pad_throttle = 0;
    std::vector<ScriptedTap> taps;
    bool skip_version_check = false;
    std::string apk_arg, obb_arg;
    hle::g_config.root = (exe_dir() / "userdata").string();

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") g_log_level = 2;
        else if (a == "-vv") g_log_level = 3;
        else if (a == "-q") g_log_level = 0;
        else if (a == "--apk" && i + 1 < argc) apk_arg = argv[++i];
        else if (a == "--obb" && i + 1 < argc) obb_arg = argv[++i];
        else if ((a == "--data" || a == "--root") && i + 1 < argc) hle::g_config.root = argv[++i];
        else if (a == "--skip-version-check") skip_version_check = true;
        else if (a == "--shot-every" && i + 1 < argc) platform::g_shot_every = atoi(argv[++i]);
        else if (a == "--exit-after" && i + 1 < argc) exit_after = atoi(argv[++i]);
        else if (a == "--dump-zones") debug::g_dump_zones = true;
        else if (a == "--censored") content::g_restore = false;
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
        else if (a == "--button-at" && i + 1 < argc) {  // --button-at seconds,action
            ScriptedTap t{};
            t.button = true;
            if (sscanf(argv[++i], "%lf,%d", &t.t, &t.key) == 2) taps.push_back(t);
        }
        else if (a == "--key" && i + 1 < argc) {  // --key seconds,androidKeycode
            ScriptedTap t{};
            if (sscanf(argv[++i], "%lf,%d", &t.t, &t.key) == 2) taps.push_back(t);
        }
        else if (a == "--tap" && i + 1 < argc) {  // --tap seconds,x,y
            ScriptedTap t{};
            if (sscanf(argv[++i], "%lf,%f,%f", &t.t, &t.x, &t.y) == 3) taps.push_back(t);
        }
    }
    fs::create_directories(hle::g_config.root);
    g_log_file = fopen((hle::g_config.root + "/carmadroid.log").c_str(), "w");

    // --- Game files -------------------------------------------------------------------------
    fs::path apk_path = apk_arg.empty() ? find_file(".apk", "carmageddon") : fs::path(apk_arg);
    fs::path obb_path = obb_arg.empty() ? find_file(".obb", "carmageddon") : fs::path(obb_arg);
    if (apk_path.empty() || !fs::exists(apk_path) || obb_path.empty() || !fs::exists(obb_path)) {
        error_box("Game files not found.\n\n"
                  "Put these two files next to carmadroid.exe:\n"
                  "  - the Carmageddon APK (version 1.8.507, armeabi-v7a)\n"
                  "  - main.507.com.stainlessgames.carmageddon.obb\n\n"
                  "or pass them with --apk <file> --obb <file>.");
        return 1;
    }
    if (!apk::open(apk_path.string())) {
        error_box("Could not open " + apk_path.string() + " as an APK (zip) file.");
        return 1;
    }
    const u32 crc = apk::crc32_of("lib/armeabi-v7a/libParsons.so");
    if (crc != kSupportedLibCrc && !skip_version_check) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Unsupported APK: %s\n\nThis port needs Carmageddon for Android version 1.8.507 (armeabi-v7a).\n"
                 "(libParsons.so CRC %08X, expected %08X.) Use --skip-version-check to try anyway.",
                 apk_path.filename().string().c_str(), crc, kSupportedLibCrc);
        error_box(msg);
        return 1;
    }
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
                if (t.button) controller::simulate_button(t.key);
                else if (t.pad) controller::simulate(t.x, t.y);
                else if (t.key) platform::inject_key(t.key);
                else platform::inject_tap(t.x, t.y);
            }
    }
    LOGI("window closed");
    hle::dump_stats();
    ExitProcess(0);
}
