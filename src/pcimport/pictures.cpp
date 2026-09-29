// Menu pictures, driver portraits and damage HUD silhouettes for converted cars
// (port of tools/pc2android/uiimg.py and damagehud.py).
#include "pcimport/cars.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace pcimport {
namespace {

constexpr double kPi = 3.14159265358979323846;

int pmod(long long a, int m) { return (int)(((a % m) + m) % m); }  // Python-style modulo

// Texture colour at (u, v), or false for a see-through texel.
bool sample(const Rgba& t, double u, double v, u8 out[3]) {
    const size_t ti = ((size_t)pmod((long long)(v * t.h), t.h) * t.w + pmod((long long)(u * t.w), t.w)) * 4;
    if (t.px[ti + 3] < 128) return false;
    out[0] = t.px[ti], out[1] = t.px[ti + 1], out[2] = t.px[ti + 2];
    return true;
}

// Screen-space triangle rasteriser shared by the renderers. a/b/c: (x, y, depth). keep(depth, index)
// decides visibility (and updates the depth buffer); shade(i, w0, w1, w2) colours the pixel.
template <class Keep, class Shade>
void raster(int W, int H, const V3& a, const V3& b, const V3& c, Keep keep, Shade shade) {
    const double area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (std::fabs(area) < 1e-9) return;
    const int x0 = std::max(0, (int)std::min({a[0], b[0], c[0]})), x1 = std::min(W - 1, (int)std::max({a[0], b[0], c[0]}) + 1);
    const int y0 = std::max(0, (int)std::min({a[1], b[1], c[1]})), y1 = std::min(H - 1, (int)std::max({a[1], b[1], c[1]}) + 1);
    for (int y = y0; y <= y1; y++) {
        const double py = y + 0.5;
        for (int x = x0; x <= x1; x++) {
            const double px = x + 0.5;
            const double w0 = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / area;
            const double w1 = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / area;
            const double w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const size_t i = (size_t)y * W + x;
            const double z = w0 * a[2] + w1 * b[2] + w2 * c[2];
            if (!keep(z, i)) continue;
            shade(i, w0, w1, w2, z);
        }
    }
}

V3 face_normal(const std::array<V3, 3>& ps) {
    const double ux = ps[1][0] - ps[0][0], uy = ps[1][1] - ps[0][1], uz = ps[1][2] - ps[0][2];
    const double vx = ps[2][0] - ps[0][0], vy = ps[2][1] - ps[0][1], vz = ps[2][2] - ps[0][2];
    return {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
}

// Draws a car triangle: textured (or grey) and shaded, with the depth test given by `closer`.
template <class Closer, class Colour>
void draw_tri(Rgba& img, std::vector<double>& zbuf, const CarTri& t, const std::array<V3, 3>& scr, Closer closer,
              Colour colour) {
    raster(img.w, img.h, scr[0], scr[1], scr[2],
           [&](double z, size_t i) { return closer(z, zbuf[i]); },
           [&](size_t i, double w0, double w1, double w2, double z) {
               u8 rgb[3] = {160, 160, 160};
               bool textured = false;
               if (t.tex) {
                   const double u = w0 * t.uv[0][0] + w1 * t.uv[1][0] + w2 * t.uv[2][0];
                   const double v = w0 * t.uv[0][1] + w1 * t.uv[1][1] + w2 * t.uv[2][1];
                   if (!sample(*t.tex, u, v, rgb)) return;
                   textured = true;
               }
               zbuf[i] = z;
               colour(&img.px[i * 4], rgb, textured);
           });
}

// RGBA picture of the car in a 3/4 view on a transparent background, with a soft shadow.
Rgba render_car(const CarModel& car, int width, int height, double yaw_deg = -35, double pitch_deg = -28,
                double fill = 0.66, int ss = 2) {
    const int W = width * ss, H = height * ss;
    const double yaw = yaw_deg * kPi / 180, pitch = pitch_deg * kPi / 180;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // Android vehicles are left-handed (front +z); flip z to view them in a right-handed frame.
    auto view = [&](const V3& p) {
        double x = p[0], y = p[1], z = -p[2];
        const double x2 = x * cy - z * sy, z2 = x * sy + z * cy;
        const double y3 = y * cp - z2 * sp, z3 = y * sp + z2 * cp;
        return V3{x2, y3, z3};
    };
    std::vector<std::array<V3, 3>> vt;
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30, ground = 1e30;
    for (const auto& t : car.tris) {
        std::array<V3, 3> v;
        for (int k = 0; k < 3; k++) {
            v[k] = view(t.p[k]);
            xmin = std::min(xmin, v[k][0]), xmax = std::max(xmax, v[k][0]);
            ymin = std::min(ymin, v[k][1]), ymax = std::max(ymax, v[k][1]);
            ground = std::min(ground, t.p[k][1]);
        }
        vt.push_back(v);
    }
    const double span = std::max(xmax - xmin, (ymax - ymin) * W / H);
    const double scale = W * fill / span, cx = (xmax + xmin) / 2, cyy = (ymax + ymin) / 2;
    auto screen = [&](const V3& p) { return V3{W / 2.0 + (p[0] - cx) * scale, H * 0.55 - (p[1] - cyy) * scale, p[2]}; };

    Rgba img(W, H);
    std::vector<double> zbuf((size_t)W * H, 1e30);
    // soft shadow: the car's ground-plane bounding box, projected, blurred ellipse
    double sx0 = 1e30, sx1 = -1e30, sy0 = 1e30, sy1 = -1e30;
    for (const auto& t : car.tris)
        for (const auto& p : t.p) {
            const V3 s = screen(view({p[0], ground, p[2]}));
            sx0 = std::min(sx0, s[0]), sx1 = std::max(sx1, s[0]), sy0 = std::min(sy0, s[1]), sy1 = std::max(sy1, s[1]);
        }
    const double ecx = (sx0 + sx1) / 2, ecy = (sy0 + sy1) / 2, erx = (sx1 - sx0) / 2 * 1.05, ery = (sy1 - sy0) / 2 * 1.1;
    for (int y = std::max(0, (int)(ecy - ery)); y < std::min(H, (int)(ecy + ery) + 1); y++)
        for (int x = std::max(0, (int)(ecx - erx)); x < std::min(W, (int)(ecx + erx) + 1); x++) {
            const double d = ((x - ecx) / erx) * ((x - ecx) / erx) + ((y - ecy) / ery) * ((y - ecy) / ery);
            if (d < 1) img.px[((size_t)y * W + x) * 4 + 3] = (u8)(int)(110 * std::pow(1 - d, 1.5));
        }
    V3 light{-0.45, 0.8, 0.4};
    const double ln = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    for (double& c : light) c /= ln;
    for (size_t ti = 0; ti < vt.size(); ti++) {
        const auto& ps = vt[ti];
        V3 n = face_normal(ps);
        const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (n[2] > 0) n = {-n[0], -n[1], -n[2]};  // winding independent
        const double shade = 0.72 + 0.55 * std::max(0.0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / (nl ? nl : 1));
        draw_tri(img, zbuf, car.tris[ti], {screen(ps[0]), screen(ps[1]), screen(ps[2])},
                 [](double z, double old) { return z < old; },
                 [&](u8* o, const u8* rgb, bool) {
                     for (int k = 0; k < 3; k++) o[k] = (u8)std::min(255, (int)(rgb[k] * shade));
                     o[3] = 255;
                 });
    }
    return resize(img, width, height);
}

// Top view in colour at a fixed scale (pixels per metre) so cars keep their relative sizes, as on the
// pre-race grid screen; rotated 180 degrees like the game's own grid pictures.
Rgba render_top(const CarModel& car, int width, int height, double px_per_m, int ss = 2) {
    const int W = width * ss, H = height * ss;
    double s = px_per_m * ss;
    double xmin = 1e30, xmax = -1e30, zmin = 1e30, zmax = -1e30;
    for (const auto& t : car.tris)
        for (const auto& p : t.p)
            xmin = std::min(xmin, p[0]), xmax = std::max(xmax, p[0]), zmin = std::min(zmin, p[2]), zmax = std::max(zmax, p[2]);
    const double cx = (xmin + xmax) / 2, cz = (zmin + zmax) / 2;
    s = std::min({s, 0.97 * W / (zmax - zmin), 0.97 * H / (xmax - xmin)});  // shrink to fit a big car
    Rgba img(W, H);
    std::vector<double> zbuf((size_t)W * H, -1e30);
    const V3 light{-0.3, 0.9, 0.3};
    // front (+z) to the right, the car's left side (-x) at the top
    auto proj = [&](const V3& p) { return V3{W / 2.0 + (p[2] - cz) * s, H / 2.0 + (p[0] - cx) * s, p[1]}; };
    for (const auto& t : car.tris) {
        V3 n = face_normal(t.p);
        const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (n[1] < 0) n = {-n[0], -n[1], -n[2]};
        const double shade = 0.7 + 0.45 * std::max(0.0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / (nl ? nl : 1));
        draw_tri(img, zbuf, t, {proj(t.p[0]), proj(t.p[1]), proj(t.p[2])},
                 [](double z, double old) { return z > old; },
                 [&](u8* o, const u8* rgb, bool) {
                     for (int k = 0; k < 3; k++) o[k] = (u8)std::min(255, (int)(rgb[k] * shade));
                     o[3] = 255;
                 });
    }
    Rgba out = resize(img, width, height);
    Rgba rot(width, height);
    const size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) memcpy(&rot.px[i * 4], &out.px[(n - 1 - i) * 4], 4);
    return rot;
}

// Square driver portrait from the first frame of a PC mugshot animation (the name label and the frame
// border are cropped off).
Rgba portrait(const fs::path& fli, int size) {
    const FliFrame f = fli_first_frame(fli);
    const int side = std::min(f.w - 8, f.h - 30);
    if (side <= 0) throw std::runtime_error("mugshot too small");
    const int x0 = (f.w - side) / 2, y0 = std::max(4, (f.h - 22 - side) / 2 + 2);
    Rgba img(side, side);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            const RGB& c = f.pal[f.px[(size_t)(y0 + y) * f.w + x0 + x]];
            u8* o = &img.px[((size_t)y * side + x) * 4];
            o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = 255;
        }
    return resize(img, size, size);
}

