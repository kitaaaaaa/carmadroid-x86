// EGL 1.4 on top of the SDL window + GL context.
#include "../platform.h"
#include "../debug_tools.h"
#include "../controller.h"
#include "../content_patches.h"
#include "android.h"
#include "hle_common.h"

namespace {
enum : u32 {
    EGL_SUCCESS = 0x3000,
    EGL_BUFFER_SIZE = 0x3020, EGL_ALPHA_SIZE = 0x3021, EGL_BLUE_SIZE = 0x3022, EGL_GREEN_SIZE = 0x3023,
    EGL_RED_SIZE = 0x3024, EGL_DEPTH_SIZE = 0x3025, EGL_STENCIL_SIZE = 0x3026, EGL_CONFIG_ID = 0x3028,
    EGL_NATIVE_VISUAL_ID = 0x302E, EGL_SAMPLES = 0x3031, EGL_SURFACE_TYPE = 0x3033,
    EGL_HEIGHT = 0x3056, EGL_WIDTH = 0x3057, EGL_RENDERABLE_TYPE = 0x3040,
    EGL_VENDOR = 0x3053, EGL_VERSION = 0x3054, EGL_EXTENSIONS = 0x3055, EGL_CLIENT_APIS = 0x308D,
};
constexpr u32 kDisplay = 1, kConfig = 1, kSurface = 1, kContext = 1;
}  // namespace

HLE(eglGetDisplay) { c.ret(kDisplay); }
HLE(eglInitialize) {
    if (c.r(1)) mem::w32(c.r(1), 1);
    if (c.r(2)) mem::w32(c.r(2), 4);
    c.ret(1);
}
HLE(eglTerminate) { c.ret(1); }
HLE(eglGetError) { c.ret(EGL_SUCCESS); }
HLE(eglChooseConfig) {
    // (dpy, attrib_list, configs, config_size, num_config)
    u32 attrs = c.r(1);
    for (u32 a = attrs; a && mem::r32(a) != 0x3038; a += 8) LOGV("  eglChooseConfig attr 0x%x = %d", mem::r32(a), (int)mem::r32(a + 4));
    if (c.r(2) && c.r(3) > 0) mem::w32(c.r(2), kConfig);
    mem::w32(c.arg(4), 1);
    c.ret(1);
}
HLE(eglGetConfigs) {
    if (c.r(1) && c.r(2) > 0) mem::w32(c.r(1), kConfig);
    mem::w32(c.r(3), 1);
    c.ret(1);
}
HLE(eglGetConfigAttrib) {
    u32 v = 0;
    switch (c.r(2)) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: v = 8; break;
        case EGL_BUFFER_SIZE: v = 32; break;
        case EGL_DEPTH_SIZE: v = 24; break;
        case EGL_STENCIL_SIZE: v = 8; break;
        case EGL_CONFIG_ID: v = kConfig; break;
        case EGL_NATIVE_VISUAL_ID: v = 1; break;  // WINDOW_FORMAT_RGBA_8888
        case EGL_SURFACE_TYPE: v = 4; break;      // EGL_WINDOW_BIT
        case EGL_RENDERABLE_TYPE: v = 1; break;   // EGL_OPENGL_ES_BIT
        case EGL_SAMPLES: v = 0; break;
        default: LOGV("eglGetConfigAttrib 0x%x", c.r(2));
    }
    mem::w32(c.r(3), v);
    c.ret(1);
}
HLE(eglCreateWindowSurface) { c.ret(kSurface); }
HLE(eglDestroySurface) { c.ret(1); }
HLE(eglCreateContext) { c.ret(kContext); }
HLE(eglDestroyContext) { c.ret(1); }
HLE(eglMakeCurrent) {
    // (dpy, draw, read, ctx)
    platform::make_current(c.r(3) != 0);
    LOGI("eglMakeCurrent(ctx=%u) on thread %u", c.r(3), c.thread_id);
    c.ret(1);
}
HLE(eglQuerySurface) {
    int w, h;
    platform::drawable_size(w, h);
    u32 v = 0;
    if (c.r(2) == EGL_WIDTH) v = (u32)w;
    else if (c.r(2) == EGL_HEIGHT) v = (u32)h;
    else LOGV("eglQuerySurface 0x%x", c.r(2));
    mem::w32(c.r(3), v);
    c.ret(1);
}
HLE(eglSwapBuffers) {
    debug::on_frame(c);
    controller::on_game_frame(c);
    content::on_game_frame();
    platform::swap();
    c.ret(1);
}
HLE(eglSwapInterval) { c.ret(1); }
HLE(eglQueryString) {
    static u32 vendor = mem::strdup("carmadroid"), version = mem::strdup("1.4 carmadroid"), empty = mem::strdup(""),
               apis = mem::strdup("OpenGL_ES");
    switch (c.r(1)) {
        case EGL_VENDOR: c.ret(vendor); break;
        case EGL_VERSION: c.ret(version); break;
        case EGL_CLIENT_APIS: c.ret(apis); break;
        default: c.ret(empty); break;
    }
}
HLE(eglGetProcAddress) {
    std::string n = mem::str(c.r(0));
    std::string base = n;
    if (base.size() > 3 && base.compare(base.size() - 3, 3, "OES") == 0) base.resize(base.size() - 3);
    u32 v = 0;
    if (hle::is_implemented(n)) v = hle::thunk_for(n);
    else if (hle::is_implemented(base)) v = hle::thunk_for(base);
    LOGI("eglGetProcAddress(%s) -> 0x%x", n.c_str(), v);
    c.ret(v);
}
