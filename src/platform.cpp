#include "platform.h"
#include "hle/android.h"
#include "controller.h"
#include <algorithm>
#include <SDL.h>
#include <windows.h>
#include <GL/gl.h>
#include <atomic>
#include <string>
#include <vector>
#include <thread>

namespace platform {
namespace {
SDL_Window* g_window = nullptr;
SDL_GLContext g_ctx = nullptr;
bool g_mouse_down = false;

// The game renders into an off-screen framebuffer of fixed size (g_rw x g_rh, at most 1920x1080 by
// default). At swap time it is scaled into the window with letterboxing, so the window can be any
// size (and toggled fullscreen) without the game ever seeing a surface resize.
int g_rw = 1920, g_rh = 1080;

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
using PFNGenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFNBindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using PFNGenRenderbuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFNBindRenderbuffer = void(APIENTRY*)(GLenum, GLuint);
using PFNRenderbufferStorage = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
using PFNFramebufferRenderbuffer = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using PFNCheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using PFNBlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
PFNGenFramebuffers pGenFramebuffers;
PFNBindFramebuffer pBindFramebuffer;
PFNGenRenderbuffers pGenRenderbuffers;
PFNBindRenderbuffer pBindRenderbuffer;
PFNRenderbufferStorage pRenderbufferStorage;
PFNFramebufferRenderbuffer pFramebufferRenderbuffer;
PFNCheckFramebufferStatus pCheckFramebufferStatus;
PFNBlitFramebuffer pBlitFramebuffer;
GLuint g_fbo = 0;

// Where the render image lands in the window's drawable (letterboxed, aspect preserved).
void present_rect(int& x, int& y, int& w, int& h) {
    int dw, dh;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    float scale = std::min(dw / (float)g_rw, dh / (float)g_rh);
    w = std::max(1, (int)(g_rw * scale + 0.5f));
    h = std::max(1, (int)(g_rh * scale + 0.5f));
    x = (dw - w) / 2;
    y = (dh - h) / 2;
}

// Window (mouse) coordinates -> game render coordinates.
void to_drawable(int x, int y, float& ox, float& oy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    float px = ww ? x * (float)dw / ww : (float)x;
    float py = wh ? y * (float)dh / wh : (float)y;
    int rx, ry, rw, rh;
    present_rect(rx, ry, rw, rh);
    ox = std::clamp((px - rx) * g_rw / (float)rw, 0.0f, g_rw - 1.0f);
    oy = std::clamp((py - ry) * g_rh / (float)rh, 0.0f, g_rh - 1.0f);
}

void create_fbo() {
    pGenFramebuffers = (PFNGenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    pBindFramebuffer = (PFNBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    pGenRenderbuffers = (PFNGenRenderbuffers)SDL_GL_GetProcAddress("glGenRenderbuffers");
    pBindRenderbuffer = (PFNBindRenderbuffer)SDL_GL_GetProcAddress("glBindRenderbuffer");
    pRenderbufferStorage = (PFNRenderbufferStorage)SDL_GL_GetProcAddress("glRenderbufferStorage");
    pFramebufferRenderbuffer = (PFNFramebufferRenderbuffer)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    pCheckFramebufferStatus = (PFNCheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    pBlitFramebuffer = (PFNBlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!pGenFramebuffers || !pBlitFramebuffer) fatal("OpenGL framebuffer objects are not available");
    GLuint rb[2];
    pGenRenderbuffers(2, rb);
    pBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    pRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_rw, g_rh);
    pBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    pRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_rw, g_rh);
    pGenFramebuffers(1, &g_fbo);
    pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    if (pCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("render framebuffer incomplete");
    LOGI("rendering at %dx%d (off-screen, scaled to the window)", g_rw, g_rh);
}

void toggle_fullscreen() {
    bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

// Active touch pointers (mouse = id 0, controller virtual fingers = 100+), in Android index order.
struct Finger { int id; float x, y; };
std::vector<Finger> g_fingers;

void emit(int action) {
    android::InputEvent e{};
    e.type = 2;         // AINPUT_EVENT_TYPE_MOTION
    e.source = 0x1002;  // AINPUT_SOURCE_TOUCHSCREEN
    e.action = action;
    e.pointer_count = (s32)std::min<size_t>(g_fingers.size(), 10);
    for (int i = 0; i < e.pointer_count; i++) e.pointers[i] = {g_fingers[i].id, g_fingers[i].x, g_fingers[i].y};
    android::push_input(e);
}

int finger_index(int id) {
    for (size_t i = 0; i < g_fingers.size(); i++)
        if (g_fingers[i].id == id) return (int)i;
    return -1;
}

void key_event(int keycode, bool down) {
    android::InputEvent e{};
    e.type = 1;          // AINPUT_EVENT_TYPE_KEY
    e.source = 0x101;    // AINPUT_SOURCE_KEYBOARD
    e.action = down ? 0 : 1;
    e.keycode = keycode;
    android::push_input(e);
}

int android_keycode(SDL_Keycode k) {
    switch (k) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return 4;  // BACK
        case SDLK_UP: return 19;
        case SDLK_DOWN: return 20;
        case SDLK_LEFT: return 21;
        case SDLK_RIGHT: return 22;
        case SDLK_RETURN: return 66;  // ENTER
        case SDLK_SPACE: return 62;
        case SDLK_m: return 82;       // MENU
        default: return 0;
    }
}
}  // namespace

bool init(const Options& opt) {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return false;
    }
    SDL_DisplayMode dm{};
    if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 1920; dm.h = 1080; }