// Picture files per car, under DATA/CONTENT/UI/ASSETS
struct Pic { const char* dir; int w, h; };
const Pic kCarPictures[] = {{"PPI_HIGH/THUMBS", 240, 240}, {"PPI_LOW/THUMBS", 160, 160}};
// Pre-race grid pictures: top views at a common scale (measured from the original cars: ~29 px/m at 300 wide)
const Pic kGridPictures[] = {{"H720/GRID", 300, 150}, {"H600/GRID", 250, 125}, {"H480/GRID", 200, 100}};
constexpr double kGridPxPerM300 = 29.0;
const Pic kDriverPictures[] = {{"PPI_HIGH/DRIVERS", 138, 138}, {"PPI_LOW/DRIVERS", 92, 92}};

// ---- damage HUD -------------------------------------------------------------------------------
// The HUD's damage display (UI/LAYOUT/<lang>/<type>/HUD_DAMAGE/<CAR>_LAYOUT.LOL) draws the image
// 'damage\<Car>' (UI/ASSETS/PPI_HIGH/DAMAGE/<CAR>.IMG, 128x256, front at the top) centred, and part
// indicators on top at fixed offsets: wheels ('_10x20' and similar), '_susp', '_steer', '_strut',
// '_engine'. A new car's layout is copied from a template car; its positions are moved to the new car's.
constexpr int kHudW = 128, kHudH = 256;

