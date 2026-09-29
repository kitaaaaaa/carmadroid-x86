// Optional PC Carmageddon content. The Android version has no cockpit graphics for the in-car view;
// the PC version's 640x480 cockpit images (DATA/64X48X8) are drawn over it here.
#include "pc_content.h"
#include "pcimport/formats.h"
#include "camera_look.h"
#include "controller.h"
#include "cpu.h"
#include "elf_loader.h"
#include "memory.h"
#include "platform.h"
#include <SDL.h>
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <filesystem>
#include <vector>

namespace pc_content {

std::string g_dir;
std::string g_splat_dir;
bool g_cockpit = true;

namespace {

// libParsons.so v1.8.507 offsets
constexpr u32 kPlayerVehicle = 0x8C1FAC;  // Vehicle* of the local player
constexpr u32 kVehicleName = 4;           // char* car name, e.g. "BLKEAGLE" (same names as the PC files)

// The PC cockpit images are 700x528; the game showed the centre of them on a 640x480 screen.
// We map the full width and the middle 480 lines to the screen (a slight horizontal stretch on 16:9).
constexpr float kImgW = 700, kImgTop = 24, kImgH = 480;
constexpr float kSideViewDeg = 40;  // head turn at which the side cockpit image is shown

constexpr GLenum kClampToEdge = 0x812F, kTexture0 = 0x84C0, kActiveTexture = 0x84E0;
using PFN_ActiveTexture = void(APIENTRY*)(GLenum);
PFN_ActiveTexture pActiveTexture;

u32 g_orig = 0;
u32 g_item_draw_orig = 0;
u32 g_lib = 0;
bool g_dash_active = false;  // the cockpit dashboard is up: hide the HUD's own damage graph and speedo
u32 g_palette[256];
bool g_palette_ok = false;

struct Image {
    GLuint tex = 0;
    int w = 0, h = 0;
};

struct HandFrame {
    int rx = 0, ry = 0, lx = 0, ly = 0;
    std::shared_ptr<Image> right, left;
};

// A speedo or tacho. Digital: a digit strip (speedo, 10 frames) or a bar revealed left to right
// (tacho). Analog: a needle from (cx,cy) swept from angle a0 to a1, with an optional overlay image.
struct Gauge {
    bool present = false, analog = false;
    int x = 0, y = 0, pitch = 0;
    std::shared_ptr<Image> img;
    float cx = 0, cy = 0, r1 = 0, r2 = 0, a0 = 0, a1 = 0, max_value = 0;
    u32 colour = 0xFFFFFFFF;
};

// Damage icon: 10 frames stacked vertically, two per damage level (the second is the flash frame).
struct DamageIcon {
    int x = 0, y = 0;
    float flash_hz[5] = {};
    std::shared_ptr<Image> img;
};

struct Cockpit {
    std::shared_ptr<Image> view[3];  // forward, left, right
    std::vector<HandFrame> hands;
    Gauge speedo, tacho;
    int gear_x = 0, gear_y = 0;
    std::shared_ptr<Image> gears;    // R, N, 1..6 stacked vertically
    std::vector<DamageIcon> damage;  // PC order: engine, transmission, steering, 4 brakes, 4 wheels
};

std::map<std::string, std::shared_ptr<Image>> g_images;
std::map<std::string, std::unique_ptr<Cockpit>> g_cockpits;  // by car name (null = none available)

// A file under DATA: from the base game, else from the Splat Pack.
std::string data_path(const std::string& rel) {
    const std::string base = g_dir + "/DATA/" + rel;
    if (!g_splat_dir.empty() && !std::filesystem::exists(std::filesystem::path(base))) {
        const std::string splat = g_splat_dir + "/DATA/" + rel;
        if (std::filesystem::exists(std::filesystem::path(splat))) return splat;
    }
    return base;
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

// BRender pixelmap file: big-endian chunks; 0x03/0x3D = header, 0x21 = pixels. Returns the first map.
struct Pix {
    int type = 0, row = 0, w = 0, h = 0;
    std::vector<u8> pixels;
};
bool load_pix(const std::string& path, Pix& pix) {
    std::vector<u8> d;
    if (!read_file(path, d)) return false;
    bool header = false;
    for (size_t off = 0; off + 8 <= d.size();) {
        const u32 id = be32(&d[off]), len = be32(&d[off + 4]);
        const u8* body = &d[off + 8];
        if (off + 8 + len > d.size()) break;
        if ((id == 0x03 || id == 0x3D) && len >= 11) {
            pix.type = body[0];
            pix.row = be16(body + 1);
            pix.w = be16(body + 3);
            pix.h = be16(body + 5);
            header = true;
        } else if (id == 0x21 && header && len >= 8) {
            const u32 count = be32(body), size = be32(body + 4);
            if ((u64)count * size > len - 8) return false;
            pix.pixels.assign(body + 8, body + 8 + count * size);
            return true;
        }
        off += 8 + len;
    }
    return false;
}

bool load_palette() {
    Pix pal;
    if (!load_pix(data_path("REG/PALETTES/DRRENDER.PAL"), pal) || pal.pixels.size() < 1024) {
        LOGE("pc: can't read DATA/REG/PALETTES/DRRENDER.PAL");
        return false;
    }
    for (int i = 0; i < 256; i++) {  // entries are x,r,g,b
        const u8* e = &pal.pixels[i * 4];
        g_palette[i] = 0xFF000000u | (u32)e[3] << 16 | (u32)e[2] << 8 | e[1];  // ABGR = RGBA bytes
    }
    g_palette[0] = 0;  // index 0 is transparent
    return true;
}

std::shared_ptr<Image> image(const std::string& name) {
    if (name.empty() || name == "none") return nullptr;
    auto it = g_images.find(name);
    if (it != g_images.end()) return it->second;
    std::shared_ptr<Image> img;
    Pix pix;
    if (load_pix(data_path("64X48X8/PIXELMAP/" + name), pix) && pix.type == 3 &&
        pix.pixels.size() >= (size_t)pix.row * pix.h) {
        std::vector<u32> rgba((size_t)pix.w * pix.h);
        for (int y = 0; y < pix.h; y++)
            for (int x = 0; x < pix.w; x++) rgba[(size_t)y * pix.w + x] = g_palette[pix.pixels[(size_t)y * pix.row + x]];
        img = std::make_shared<Image>();
        img->w = pix.w;
        img->h = pix.h;
        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pix.w, pix.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else {
        LOGE("pc: can't read pixelmap %s", name.c_str());
    }
    g_images[name] = img;
    return img;
}

// Non-empty lines of a PC data text file with // comments removed ('@' lines are encrypted).
std::vector<std::string> text_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    std::string s;
    while (std::getline(f, s)) {
        while (!s.empty() && s.back() == '\r') s.pop_back();
        s = pcimport::c1_decode_line(s);
        if (size_t c = s.find("//"); c != std::string::npos) s.resize(c);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        lines.push_back(s.substr(b));
    }
    return lines;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) {
        size_t b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
        out.push_back(b == std::string::npos ? "" : t.substr(b, e - b + 1));
    }
    return out;
}

// "d,x,y,image,x-pitch" or "a,x,y,image|none,centre-x,centre-y,radius1,radius2,start-angle,end-angle,
// colour[,max-value]" (angles in degrees, anticlockwise from the right; colour is a palette index).
Gauge parse_gauge(const std::string& line) {
    Gauge g;
    const auto f = split(line);
    if (f.size() < 4) return g;
    g.x = std::atoi(f[1].c_str());
    g.y = std::atoi(f[2].c_str());
    g.img = image(f[3]);
    if (f[0] == "d") {
        g.present = g.img != nullptr;
        if (f.size() > 4) g.pitch = std::atoi(f[4].c_str());
    } else if (f[0] == "a" && f.size() >= 11) {
        g.present = g.analog = true;
        g.cx = (float)std::atof(f[4].c_str());
        g.cy = (float)std::atof(f[5].c_str());
        g.r1 = (float)std::atof(f[6].c_str());
        g.r2 = std::fabs((float)std::atof(f[7].c_str()));
        g.a0 = (float)std::atof(f[8].c_str());
        g.a1 = (float)std::atof(f[9].c_str());
        g.colour = g_palette[std::abs(std::atoi(f[10].c_str())) & 255] | 0xFF000000u;
        if (f.size() > 11) g.max_value = (float)std::atof(f[11].c_str());
    }
    return g;
}

// DATA/64X48X8/CARS/<car>.TXT: forward/left/right images (each followed by a rectangle),
// speedo/tacho/gear lines, then the hands frame count and one line per frame.
std::unique_ptr<Cockpit> load_cockpit(const std::string& car) {
    auto lines = text_lines(data_path("64X48X8/CARS/" + car + ".TXT"));
    // Converted cars whose PC name starts with a digit are called Car<name> (CAR333: the PC's 333).
    if (lines.empty() && car.size() > 3 && car.compare(0, 3, "CAR") == 0 && isdigit((unsigned char)car[3]))
        lines = text_lines(data_path("64X48X8/CARS/" + car.substr(3) + ".TXT"));
    if (lines.size() < 7) {
        LOGI("pc: no PC cockpit for car %s", car.c_str());
        return nullptr;
    }
    auto cp = std::make_unique<Cockpit>();
    for (int v = 0; v < 3; v++) cp->view[v] = image(lines[v * 2]);
    if (!cp->view[0]) return nullptr;
    // Speedo, tacho and gear lines come in pairs: external view first, then the cockpit one.
    if (lines.size() > 11) {
        cp->speedo = parse_gauge(lines[7]);
        cp->tacho = parse_gauge(lines[9]);
        const auto g = split(lines[11]);
        if (g.size() >= 3) {
            cp->gear_x = std::atoi(g[0].c_str());
            cp->gear_y = std::atoi(g[1].c_str());
            cp->gears = image(g[2]);
        }
    }
    size_t i = 6;
    while (i < lines.size() && lines[i].find_first_not_of("0123456789") != std::string::npos) i++;
    if (i < lines.size()) {
        const int n = std::atoi(lines[i].c_str());
        const size_t d = i + 1 + n + 2;  // after the hands: mirror and pratcam rectangles, then 11 damage icons
        for (int k = 0; k < 11 && d + k < lines.size(); k++) {
            const auto f = split(lines[d + k]);
            if (f.size() < 8) break;
            DamageIcon icon;
            icon.x = std::atoi(f[0].c_str());
            icon.y = std::atoi(f[1].c_str());
            for (int j = 0; j < 5; j++) icon.flash_hz[j] = (float)std::atof(f[2 + j].c_str());
            icon.img = image(f[7]);
            cp->damage.push_back(icon);
        }
        for (int k = 0; k < n && i + 1 + k < lines.size(); k++) {
            const auto f = split(lines[i + 1 + k]);
            if (f.size() < 6) break;
            HandFrame h;
            h.rx = std::atoi(f[0].c_str());
            h.ry = std::atoi(f[1].c_str());
            h.right = image(f[2]);
            h.lx = std::atoi(f[3].c_str());
            h.ly = std::atoi(f[4].c_str());
            h.left = image(f[5]);
            cp->hands.push_back(h);
        }
    }
    LOGI("pc: loaded cockpit for %s (%zu hands frames)", car.c_str(), cp->hands.size());
    return cp;
}

float g_sx = 1, g_sy = 1;  // cockpit image pixels -> screen pixels

// Draws the part (u,v,w,h) of an image with its top-left at cockpit image position (x,y).
void blit(const Image& img, float x, float y, float u, float v, float w, float h) {
    glBindTexture(GL_TEXTURE_2D, img.tex);
    const float x0 = x * g_sx, y0 = (y - kImgTop) * g_sy, x1 = (x + w) * g_sx, y1 = (y + h - kImgTop) * g_sy;
    const float s0 = u / img.w, t0 = v / img.h, s1 = (u + w) / img.w, t1 = (v + h) / img.h;
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(x0, y0);
    glTexCoord2f(s1, t0); glVertex2f(x1, y0);
    glTexCoord2f(s1, t1); glVertex2f(x1, y1);
    glTexCoord2f(s0, t1); glVertex2f(x0, y1);
    glEnd();
}

void quad(const Image& img, float x, float y) { blit(img, x, y, 0, 0, (float)img.w, (float)img.h); }

void needle(const Gauge& g, float frac) {
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    const float a = (g.a0 + (g.a1 - g.a0) * frac) * 3.14159265f / 180.0f;
    const float dx = std::cos(a), dy = -std::sin(a);
    glDisable(GL_TEXTURE_2D);
    glLineWidth(std::fmax(1.5f * g_sy, 1.0f));
    glColor4ub(g.colour & 0xFF, (g.colour >> 8) & 0xFF, (g.colour >> 16) & 0xFF, 0xFF);
    glBegin(GL_LINES);
    glVertex2f((g.cx + dx * g.r1) * g_sx, (g.cy + dy * g.r1 - kImgTop) * g_sy);
    glVertex2f((g.cx + dx * g.r2) * g_sx, (g.cy + dy * g.r2 - kImgTop) * g_sy);
    glEnd();
    glColor4f(1, 1, 1, 1);
    glEnable(GL_TEXTURE_2D);
}

// Car state read from the Android game (libParsons v1.8.507 Vehicle layout).
struct Instruments {
    int mph = 0, gear = 0;
    float revs_frac = 0;
    float health[11];  // 1 = intact, in the PC order (engine, transmission, steering, brakes, wheels)
};

bool read_instruments(u32 veh, Instruments& in) {
    const u32 a = mem::r32(veh + 0x28);
    if (!mem::valid(a + 0x24, 4)) return false;
    const u32 b = mem::r32(a + 0x24);
    if (!mem::valid(b + 0x4ec, 4)) return false;
    const u32 gearbox = mem::r32(b + 0x4ec);
    if (!mem::valid(gearbox, 0xd0)) return false;
    const float* gb = mem::ptr<float>(gearbox);
    const float revs = gb[0xcc / 4], redline = gb[0x64 / 4];  // Vehicle_GetSubjectiveRevs; SFX_Engine's range
    in.revs_frac = redline > 600 ? (revs - 500) / (redline - 500) : 0;
    in.gear = (int)mem::r32(gearbox + 0xc8);  // GetDisplayGear: <0 reverse, 0 neutral
    in.mph = (int)std::fabs(mem::ptr<float>(veh)[0x68 / 4] * 2);  // GetDisplayMPH
    const u32 health = mem::r32(veh + 0x2c);  // HUD_SendDamageToUI
    static const u32 kHealthOff[11] = {0x08, 0x0c, 0x10, 0x24, 0x28, 0x2c, 0x30, 0x14, 0x18, 0x1c, 0x20};
    for (int i = 0; i < 11; i++)
        in.health[i] = mem::valid(health, 0x34) ? mem::ptr<float>(health)[kHealthOff[i] / 4] : 1.0f;
    return true;
}

void draw_instruments(const Cockpit& cp, const Instruments& in) {
    if (cp.speedo.present) {
        const Gauge& g = cp.speedo;
        if (g.analog) {
            needle(g, in.mph / (g.max_value > 0 ? g.max_value : 200.0f));
            if (g.img) quad(*g.img, (float)g.x, (float)g.y);
        } else {
            const float dh = g.img->h / 10.0f;
            const int v = in.mph > 999 ? 999 : in.mph;
            const int digits[3] = {v / 100, v / 10 % 10, v % 10};
            for (int k = 0; k < 3; k++) {
                if ((k == 0 && v < 100) || (k == 1 && v < 10)) continue;  // no leading zeros
                blit(*g.img, (float)(g.x + k * g.pitch), (float)g.y, 0, digits[k] * dh, (float)g.img->w, dh);
            }
        }
    }
    if (cp.tacho.present) {
        const Gauge& g = cp.tacho;
        const float f = in.revs_frac < 0 ? 0 : in.revs_frac > 1 ? 1 : in.revs_frac;
        if (g.analog) {
            needle(g, f);
            if (g.img) quad(*g.img, (float)g.x, (float)g.y);
        } else if (f > 0) {
            blit(*g.img, (float)g.x, (float)g.y, 0, 0, g.img->w * f, (float)g.img->h);
        }
    }
    if (cp.gears) {  // R, N, 1, 2, ...
        const int frames = cp.gears->h % 28 == 0 ? cp.gears->h / 28 : 8;
        int f = in.gear < 0 ? 0 : in.gear + 1;
        if (f >= frames) f = frames - 1;
        const float fh = (float)cp.gears->h / frames;
        blit(*cp.gears, (float)cp.gear_x, (float)cp.gear_y, 0, f * fh, (float)cp.gears->w, fh);
    }
    // Damage icons: level 0 (intact) .. 4 (wrecked), flashing between the level's two frames.
    const u64 ms = SDL_GetTicks64();
    for (size_t i = 0; i < cp.damage.size() && i < 11; i++) {
        const DamageIcon& d = cp.damage[i];
        if (!d.img) continue;
        // Same as the PC game: 5 levels of 20% damage each, so a part only lights up past 20%.
        int level = (int)((1.0f - in.health[i]) * 5.0f);
        level = level < 0 ? 0 : level > 4 ? 4 : level;
        const float hz = d.flash_hz[level];
        const int flash = hz > 0 ? (int)((ms * hz * 2) / 1000) & 1 : 0;
        const float fh = d.img->h / 10.0f;
        blit(*d.img, (float)d.x, (float)d.y, 0, (level * 2 + flash) * fh, (float)d.img->w, fh);
    }
}

void draw_cockpit() {
    g_dash_active = false;
    if (!platform::in_race() || !camera_look::bonnet_view()) return;
    const u32 veh = mem::r32(g_lib + kPlayerVehicle);
    if (!mem::valid(veh, 8)) return;
    const char* name = mem::str(mem::r32(veh + kVehicleName));
    if (!name || !mem::valid(mem::guest(name), 1)) return;

    int w, h;
    platform::drawable_size(w, h);
    if (w <= 0 || h <= 0) return;

    GLint active = kTexture0;
    glGetIntegerv(kActiveTexture, &active);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (pActiveTexture) {
        pActiveTexture(kTexture0 + 1);
        glDisable(GL_TEXTURE_2D);
        pActiveTexture(kTexture0);
    }

    auto it = g_cockpits.find(name);
    if (it == g_cockpits.end()) it = g_cockpits.emplace(name, load_cockpit(name)).first;
    const Cockpit* cp = it->second.get();
    g_dash_active = cp != nullptr;
    if (cp) {
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_LIGHTING);
        glDisable(GL_FOG);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(1, 1, 1, 1);
        glMatrixMode(GL_TEXTURE);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, w, h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        g_sx = w / kImgW;
        g_sy = h / kImgH;
        const float yaw = camera_look::yaw_deg();
        const bool behind = std::fabs(yaw) > 120;  // looking out of the back: no cockpit
        const int view = yaw <= -kSideViewDeg ? 1 : yaw >= kSideViewDeg ? 2 : 0;
        const Image* img = cp->view[view] ? cp->view[view].get() : cp->view[0].get();
        if (!behind) quad(*img, 0, 0);
        Instruments in;
        if (view == 0 && !behind && read_instruments(veh, in)) draw_instruments(*cp, in);
        if (view == 0 && !behind && !cp->hands.empty()) {
            const int n = (int)cp->hands.size();
            int f = (int)std::lround((controller::steer_input() + 1) * 0.5f * (n - 1));
            f = f < 0 ? 0 : f >= n ? n - 1 : f;
            const HandFrame& hf = cp->hands[f];
            if (hf.right) quad(*hf.right, (float)hf.rx, (float)hf.ry);
            if (hf.left) quad(*hf.left, (float)hf.lx, (float)hf.ly);
        }

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_TEXTURE);
        glPopMatrix();
    }
    glPopAttrib();
    if (pActiveTexture) pActiveTexture(active);
}