    // Render resolution: the desktop's aspect ratio, capped to max_w x max_h.
    if (opt.render_w > 0 && opt.render_h > 0) {
        g_rw = opt.render_w;
        g_rh = opt.render_h;
    } else {
        double aspect = dm.w / (double)dm.h;
        g_rw = std::min(dm.w, opt.max_w);
        g_rh = (int)(g_rw / aspect + 0.5);
        if (g_rh > opt.max_h) { g_rh = std::min(dm.h, opt.max_h); g_rw = (int)(g_rh * aspect + 0.5); }
        g_rw &= ~1;
        g_rh &= ~1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (opt.hidden) flags |= SDL_WINDOW_HIDDEN;
    // Windowed: the render size if it fits, else 85% of the desktop with the same aspect ratio.
    int ww = g_rw, wh = g_rh;
    if (ww > dm.w * 0.85 || wh > dm.h * 0.85) {
        double k = std::min(dm.w * 0.85 / ww, dm.h * 0.85 / wh);
        ww = (int)(ww * k);
        wh = (int)(wh * k);
    }
    g_window = SDL_CreateWindow("Carmageddon (carmadroid)  -  F11: fullscreen", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, ww, wh, flags);
    if (!g_window) { LOGE("SDL_CreateWindow: %s", SDL_GetError()); return false; }
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) { LOGE("SDL_GL_CreateContext: %s", SDL_GetError()); return false; }
    SDL_GL_SetSwapInterval(opt.vsync ? 1 : 0);
    LOGI("OpenGL: %s / %s, vsync %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION),
         opt.vsync ? "on" : "off (unlocked frame rate)");
    SDL_GL_MakeCurrent(g_window, nullptr);  // the game's render thread takes it via eglMakeCurrent
    android::set_window_size(g_rw, g_rh);
    controller::init();
    return true;
}

void make_current(bool current) {
    if (SDL_GL_MakeCurrent(g_window, current ? g_ctx : nullptr) != 0) LOGE("SDL_GL_MakeCurrent: %s", SDL_GetError());
    if (current) {
        if (!g_fbo) create_fbo();
        pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    }
}

int g_shot_every = 0;
std::string g_shot_dir;
static int g_frame = 0;

static void save_screenshot() {
    int w, h;
    drawable_size(w, h);
    std::vector<unsigned char> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    char name[512];
    snprintf(name, sizeof name, "%s/frame_%06d.bmp", g_shot_dir.c_str(), g_frame);
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
    LOGI("saved %s", name);
}

int g_menu_fps = 120;
int g_race_fps = 0;
static std::atomic<u64> g_last_race_frame_ms{0};

static u64 now_ms() { return SDL_GetTicks64(); }

void note_race_frame() { g_last_race_frame_ms = now_ms(); }

static void limit_frame_rate() {
    static u64 next_us = 0;
    const bool racing = now_ms() - g_last_race_frame_ms < 250;
    const int cap = racing ? g_race_fps : g_menu_fps;
    const u64 freq = SDL_GetPerformanceFrequency();
    auto now_us = [&] { return SDL_GetPerformanceCounter() * 1000000 / freq; };
    if (cap <= 0) { next_us = 0; return; }
    const u64 period = 1000000 / cap;
    u64 t = now_us();
    if (next_us == 0 || t > next_us + period * 4) next_us = t;  // resync after stalls
    while ((t = now_us()) < next_us) {
        u64 left = next_us - t;
        if (left > 2000) SDL_Delay((Uint32)((left - 1000) / 1000));
        else std::this_thread::yield();
    }
    next_us += period;
}

void swap() {
    g_frame++;
    if (g_shot_every > 0 && g_frame % g_shot_every == 0) save_screenshot();
    limit_frame_rate();

    // Present: scale the game's framebuffer into the window. Save the GL state that affects
    // clears/blits so the game's own state is untouched.
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);