struct Part {
    bool named = false;
    std::string name;
    double x, y;
};

// Lua 5.1 instruction fields
u32 op_of(u32 ins) { return ins & 0x3f; }
u32 b_of(u32 ins) { return (ins >> 23) & 0x1ff; }
u32 c_of(u32 ins) { return (ins >> 14) & 0x1ff; }
u32 ins_at(const Bytes& d, size_t at) { u32 v; memcpy(&v, &d[at], 4); return v; }

std::vector<Part> layout_parts(const Bytes& d) {
    const auto funcs = lua_load(d);
    const LuaFunc& f = funcs[0];
    const auto& K = f.k;
    std::vector<Part> parts;
    bool have_name = false;
    std::string name;
    std::map<std::string, double> cur;
    for (u32 i = 0; i < f.ncode; i++) {
        const u32 ins = ins_at(d, f.code_at + 4 * i), op = op_of(ins), b = b_of(ins), c = c_of(ins);
        if (op == 5) {  // GETGLOBAL
            const u32 bx = ins >> 14;
            have_name = bx < K.size() && K[bx].type == LuaConst::Str && !K[bx].str.empty();
            name = have_name ? K[bx].str : "";
            cur.clear();
        } else if (op == 9 && b >= 256 && c >= 256 && b - 256 < K.size() && c - 256 < K.size()) {  // SETTABLE r[K] = K
            if (K[b - 256].type == LuaConst::Str && K[c - 256].type == LuaConst::Num) cur[K[b - 256].str] = K[c - 256].num;
        } else if (op == 28 && have_name && cur.count("x") && cur.count("y")) {  // CALL: end of an item
            parts.push_back({true, name, cur["x"], cur["y"]});
            have_name = false;
        }
    }
    return parts;
}

