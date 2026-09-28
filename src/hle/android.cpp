// Android NDK: logging, ALooper, AInputQueue/events, sensors, assets, configuration.
#include "android.h"
#include "fds.h"
#include "hle_common.h"
#include "../apk.h"
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace hle;
using mem::ptr;
using mem::str;

// ============================================================================
// Logging
// ============================================================================
static const char* prio_name(u32 p) {
    static const char* n[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
    return p < 9 ? n[p] : "?";
}
static void android_log(u32 prio, const char* tag, const std::string& msg) {
    std::string m = msg;
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    log_line(prio_name(prio), "[%s] %s", tag ? tag : "", m.c_str());
}
HLE(__android_log_print) {
    RegArgs a(c, 3);
    android_log(c.r(0), str(c.r(1)), format(str(c.r(2)), a));
    c.ret(1);
}
HLE(__android_log_vprint) {
    VaArgs a(c.r(3));
    android_log(c.r(0), str(c.r(1)), format(str(c.r(2)), a));
    c.ret(1);
}
HLE(__android_log_write) {
    android_log(c.r(0), str(c.r(1)), str(c.r(2)));
    c.ret(1);
}

namespace android {

// ============================================================================
// Window
// ============================================================================
static int g_win_w = 1280, g_win_h = 720;
static u32 g_window = 0;
void set_window_size(int w, int h) { g_win_w = w; g_win_h = h; }
void get_window_size(int& w, int& h) { w = g_win_w; h = g_win_h; }
u32 native_window_handle() {
    if (!g_window) g_window = mem::calloc(1, 64);
    return g_window;
}

// ============================================================================
// Input queue
// ============================================================================
static std::mutex g_input_lock;
static std::deque<u32> g_input_events;  // guest addresses of InputEvent copies
static u32 g_input_queue = 0;

u32 input_queue_handle() {
    if (!g_input_queue) g_input_queue = mem::calloc(1, 16);
    return g_input_queue;
}

void push_input(const InputEvent& e) {
    u32 g = mem::malloc(sizeof(InputEvent));
    memcpy(ptr(g), &e, sizeof e);
    {
        std::lock_guard<std::mutex> l(g_input_lock);
        g_input_events.push_back(g);
    }
    { std::lock_guard<std::mutex> l(fds::g_lock); }
    fds::g_any_data.notify_all();
}

static bool input_ready() {
    std::lock_guard<std::mutex> l(g_input_lock);
    return !g_input_events.empty();
}

// ============================================================================
// Sensors
// ============================================================================
static std::mutex g_accel_lock;
static float g_accel[3] = {0.0f, 0.0f, 9.80665f};  // device flat on a table
static u64 g_last_sensor_ns = 0;
static bool g_sensor_enabled = false;

void set_accelerometer(float x, float y, float z) {
    std::lock_guard<std::mutex> l(g_accel_lock);
    g_accel[0] = x;
    g_accel[1] = y;
    g_accel[2] = z;
}

static u64 now_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool sensor_ready() { return g_sensor_enabled && now_ns() - g_last_sensor_ns >= 16'000'000ull; }

}  // namespace android

using namespace android;

// ============================================================================
// ALooper
// ============================================================================
namespace {
struct LooperSource {
    s32 fd;         // guest fd, or -1 for synthetic sources
    s32 ident;
    u32 callback;
    u32 data;
    std::function<bool()> ready;
};
struct Looper {
    std::vector<LooperSource> sources;
};
std::mutex g_looper_lock;
std::vector<Looper*> g_loopers;  // guest handle = index + 1 (tagged)
thread_local u32 t_looper = 0;

Looper* looper_from(u32 h) {
    std::lock_guard<std::mutex> l(g_looper_lock);
    u32 i = h - 0x4C000000;
    return i < g_loopers.size() ? g_loopers[i] : nullptr;
}
u32 new_looper() {
    std::lock_guard<std::mutex> l(g_looper_lock);
    g_loopers.push_back(new Looper());
    return 0x4C000000 + (u32)g_loopers.size() - 1;
}
void add_source(u32 looper, LooperSource s) {
    Looper* lp = looper_from(looper);
    if (!lp) { LOGE("ALooper_addFd: bad looper 0x%x", looper); return; }
    std::lock_guard<std::mutex> l(g_looper_lock);
    for (auto& e : lp->sources)
        if (e.fd == s.fd && s.fd >= 0) { e = s; return; }
    lp->sources.push_back(std::move(s));
}
void remove_source_by_ident(s32 ident) {
    std::lock_guard<std::mutex> l(g_looper_lock);
    for (Looper* lp : g_loopers)
        std::erase_if(lp->sources, [&](const LooperSource& s) { return s.ident == ident && s.fd < 0; });
}
}  // namespace

HLE(ALooper_prepare) {
    if (!t_looper) t_looper = new_looper();
    c.ret(t_looper);
}
HLE(ALooper_forThread) { c.ret(t_looper); }
HLE(ALooper_addFd) {
    // (looper, fd, ident, events, callback, data)
    s32 fd = (s32)c.r(1);
    LooperSource s{fd, (s32)c.r(2), c.arg(4), c.arg(5), [fd] { return fds::readable(fd); }};
    if (s.callback) LOGE("ALooper_addFd with callback not supported");
    add_source(c.r(0), s);
    c.ret(1);
}
HLE(ALooper_removeFd) {
    Looper* lp = looper_from(c.r(0));
    if (lp) {
        std::lock_guard<std::mutex> l(g_looper_lock);
        std::erase_if(lp->sources, [&](const LooperSource& s) { return s.fd == (s32)c.r(1); });
    }
    c.ret(1);
}
HLE(ALooper_wake) {
    { std::lock_guard<std::mutex> l(fds::g_lock); }
    fds::g_any_data.notify_all();
}
HLE(ALooper_pollAll) {
    // (timeoutMillis, outFd*, outEvents*, outData*)
    s32 timeout = (s32)c.r(0);
    u32 out_fd = c.r(1), out_events = c.r(2), out_data = c.r(3);
    Looper* lp = looper_from(t_looper);
    if (!lp) { c.ret((u32)-4); return; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout < 0 ? 0 : timeout);
    for (;;) {
        std::vector<LooperSource> srcs;
        {
            std::lock_guard<std::mutex> l(g_looper_lock);
            srcs = lp->sources;
        }
        for (auto& s : srcs) {
            if (s.ready && s.ready()) {
                if (out_fd) mem::w32(out_fd, (u32)s.fd);
                if (out_events) mem::w32(out_events, 1);
                if (out_data) mem::w32(out_data, s.data);
                c.ret((u32)s.ident);
                return;
            }
        }
        if (timeout == 0) { c.ret((u32)-3); return; }  // ALOOPER_POLL_TIMEOUT
        auto now = std::chrono::steady_clock::now();
        if (timeout > 0 && now >= deadline) { c.ret((u32)-3); return; }
        std::unique_lock<std::mutex> l(fds::g_lock);
        auto wake = now + std::chrono::milliseconds(4);  // also re-check timed sources (sensors)
        if (timeout > 0 && deadline < wake) wake = deadline;
        fds::g_any_data.wait_until(l, wake);
    }
}
HLE(ALooper_pollOnce) { hle_fn_ALooper_pollAll(c); }

// ============================================================================
// AInputQueue / AInputEvent
// ============================================================================
enum { LOOPER_ID_INPUT_DEFAULT = 2 };
HLE(AInputQueue_attachLooper) {
    // (queue, looper, ident, callback, data)
    add_source(c.r(1), LooperSource{-1, (s32)c.r(2), c.arg(3), c.arg(4), input_ready});
    LOGI("input queue attached (ident %d)", (int)c.r(2));
}
HLE(AInputQueue_detachLooper) {}
HLE(AInputQueue_hasEvents) { c.ret(input_ready() ? 1 : 0); }
HLE(AInputQueue_getEvent) {
    std::lock_guard<std::mutex> l(g_input_lock);
    if (g_input_events.empty()) { c.ret((u32)-1); return; }
    mem::w32(c.r(1), g_input_events.front());
    g_input_events.pop_front();
    c.ret(0);
}
HLE(AInputQueue_preDispatchEvent) { c.ret(0); }
HLE(AInputQueue_finishEvent) { mem::free(c.r(1)); }

static InputEvent* ev(Cpu& c) { return ptr<InputEvent>(c.r(0)); }
HLE(AInputEvent_getType) { c.ret((u32)ev(c)->type); }
HLE(AInputEvent_getSource) { c.ret((u32)ev(c)->source); }
HLE(AInputEvent_getDeviceId) { c.ret(1); }
HLE(AKeyEvent_getKeyCode) { c.ret((u32)ev(c)->keycode); }
HLE(AKeyEvent_getAction) { c.ret((u32)ev(c)->action); }
HLE(AKeyEvent_getMetaState) { c.ret((u32)ev(c)->meta); }
HLE(AKeyEvent_getRepeatCount) { c.ret(0); }
HLE(AMotionEvent_getAction) { c.ret((u32)ev(c)->action); }
HLE(AMotionEvent_getPointerCount) { c.ret((u32)ev(c)->pointer_count); }
static const Pointer& ptr_at(Cpu& c) {
    InputEvent* e = ev(c);
    u32 i = c.r(1);
    return e->pointers[i < 10 ? i : 0];
}
HLE(AMotionEvent_getPointerId) { c.ret((u32)ptr_at(c).id); }
HLE(AMotionEvent_getX) { c.retf(ptr_at(c).x); }
HLE(AMotionEvent_getY) { c.retf(ptr_at(c).y); }
HLE(AMotionEvent_getRawX) { c.retf(ptr_at(c).x); }
HLE(AMotionEvent_getRawY) { c.retf(ptr_at(c).y); }
HLE(AMotionEvent_getPressure) { c.retf(1.0f); }
HLE(AMotionEvent_getSize) { c.retf(0.1f); }

// ============================================================================
// Sensors
// ============================================================================
static u32 g_sensor_mgr = 0, g_accel_sensor = 0, g_sensor_list = 0;
static u32 sensor_mgr() {
    if (!g_sensor_mgr) {
        g_sensor_mgr = mem::calloc(1, 16);
        g_accel_sensor = mem::calloc(1, 16);
        g_sensor_list = mem::calloc(1, 8);
        mem::w32(g_sensor_list, g_accel_sensor);
    }
    return g_sensor_mgr;
}
HLE(ASensorManager_getInstance) { c.ret(sensor_mgr()); }
HLE(ASensorManager_getDefaultSensor) {
    sensor_mgr();
    c.ret(c.r(1) == 1 ? g_accel_sensor : 0);  // only ASENSOR_TYPE_ACCELEROMETER
}
HLE(ASensorManager_getSensorList) {
    sensor_mgr();
    mem::w32(c.r(1), g_sensor_list);
    c.ret(1);
}
HLE(ASensorManager_createEventQueue) {
    // (manager, looper, ident, callback, data)
    add_source(c.r(1), LooperSource{-1, (s32)c.r(2), c.r(3), c.arg(4), sensor_ready});
    c.ret(mem::calloc(1, 16));
}
HLE(ASensorManager_destroyEventQueue) { c.ret(0); }
HLE(ASensorEventQueue_enableSensor) { g_sensor_enabled = true; c.ret(0); }
HLE(ASensorEventQueue_disableSensor) { g_sensor_enabled = false; c.ret(0); }
HLE(ASensorEventQueue_setEventRate) { c.ret(0); }
HLE(ASensorEventQueue_getEvents) {
    // (queue, ASensorEvent* events, count) ; ASensorEvent is 104 bytes
    if (!sensor_ready() || c.r(2) == 0) { c.ret(0); return; }
    g_last_sensor_ns = now_ns();
    u32 e = c.r(1);
    memset(ptr(e), 0, 104);
    mem::w32(e + 0, 104);
    mem::w32(e + 4, 1);
    mem::w32(e + 8, 9);  // ASENSOR_TYPE_GRAVITY: the game applies no low-pass filter to these (no input lag)
    u64 ts = g_last_sensor_ns;
    memcpy(ptr(e + 16), &ts, 8);
    {
        std::lock_guard<std::mutex> l(g_accel_lock);
        memcpy(ptr(e + 24), g_accel, 12);
    }
    c.ret(1);
}
HLE(ASensor_getName) { static u32 s = mem::strdup("Virtual Accelerometer"); c.ret(s); }
HLE(ASensor_getVendor) { static u32 s = mem::strdup("carmadroid"); c.ret(s); }
HLE(ASensor_getType) { c.ret(1); }
HLE(ASensor_getResolution) { c.retf(0.01f); }
HLE(ASensor_getMinDelay) { c.ret(10000); }

// ============================================================================
// AConfiguration: plausible values for a landscape phone in English
// ============================================================================
HLE(AConfiguration_new) { c.ret(mem::calloc(1, 64)); }
HLE(AConfiguration_delete) { mem::free(c.r(0)); }
HLE(AConfiguration_fromAssetManager) {}
HLE(AConfiguration_getLanguage) { mem::w8(c.r(1), 'e'); mem::w8(c.r(1) + 1, 'n'); }
HLE(AConfiguration_getCountry) { mem::w8(c.r(1), 'G'); mem::w8(c.r(1) + 1, 'B'); }
HLE(AConfiguration_getMcc) { c.ret(0); }
HLE(AConfiguration_getMnc) { c.ret(0); }
HLE(AConfiguration_getOrientation) { c.ret(2); }  // landscape
HLE(AConfiguration_getTouchscreen) { c.ret(3); }  // finger
HLE(AConfiguration_getDensity) { c.ret(240); }
HLE(AConfiguration_getKeyboard) { c.ret(1); }     // nokeys
HLE(AConfiguration_getNavigation) { c.ret(1); }   // nonav
HLE(AConfiguration_getKeysHidden) { c.ret(1); }
HLE(AConfiguration_getNavHidden) { c.ret(1); }
HLE(AConfiguration_getSdkVersion) { c.ret(22); }
HLE(AConfiguration_getScreenSize) { c.ret(3); }   // large
HLE(AConfiguration_getScreenLong) { c.ret(2); }
HLE(AConfiguration_getUiModeType) { c.ret(1); }
HLE(AConfiguration_getUiModeNight) { c.ret(1); }

// ============================================================================
// AAssetManager: served from the APK's assets folder on disk
// ============================================================================
namespace {
struct Asset {
    std::vector<u8> data;
    u32 pos = 0;
};
struct AssetDir {
    std::vector<std::string> names;
    size_t next = 0;
    u32 name_buf = 0;
};
std::mutex g_asset_lock;
std::unordered_map<u32, Asset*> g_assets;
std::unordered_map<u32, AssetDir*> g_asset_dirs;
}  // namespace

HLE(AAssetManager_open) {
    auto* a = new Asset();
    if (!apk::read(std::string("assets/") + str(c.r(1)), a->data)) {
        LOGV("AAssetManager_open(%s): not found", str(c.r(1)));
        delete a;
        c.ret(0);
        return;
    }
    u32 h = mem::calloc(1, 16);
    std::lock_guard<std::mutex> l(g_asset_lock);
    g_assets[h] = a;
    LOGV("AAssetManager_open(%s): %zu bytes", str(c.r(1)), a->data.size());
    c.ret(h);
}
static Asset* asset(u32 h) {
    std::lock_guard<std::mutex> l(g_asset_lock);
    auto it = g_assets.find(h);
    return it == g_assets.end() ? nullptr : it->second;
}
HLE(AAsset_close) {
    std::lock_guard<std::mutex> l(g_asset_lock);
    auto it = g_assets.find(c.r(0));
    if (it != g_assets.end()) { delete it->second; g_assets.erase(it); mem::free(c.r(0)); }
}
HLE(AAsset_read) {
    Asset* a = asset(c.r(0));
    if (!a) { c.ret((u32)-1); return; }
    u32 n = std::min<u32>(c.r(2), (u32)a->data.size() - a->pos);
    memcpy(ptr(c.r(1)), a->data.data() + a->pos, n);
    a->pos += n;
    c.ret(n);
}
HLE(AAsset_seek) {
    Asset* a = asset(c.r(0));
    if (!a) { c.ret((u32)-1); return; }
    s64 off = (s32)c.r(1);
    s64 base = c.r(2) == 0 ? 0 : c.r(2) == 1 ? a->pos : (s64)a->data.size();
    s64 np = base + off;
    if (np < 0 || np > (s64)a->data.size()) { c.ret((u32)-1); return; }
    a->pos = (u32)np;
    c.ret(a->pos);
}
HLE(AAsset_getLength) { Asset* a = asset(c.r(0)); c.ret(a ? (u32)a->data.size() : 0); }
HLE(AAsset_getRemainingLength) { Asset* a = asset(c.r(0)); c.ret(a ? (u32)a->data.size() - a->pos : 0); }
HLE(AAssetManager_openDir) {
    auto* d = new AssetDir();
    d->names = apk::list(std::string("assets/") + str(c.r(1)));
    d->name_buf = mem::malloc(260);
    u32 h = mem::calloc(1, 16);
    std::lock_guard<std::mutex> l(g_asset_lock);
    g_asset_dirs[h] = d;
    c.ret(h);
}
HLE(AAssetDir_getNextFileName) {
    std::lock_guard<std::mutex> l(g_asset_lock);
    auto it = g_asset_dirs.find(c.r(0));
    if (it == g_asset_dirs.end() || it->second->next >= it->second->names.size()) { c.ret(0); return; }
    AssetDir* d = it->second;
    strncpy(ptr<char>(d->name_buf), d->names[d->next++].c_str(), 259);
    c.ret(d->name_buf);
}
HLE(AAssetDir_close) {
    std::lock_guard<std::mutex> l(g_asset_lock);
    auto it = g_asset_dirs.find(c.r(0));
    if (it != g_asset_dirs.end()) { mem::free(it->second->name_buf); delete it->second; g_asset_dirs.erase(it); }
}

// ============================================================================
// ANativeWindow
// ============================================================================
HLE(ANativeWindow_setBuffersGeometry) { c.ret(0); }
HLE(ANativeWindow_getWidth) { int w, h; get_window_size(w, h); c.ret((u32)w); }
HLE(ANativeWindow_getHeight) { int w, h; get_window_size(w, h); c.ret((u32)h); }
HLE(ANativeWindow_acquire) {}
HLE(ANativeWindow_release) {}