// HUD (Lube) items to hide while the dashboard is up: the damage graph (lube.item.hud_damage), the
// speed and gear texts (watching hud_mph / hud_gear) and the frame drawn just before each of them.
// CLubeMenuItem: +28 class name, +36 watched property (both char*).
std::map<u32, u32> g_hidden_frames;  // frame item -> its class name pointer when found
u32 g_last_frame = 0, g_damage_item = 0;

bool hide_hud_item(u32 item) {
    const u32 cls_ptr = mem::r32(item + 28);
    const char* cls = mem::valid(cls_ptr, 32) ? mem::ptr<const char>(cls_ptr) : nullptr;
    if (!cls) return false;
    const u32 prop_ptr = mem::r32(item + 36);
    const char* prop = mem::valid(prop_ptr, 16) ? mem::ptr<const char>(prop_ptr) : "";
    if (!strcmp(cls, "lube.item.simple_frame")) {
        g_last_frame = item;
        auto f = g_hidden_frames.find(item);
        return f != g_hidden_frames.end() && f->second == cls_ptr;
    }
    const bool damage = !strcmp(cls, "lube.item.hud_damage");
    const bool speed = !strcmp(cls, "lube.item.text") && (!strcmp(prop, "hud_mph") || !strcmp(prop, "hud_gear"));
    if (damage && item != g_damage_item) {  // new HUD (new race): forget the old frames
        g_damage_item = item;
        g_hidden_frames.clear();
    }
    if ((damage || (speed && !strcmp(prop, "hud_mph"))) && g_last_frame)
        g_hidden_frames[g_last_frame] = mem::r32(g_last_frame + 28);
    return damage || speed;
}

}  // namespace

void apply_patches() {
    if (g_dir.empty() || !g_cockpit) return;
    for (Module* m : loader::modules())
        if (m->name == "libParsons.so") g_lib = m->base;
    if (!g_lib || !load_palette()) return;
    const u32 fn = loader::find_symbol("_Z8PDDraw2Db");
    if (!fn) { LOGE("pc: PDDraw2D not found"); return; }
    pActiveTexture = (PFN_ActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
    g_orig = hle::hook_function(fn, "PDDraw2D", [](Cpu& c) {
        const u32 arg = c.r(0);
        draw_cockpit();
        c.call(g_orig, {arg});
    });
    if (const u32 draw = loader::find_symbol("_ZN13CLubeMenuItem4drawERK12CUITransform")) {
        g_item_draw_orig = hle::hook_function(draw, "CLubeMenuItem::draw", [](Cpu& c) {
            const u32 item = c.r(0);
            if (hide_hud_item(item) && g_dash_active) return;
            c.call(g_item_draw_orig, {item, c.r(1)});
        });
    }
    LOGI("pc: PC content from %s (cockpit overlay on)", g_dir.c_str());
}

}  // namespace pc_content