bool is_wheel_part(const std::string& n) {
    static const std::regex re("_\\d+x\\d+");
    return std::regex_match(n, re);
}

// (front y, rear y, front half-track, rear half-track) of the wheel indicators, in HUD pixels
// relative to the image centre.
std::array<double, 4> wheel_spots(const std::vector<Part>& parts) {
    std::vector<std::pair<double, double>> front, rear;
    int wheels = 0;
    for (const auto& p : parts)
        if (is_wheel_part(p.name)) {
            wheels++;
            (p.y < 0 ? front : rear).push_back({p.x, p.y});
        }
    if (wheels < 4 || front.empty() || rear.empty()) return {-60, 60, 34, 36};
    double fy = 0, ry = 0, ft = 0, rt = 0;
    for (auto& w : front) fy += w.second, ft += std::fabs(w.first);
    for (auto& w : rear) ry += w.second, rt += std::fabs(w.first);
    return {fy / front.size(), ry / rear.size(), ft / front.size(), rt / rear.size()};
}

struct Fit {
    double sx, sz, cx, cz;
    // Car position (front is +z) -> HUD offset from the image centre (front at the top: -y).
    std::pair<double, double> hud(double x, double z) const { return {(x - cx) * sx, -(z - cz) * sz}; }
};

// Stretch the car to fill the 128x256 frame: the HUD picture is not meant to keep its proportions.
Fit fit(const CarModel& car) {
    double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (const auto& t : car.tris)
        for (const auto& p : t.p) x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), z0 = std::min(z0, p[2]), z1 = std::max(z1, p[2]);
    return {0.92 * kHudW / (x1 - x0), 0.95 * kHudH / (z1 - z0), (x0 + x1) / 2, (z0 + z1) / 2};
}

// Grey top view of the car, front up, fitted to the frame.
Rgba render_silhouette(const CarModel& car) {
    const Fit f = fit(car);
    constexpr int SS = 2;
    Rgba img(kHudW * SS, kHudH * SS);
    std::vector<double> zbuf((size_t)img.w * img.h, -1e30);
    auto proj = [&](const V3& p) {
        const auto h = f.hud(p[0], p[2]);
        return V3{(kHudW / 2.0 + h.first) * SS, (kHudH / 2.0 + h.second) * SS, p[1]};
    };
    const V3 light{0.3, 0.9, 0.35};
    for (const auto& t : car.tris) {
        V3 n = face_normal(t.p);
        const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (n[1] < 0) n = {-n[0], -n[1], -n[2]};
        const double shade = 0.45 + 0.6 * std::max(0.0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / (nl ? nl : 1));
        draw_tri(img, zbuf, t, {proj(t.p[0]), proj(t.p[1]), proj(t.p[2])},
                 [](double z, double old) { return z > old; },
                 [&](u8* o, const u8* rgb, bool textured) {
                     const double lum = textured ? 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2] : 150;
                     const u8 g = (u8)std::min(255, (int)((90 + lum * 0.45) * shade));
                     o[0] = o[1] = o[2] = g;
                     o[3] = 255;
                 });
    }
    return resize(img, kHudW, kHudH);
}

