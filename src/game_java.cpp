// Host replacement for the game's Java layer (ParsonsLoader + helper classes).
#include "game_java.h"
#include "elf_loader.h"
#include "hle/android.h"
#include "hle/hle_common.h"
#include "hle/jni.h"
#include "audio.h"

using jni::Value;

namespace {

const std::string PKG = "com/stainlessgames/carmageddon/";

Value str_value(const std::string& s) { Value v{}; v.i = jni::new_string(s); return v; }
Value int_value(u32 i) { Value v{}; v.i = i; return v; }

u32 g_activity = 0;   // guest ANativeActivity*
u32 g_callbacks = 0;  // guest ANativeActivityCallbacks*
u32 g_activity_obj = 0;

void define_java_methods() {
    // Class loading via the activity's class loader
    jni::define(PKG + "ParsonsLoader.getClassLoader", [](u32, auto&) { return int_value(jni::new_object("java/lang/ClassLoader")); });
    jni::define("java/lang/ClassLoader.loadClass", [](u32, const std::vector<Value>& a) {
        return int_value(jni::find_class(jni::string_value(a[0].i)));
    });

    // Paths
    jni::define(PKG + "AppEnvironmentInfo.GetAppInternalDataPath", [](u32, auto&) { return str_value(hle::guest_files_dir()); });
    jni::define(PKG + "AppEnvironmentInfo.GetAppOBBPath", [](u32, auto&) { return str_value(hle::guest_obb_path()); });

    // Display / device / locale info
    jni::define(PKG + "DisplayInfo.GetDPI", [](u32, auto&) { return int_value(240); });
    jni::define(PKG + "DisplayInfo.GetRotation", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "DeviceInfo.getDeviceModelId", [](u32, auto&) { return str_value("carmadroid"); });
    jni::define(PKG + "LocaleInfo.GetCountry", [](u32, auto&) { return str_value("GB"); });
    jni::define(PKG + "LocaleInfo.GetLanguage", [](u32, auto&) { return str_value("en"); });
    jni::define_static_field(PKG + "DisplayInfo.densityDPI", int_value(240));
    jni::define_static_field(PKG + "DisplayInfo.rotation", int_value(0));
    jni::define(PKG + "ParsonsLoader.native_getDeviceInfo_GetDeviceModelId", [](u32, auto&) { return str_value("carmadroid"); });
    jni::define(PKG + "ParsonsLoader.native_getDeviceInfo_GetAndroidVersionId", [](u32, auto&) { return str_value("22"); });
    jni::define(PKG + "ParsonsLoader.native_getDeviceInfo_GetBuildVersion", [](u32, auto&) { return str_value("1.8.507"); });
    jni::define(PKG + "ParsonsLoader.native_getDeviceInfo_GetStore", [](u32, auto&) { return str_value("GooglePlay"); });

    // UI state
    jni::define(PKG + "ParsonsLoader.native_isXOkeysSwapped", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.native_isNavigationVisible", [](u32, auto&) { return int_value(1); });  // NAVIGATIONHIDDEN_YES
    jni::define(PKG + "ParsonsLoader.native_isKeyboardVisible", [](u32, auto&) { return int_value(2); });    // hardKeyboardHidden YES
    jni::define(PKG + "ParsonsLoader.native_setIsNavigationVisible", [](u32, auto&) { return Value{}; });
    jni::define(PKG + "ParsonsLoader.native_setIsKeyboardVisible", [](u32, auto&) { return Value{}; });
    jni::define(PKG + "ParsonsLoader.native_showVirtualKeyboard", [](u32, auto&) { return Value{}; });
    jni::define(PKG + "ParsonsLoader.native_hideVirtualKeyboard", [](u32, auto&) { return Value{}; });
    jni::define(PKG + "ParsonsLoader.isMoviePlaying", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.playMovie", [](u32, const std::vector<Value>& a) {
        LOGI("playMovie(%s) skipped", jni::string_value(a[0].i).c_str());
        return Value{};
    });
    jni::define(PKG + "ParsonsLoader.native_quitToDashboard", [](u32, auto&) -> Value {
        LOGI("game requested quit");
        hle::dump_stats();
        exit(0);
    });

    // Store / social / ads: all unavailable
    jni::define(PKG + "ParsonsLoader.native_isBilllingSupported", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.native_itemPurchased", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.native_getItemPrice", [](u32, auto&) { return str_value(""); });
    jni::define(PKG + "ParsonsLoader.native_isFacebookLoggedIn", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.native_isInterstitialAdCached", [](u32, auto&) { return int_value(0); });
    jni::define(PKG + "ParsonsLoader.native_isGamesClientReady", [](u32, auto&) { return int_value(0); });
    for (const char* noop : {"native_requestPurchaseItem", "native_forceRestorePurchases", "native_facebookLogIn",
                             "native_facebookLogOut", "native_facebookInviteFriends", "native_facebookLikePage",
                             "native_showInterstitialAd", "native_showBannerAd", "native_openBrowserAtPage",
                             "native_openStoreAtAppPage", "native_showInputDialog"}) {
        std::string n = noop;
        jni::define(PKG + "ParsonsLoader." + n, [n](u32, auto&) { LOGI("Java %s ignored", n.c_str()); return Value{}; });
    }
}

u32 callback(int offset) { return mem::r32(g_callbacks + offset); }

u32 native_fn(const char* name) {
    u32 f = loader::find_symbol(name);
    if (!f) fatal("missing native function %s", name);
    return f;
}

}  // namespace

namespace game_java {

void on_create(Cpu& c) {
    jni::init();
    define_java_methods();

    g_activity_obj = jni::new_object(PKG + "ParsonsLoader");
    g_callbacks = mem::calloc(16, 4);
    g_activity = mem::calloc(1, 48);
    mem::w32(g_activity + 0, g_callbacks);
    mem::w32(g_activity + 4, jni::vm());
    mem::w32(g_activity + 8, jni::env());
    mem::w32(g_activity + 12, g_activity_obj);
    mem::w32(g_activity + 16, mem::strdup(hle::guest_files_dir().c_str()));
    mem::w32(g_activity + 20, mem::strdup(hle::guest_external_dir().c_str()));
    mem::w32(g_activity + 24, 22);  // sdkVersion
    mem::w32(g_activity + 32, mem::calloc(1, 16));  // assetManager
    mem::w32(g_activity + 36, mem::strdup(("/sdcard/Android/obb/" + hle::g_config.package).c_str()));

    if (u32 onload = loader::find_symbol("JNI_OnLoad")) {
        u32 v = c.call(onload, {jni::vm(), 0});
        LOGI("JNI_OnLoad -> 0x%x", v);
    }

    LOGI("ANativeActivity_onCreate");
    c.call(native_fn("ANativeActivity_onCreate"), {g_activity, 0, 0});

    // The rest of ParsonsLoader.onCreate():
    u32 clazz = jni::find_class(PKG + "NativeFunctions");
    u32 pub = c.call(native_fn("Java_com_stainlessgames_carmageddon_NativeFunctions_getPublicBuildDefine"), {jni::env(), clazz});
    u32 market = c.call(native_fn("Java_com_stainlessgames_carmageddon_NativeFunctions_getMarketDefine"), {jni::env(), clazz});
    LOGI("public build define = %u, market define = %d", pub, (int)market);
    // The OBB is "in place", so the game thread is un-paused immediately.
    c.call(native_fn("Java_com_stainlessgames_carmageddon_NativeFunctions_pauseGameThread"), {jni::env(), clazz, 0});
}

void on_start(Cpu& c) {
    LOGI("-> onStart");
    audio::start();  // ParsonsLoader.onStart() -> mFMODAudioDevice.start()
    c.call(callback(0), {g_activity});
}
void on_resume(Cpu& c) { LOGI("-> onResume"); c.call(callback(4), {g_activity}); }
void on_pause(Cpu& c) { LOGI("-> onPause"); c.call(callback(12), {g_activity}); }
void on_stop(Cpu& c) { LOGI("-> onStop"); c.call(callback(16), {g_activity}); }

void on_window_created(Cpu& c) {
    LOGI("-> onNativeWindowCreated");
    c.call(callback(28), {g_activity, android::native_window_handle()});
}
void on_input_queue_created(Cpu& c) {
    LOGI("-> onInputQueueCreated");
    c.call(callback(44), {g_activity, android::input_queue_handle()});
}
void on_focus(Cpu& c, bool focused) {
    LOGI("-> onWindowFocusChanged(%d)", focused);
    c.call(callback(24), {g_activity, focused ? 1u : 0u});
}

}  // namespace game_java