    int x, y, w, h, dw, dh;
    present_rect(x, y, w, h);
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    pBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    pBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pBlitFramebuffer(0, 0, g_rw, g_rh, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT,
                     (w == g_rw && h == g_rh) ? GL_NEAREST : GL_LINEAR);
    SDL_GL_SwapWindow(g_window);

    pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void drawable_size(int& w, int& h) {
    w = g_rw;
    h = g_rh;
}

void* gl_proc(const char* name) { return SDL_GL_GetProcAddress(name); }

void inject_tap(float x, float y) {
    touch_down(0, x, y);
    SDL_Delay(80);
    touch_up(0);
    LOGI("injected tap at %.0f,%.0f", x, y);
}

void touch_down(int id, float x, float y) {
    if (finger_index(id) >= 0) return;
    g_fingers.push_back({id, x, y});
    int idx = (int)g_fingers.size() - 1;
    emit(idx == 0 ? 0 /* DOWN */ : (5 /* POINTER_DOWN */ | (idx << 8)));
}

void touch_move(int id, float x, float y) {
    int idx = finger_index(id);
    if (idx < 0) return;
    g_fingers[idx].x = x;
    g_fingers[idx].y = y;
    emit(2 /* MOVE */);
}

void touch_up(int id) {
    int idx = finger_index(id);
    if (idx < 0) return;
    emit(g_fingers.size() == 1 ? 1 /* UP */ : (6 /* POINTER_UP */ | (idx << 8)));
    g_fingers.erase(g_fingers.begin() + idx);
}

void send_key(int android_keycode, bool down) { key_event(android_keycode, down); }

void inject_key(int k) {
    key_event(k, true);
    SDL_Delay(80);
    key_event(k, false);
    LOGI("injected key %d", k);
}

bool pump_events() {
    SDL_Event ev;
    while (SDL_WaitEventTimeout(&ev, 5)) {
        switch (ev.type) {
            case SDL_QUIT: return false;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                controller::handle_event(ev);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    g_mouse_down = true;
                    float x, y;
                    to_drawable(ev.button.x, ev.button.y, x, y);
                    touch_down(0, x, y);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && g_mouse_down) { g_mouse_down = false; touch_up(0); }
                break;
            case SDL_MOUSEMOTION:
                if (g_mouse_down) {
                    float x, y;
                    to_drawable(ev.motion.x, ev.motion.y, x, y);
                    touch_move(0, x, y);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                    (ev.key.keysym.sym == SDLK_F11 ||
                     (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
                    toggle_fullscreen();
                    break;
                }
                if (!ev.key.repeat) {
                    if (int k = android_keycode(ev.key.keysym.sym)) key_event(k, ev.type == SDL_KEYDOWN);
                }
                break;
        }
        if (!SDL_PollEvent(nullptr)) break;
    }
    controller::update();
    return true;
}

}  // namespace platform