// New (x, y) per layout item: wheels and suspension at the car's wheels, the others (steering, strut,
// engine) moved proportionally from the template's axles to the car's.
bool new_positions(const std::vector<Part>& parts, const CarModel& car, std::vector<std::pair<double, double>>& out) {
    const Fit f = fit(car);
    for (const char* w : {"whlfl", "whlfr", "whlrl", "whlrr"})
        if (!car.wheels.count(w)) return false;
    auto hud = [&](const char* w) { const auto& p = car.wheels.at(w); return f.hud(p.first, p.second); };
    const auto [fy, ry, ftrack, rtrack] = wheel_spots(parts);
    const double nfy = (hud("whlfl").second + hud("whlfr").second) / 2, nry = (hud("whlrl").second + hud("whlrr").second) / 2;
    const double nft = (std::fabs(hud("whlfl").first) + std::fabs(hud("whlfr").first)) / 2;
    const double nrt = (std::fabs(hud("whlrl").first) + std::fabs(hud("whlrr").first)) / 2;
    auto map_y = [&](double y) { return ry != fy ? nfy + (y - fy) * (nry - nfy) / (ry - fy) : y; };
    auto map_x = [&](double x, double y) {
        const double t = ry != fy ? std::min(1.0, std::max(0.0, (y - fy) / (ry - fy))) : 0.5;
        const double old_track = ftrack + (rtrack - ftrack) * t, new_track = nft + (nrt - nft) * t;
        return old_track ? x * new_track / old_track : x;
    };
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    out.clear();
    for (const auto& p : parts) {
        if (!p.named || p.name == "anchor") out.push_back({p.x, p.y});  // the car image itself stays centred
        else out.push_back({r1(map_x(p.x, p.y)), r1(map_y(p.y))});
    }
    return true;
}

// Rewrites a compiled layout: the image name, and each item's x/y (as new constants, since values are
// shared between items).
Bytes rewrite_layout(const Bytes& src, const std::string& image_name, const std::vector<std::pair<double, double>>& positions) {
    Bytes d = src;
    const auto funcs = lua_load(d);
    const LuaFunc& f = funcs[0];
    std::vector<LuaConst> K = f.k;
    auto kindex = [&](const LuaConst& c) {
        for (size_t i = 0; i < K.size(); i++)
            if (K[i] == c) return (u32)i;
        K.push_back(c);
        return (u32)(K.size() - 1);
    };
    for (auto& c : K)
        if (c.type == LuaConst::Str && lower(c.str).rfind("damage\\", 0) == 0) c.str = "damage\\" + image_name;
    int item = -1;
    bool naming = false, had_xy = false;
    for (u32 i = 0; i < f.ncode; i++) {
        const size_t at = f.code_at + 4 * i;
        u32 ins = ins_at(d, at);
        const u32 op = op_of(ins), b = b_of(ins), c = c_of(ins);
        if (op == 5) {
            naming = true;
            had_xy = false;
        } else if (op == 9 && b >= 256 && c >= 256 && naming && b - 256 < K.size() && K[b - 256].type == LuaConst::Str &&
                   (K[b - 256].str == "x" || K[b - 256].str == "y")) {
            had_xy = true;
            if (item + 1 < (int)positions.size()) {
                const auto& pos = positions[item + 1];
                LuaConst v;
                v.type = LuaConst::Num;
                v.num = K[b - 256].str == "x" ? pos.first : pos.second;
                const u32 k = kindex(v);
                if (k > 255) throw std::runtime_error("layout: too many constants");
                ins = (ins & ~(0x1ffu << 14)) | ((k + 256) << 14);
                memcpy(&d[at], &ins, 4);
            }
        } else if (op == 28 && naming) {
            if (had_xy) item++;
            naming = false;
        }
    }
    Bytes out(d.begin(), d.begin() + f.k_start);
    const Bytes sec = lua_const_section(K);
    out.insert(out.end(), sec.begin(), sec.end());
    out.insert(out.end(), d.begin() + f.k_end, d.end());
    lua_load(out);  // sanity: still parses
    return out;
}

}  // namespace

std::unique_ptr<CarModel> load_car(const fs::path& folder) {
    auto car = std::make_unique<CarModel>();
    auto texture_for = [&](const std::string& material) -> const Rgba* {
        auto it = car->textures.find(material);
        if (it != car->textures.end()) return it->second.get();
        std::unique_ptr<Rgba> tex;
        Bytes mtl;
        if (read_file(folder / (upper(material) + ".MTL"), mtl)) {
            const std::string name = mtl_texture(mtl);
            Bytes img;
            if (!name.empty() && read_file(folder / (upper(name) + ".IMG"), img)) tex = std::make_unique<Rgba>(read_img(img));
        }
        const Rgba* p = tex.get();
        car->textures[material] = std::move(tex);
        return p;
    };
    const CntNode root = read_cnt(read_file(folder / "CARBODY.CNT"));
    std::vector<const CntNode*> nodes{&root};
    for (size_t k = 0; k < nodes.size(); k++)
        for (const auto& c : nodes[k]->children) nodes.push_back(&c);
    for (const CntNode* node : nodes) {
        if (lower(node->name).rfind("whl", 0) == 0) car->wheels[lower(node->name)] = {node->m[9], node->m[11]};
        if (node->model.empty()) continue;
        Bytes md;
        if (!read_file(folder / (upper(node->model) + ".MDL"), md)) continue;
        const Mdl m = read_mdl(md);
        for (const auto& fc : m.faces) {
            CarTri t;
            for (int k = 0; k < 3; k++) {
                const MdlVert& v = m.verts.at(fc[2 + k]);
                t.p[k] = {v.f[0] + node->m[9], v.f[1] + node->m[10], v.f[2] + node->m[11]};
                t.uv[k] = {v.f[6], v.f[7]};
            }
            t.tex = texture_for(m.materials.at(fc[0]));
            car->tris.push_back(t);
        }
    }
    if (car->tris.empty()) throw std::runtime_error("no geometry in " + folder.string());
    return car;
}

void make_pictures(Install& inst, const fs::path& content, const std::string& name, const CarModel& car,
                   const fs::path& mug_fli) {
    const fs::path assets = content / "UI" / "ASSETS";
    const std::string file = upper(name) + ".IMG";
    const Rgba big = render_car(car, 480, 480, -35, -28, 0.66, 1);
    for (const Pic& p : kCarPictures)
        if (fs::is_directory(assets / p.dir)) inst.write(assets / p.dir / file, img_rle(resize(big, p.w, p.h)));
    for (const Pic& p : kGridPictures)
        if (fs::is_directory(assets / p.dir))
            inst.write(assets / p.dir / file, img_rle(render_top(car, p.w, p.h, kGridPxPerM300 * p.w / 300)));
    if (!mug_fli.empty() && fs::exists(mug_fli))
        for (const Pic& p : kDriverPictures)
            if (fs::is_directory(assets / p.dir)) inst.write(assets / p.dir / file, img_rle(portrait(mug_fli, p.w)));
}

int make_damage_hud(Install& inst, const fs::path& content, const std::string& name, const CarModel& car) {
    const fs::path ui = content / "UI";
    const std::string layout_name = upper(name) + "_LAYOUT.LOL";
    std::vector<fs::path> layouts, damage_dirs;
    for (const auto& e : fs::recursive_directory_iterator(ui)) {
        if (e.is_directory() && upper(e.path().filename().string()) == "DAMAGE" &&
            upper(e.path().string()).find("ASSETS") != std::string::npos)
            damage_dirs.push_back(e.path());
        if (e.is_regular_file() && upper(e.path().filename().string()) == layout_name &&
            upper(e.path().parent_path().string()).find("HUD_DAMAGE") != std::string::npos)
            layouts.push_back(e.path());
    }
    if (layouts.empty()) return 0;
    const Bytes img = img_rle(render_silhouette(car));
    for (const auto& dir : damage_dirs) inst.write(dir / (upper(name) + ".IMG"), img);
    for (const auto& p : layouts) {
        const Bytes d = read_file(p);
        const auto parts = layout_parts(d);
        std::vector<std::pair<double, double>> pos;
        if (!new_positions(parts, car, pos))
            for (const auto& part : parts) pos.push_back({part.x, part.y});
        inst.write(p, rewrite_layout(d, name, pos));
    }
    return (int)layouts.size();
}

}  // namespace pcimport
