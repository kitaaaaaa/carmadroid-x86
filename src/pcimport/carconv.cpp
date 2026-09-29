// A PC Carmageddon car -> an Android vehicle folder (port of tools/pc2android/carconv.py).
#include "pcimport/cars.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace pcimport {
namespace {

constexpr double kScale = 6.9;  // C1 world units -> metres (dethrace WORLD_SCALE)
const std::map<std::string, std::string> kWheels = {
    {"FLWHEEL.ACT", "whlFL"}, {"FRWHEEL.ACT", "whlFR"}, {"RLWHEEL.ACT", "whlRL"}, {"RRWHEEL.ACT", "whlRR"}};

// Every car's wheels hang from mounts 0.2 m above the car's base, near the tyres' outer edges, with only
// 0.27 m of travel (the wheel radius doesn't change that). If the body is higher than the stock cars'
// (0.35 m at most), landing, rolling in a turn or hitting a kerb pushes mounts below the road, the
// wheels lose the ground and the car sits on its body.
constexpr double kLowestBody = 0.35;

using M12 = std::array<double, 12>;

// BRender matrices are row-vector 4x3 (rows: x axis, y axis, z axis, translation). a then b.
M12 mat_mul(const M12& a, const M12& b) {
    M12 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += a[i * 3 + k] * b[k * 3 + j];
            r[i * 3 + j] = s + (i == 3 ? b[9 + j] : 0);
        }
    return r;
}

V3 xform(const M12& m, const std::array<float, 3>& v) {
    const double x = v[0], y = v[1], z = v[2];
    return {x * m[0] + y * m[3] + z * m[6] + m[9], x * m[1] + y * m[4] + z * m[7] + m[10],
            x * m[2] + y * m[5] + z * m[8] + m[11]};
}

double det3(const M12& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// C1 position -> Android: scaled, z flipped (a mirror, so triangle winding flips too).
V3 to_android(const V3& p) { return {p[0] * kScale, p[1] * kScale, -p[2] * kScale}; }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 norm(const V3& a) {
    const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 1e-12 ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 1, 0};
}
double dist(const V3& a, const V3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

struct Corner {
    V3 p;
    std::array<double, 2> uv;
};
struct Tri {
    std::string mat;
    std::array<Corner, 3> c;
    bool back;
};

// Triangles with per-corner data, grouped by material name.
struct Mesh {
    std::vector<Tri> tris;
    const std::set<std::string>* two_sided;              // Android material names
    const std::map<std::string, std::pair<int, int>>* inset;  // material -> texture size: half-texel UV inset

    void add_model(const BrModel& model, const M12& matrix, const std::function<std::string(const std::string&)>& mat_names,
                   const std::string& default_mat) {
        const bool flip = det3(matrix) < 0;
        for (size_t fi = 0; fi < model.faces.size(); fi++) {
            const int mi = fi < model.face_mats.size() ? model.face_mats[fi] : 0;
            const std::string mat = mi > 0 && (size_t)(mi - 1) < model.materials.size() ? mat_names(model.materials[mi - 1])
                                                                                         : default_mat;
            std::array<Corner, 3> cs;
            for (int k = 0; k < 3; k++) {
                const u16 vi = model.faces[fi][k];
                if (vi >= model.verts.size()) throw std::runtime_error("bad vertex index in " + model.name);
                cs[k].p = to_android(xform(matrix, model.verts[vi]));
                cs[k].uv = vi < model.uvs.size() ? std::array<double, 2>{model.uvs[vi][0], model.uvs[vi][1]}
                                                 : std::array<double, 2>{0, 0};
                if (auto it = inset->find(mat); it != inset->end()) {  // sample texel centres like the PC renderer
                    const double tw = it->second.first, th = it->second.second;
                    cs[k].uv = {cs[k].uv[0] * (tw - 1) / tw + 0.5 / tw, cs[k].uv[1] * (th - 1) / th + 0.5 / th};
                }
            }
            // the z flip mirrors: reverse the winding (and again if the actor matrix mirrors)
            if (!flip) std::swap(cs[1], cs[2]);
            tris.push_back({mat, cs, false});
            if (two_sided->count(mat))  // PC two-sided material: also draw the inside
                tris.push_back({mat, {cs[0], cs[2], cs[1]}, true});
        }
    }

    void bounds(V3& lo, V3& hi) const {
        lo = {1e30, 1e30, 1e30};
        hi = {-1e30, -1e30, -1e30};
        for (const auto& t : tris)
            for (const auto& c : t.c)
                for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], c.p[i]), hi[i] = std::max(hi[i], c.p[i]);
    }
};

using PosKey = std::tuple<long long, long long, long long, bool>;
PosKey pos_key(const V3& p, bool back) {
    return {std::llround(p[0] * 1e5), std::llround(p[1] * 1e5), std::llround(p[2] * 1e5), back};
}

// An MDL (v6.2, with USER data) from a Mesh.
Mdl build_mdl(const Mesh& mesh) {
    std::vector<std::string> mats;
    for (const auto& t : mesh.tris)
        if (std::find(mats.begin(), mats.end(), t.mat) == mats.end()) mats.push_back(t.mat);
    // smooth normals per position (C1 models share vertices between faces)
    std::vector<V3> face_n;
    std::map<PosKey, V3> acc;
    for (const auto& t : mesh.tris) {
        const V3 n = norm(cross(sub(t.c[1].p, t.c[0].p), sub(t.c[2].p, t.c[0].p)));
        face_n.push_back(n);
        for (const auto& c : t.c) {
            V3& a = acc.try_emplace(pos_key(c.p, t.back), V3{0, 0, 0}).first->second;
            a[0] += n[0], a[1] += n[1], a[2] += n[2];
        }
    }
    struct Vert {
        V3 p, n;
        std::array<double, 2> uv;
    };
    std::vector<Vert> verts;
    Mdl out;
    out.materials = mats;
    out.flags = 1;  // USER data present
    for (size_t mi = 0; mi < mats.size(); mi++) {
        const u32 start = (u32)verts.size();
        std::map<std::tuple<PosKey, long long, long long>, u32> index;
        std::vector<u32> tri_list;
        for (const auto& t : mesh.tris) {
            if (t.mat != mats[mi]) continue;
            u32 ids[3];
            for (int k = 0; k < 3; k++) {
                const auto& c = t.c[k];
                const PosKey pk = pos_key(c.p, t.back);
                const auto key = std::make_tuple(pk, std::llround(c.uv[0] * 1e5), std::llround(c.uv[1] * 1e5));
                auto it = index.find(key);
                if (it == index.end()) {
                    it = index.emplace(key, (u32)verts.size()).first;
                    verts.push_back({c.p, norm(acc[pk]), c.uv});
                }
                ids[k] = it->second;
            }
            out.faces.push_back({(u32)mi, 0, ids[0], ids[1], ids[2]});
            for (u32 id : ids) tri_list.push_back(id - start);
        }
        MdlGroup g;
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (size_t i = start; i < verts.size(); i++)
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], verts[i].p[k]), hi[k] = std::max(hi[k], verts[i].p[k]);
        const V3 centre{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
        for (size_t i = start; i < verts.size(); i++) g.radius = std::max(g.radius, dist(centre, verts[i].p));
        g.centre = centre;
        g.mn = lo;
        g.mx = hi;
        g.strip_off = start;
        g.list_off = start;
        g.list_count = (u32)(verts.size() - start);
        g.list = std::move(tri_list);
        out.groups.push_back(std::move(g));
    }
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const auto& v : verts)
        for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], v.p[k]), hi[k] = std::max(hi[k], v.p[k]);
    out.bmin = lo;
    out.bmax = hi;
    out.centre = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    for (const auto& v : verts) out.radius = std::max(out.radius, dist(out.centre, v.p));
    out.user_verts = (u32)verts.size();
    out.user_faces = (u32)out.faces.size();
    for (const auto& v : verts) {
        MdlVert mv;
        const double f[10] = {v.p[0], v.p[1], v.p[2], v.n[0], v.n[1], v.n[2], v.uv[0], v.uv[1], 0, 0};
        for (int k = 0; k < 10; k++) mv.f[k] = (float)f[k];
        mv.c[0] = mv.c[1] = mv.c[2] = 128;
        mv.c[3] = 255;
        out.verts.push_back(mv);
    }
    // USER data: flags, verts (x,y,z,1), faces (137 bytes), PREP->USER face and vertex lookups
    Writer w;
    w.u32_(0);
    for (const auto& v : verts) {
        w.fs(v.p);
        w.u32_(1);
    }
    for (const auto& f : out.faces) {
        const Vert &va = verts[f[2]], &vb = verts[f[3]], &vc = verts[f[4]];
        const V3 n = norm(cross(sub(vb.p, va.p), sub(vc.p, va.p)));
        w.f32(-(n[0] * va.p[0] + n[1] * va.p[1] + n[2] * va.p[2]));
        w.fs(n);
        for (const Vert* v : {&va, &vb, &vc}) w.fs(v->n);
        w.u32_(f[0]);
        w.u32_(0);
        w.u32_(f[2]);
        w.u32_(f[3]);
        w.u32_(f[4]);
        for (int k = 0; k < 3; k++) {
            const u8 c[4] = {128, 128, 128, 255};
            w.raw(c, 4);
        }
        for (const Vert* v : {&va, &vb, &vc}) {
            w.f32(v->uv[0]);
            w.f32(v->uv[1]);
            w.f32(0);
            w.f32(0);
        }
        w.u8_(0);
        w.u32_(0);
    }
    for (u32 i = 0; i < out.faces.size(); i++) w.u32_(i);
    w.u32_((u32)verts.size());
    for (u32 i = 0; i < verts.size(); i++) w.u32_(i);
    out.tail = std::move(w.b);
    return out;
}

// Gives transparent pixels the colour of a neighbouring opaque one (repeatedly), so texture filtering
// at the edge of a see-through area doesn't blend in black.
void bleed(Rgba& img) {
    const int w = img.w, h = img.h;
    std::vector<char> solid((size_t)w * h);
    bool any = false, all = true;
    for (size_t i = 0; i < solid.size(); i++) {
        solid[i] = img.px[i * 4 + 3] >= 128;
        any |= solid[i] != 0;
        all &= solid[i] != 0;
    }
    if (all || !any) return;
    for (int iter = 0; iter < 8; iter++) {
        std::vector<size_t> grown;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = (size_t)y * w + x;
                if (solid[i]) continue;
                static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& dxy : dirs) {
                    const size_t j = (size_t)((y + dxy[1] + h) % h) * w + (x + dxy[0] + w) % w;
                    if (solid[j]) {
                        memcpy(&img.px[i * 4], &img.px[j * 4], 3);
                        grown.push_back(i);
                        break;
                    }
                }
            }
        if (grown.empty()) break;
        for (size_t i : grown) solid[i] = 1;
    }
}

bool has_transparency(const Rgba& img) {
    for (size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] < 128) return true;
    return false;
}

// All named pixelmaps in the car's PIX files (the first file that has a name wins).
struct PixelmapSource {
    std::map<std::string, BrPixmap> maps;
    const std::vector<RGB>* palette;

    bool rgba(const std::string& name, Rgba& out) const {
        auto it = maps.find(upper(name));
        if (it == maps.end()) return false;
        const BrPixmap& m = it->second;
        const std::vector<RGB>& pal = m.palette.empty() ? *palette : m.palette;
        out = Rgba(m.w, m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                const size_t s = (size_t)y * m.row + x;
                const u8 i = s < m.px.size() ? m.px[s] : 0;
                const RGB c = i < pal.size() ? pal[i] : RGB{0, 0, 0};
                u8* o = &out.px[((size_t)y * m.w + x) * 4];
                o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = i == 0 ? 0 : 255;
            }
        return true;
    }
};

fs::path find(const std::vector<fs::path>& dirs, const fs::path& sub, const std::string& name) {
    for (const auto& d : dirs) {
        const fs::path p = d / sub / name;
        if (fs::exists(p)) return p;
    }
    throw std::runtime_error("missing " + (sub / name).string());
}

std::string fmt_f(double v) {
    char b[64];
    snprintf(b, sizeof b, "%f", v);
    return b;
}

// Collision shape for CAR.TXT: a box around the body. A body higher than kLowestBody gets a base below
// it at kLowestBody, as wide as the tyres' outer edges and reaching half a wheel radius past the axles,
// so it meets the road before any wheel mount can. wheels: (x, z, half width, radius) each.
std::string shape_text(const V3& lo, const V3& hi, const std::vector<std::array<double, 4>>& wheels) {
    if (lo[1] <= kLowestBody)
        return "(default label)\n1\nRoundedAlignedCuboid\n0.15\n" + fmt_f(lo[0]) + "," + fmt_f(std::max(lo[1], 0.2)) +
               "," + fmt_f(lo[2]) + "\n" + fmt_f(hi[0]) + "," + fmt_f(hi[1]) + "," + fmt_f(hi[2]) + "\n";
    std::vector<V3> pts;
    for (double x : {lo[0], hi[0]})
        for (double y : {lo[1], hi[1]})
            for (double z : {lo[2], hi[2]}) pts.push_back({x, y, z});
    if (!wheels.empty()) {
        double mid = 0;
        for (const auto& w : wheels) mid += w[1];
        mid /= wheels.size();
        for (const auto& w : wheels)
            pts.push_back({w[0] + (w[0] > 0 ? w[2] : -w[2]), kLowestBody, w[1] + (w[1] > mid ? w[3] : -w[3]) * 0.5});
    } else {
        const double cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2, hx = (hi[0] - lo[0]) / 2, hz = (hi[2] - lo[2]) / 2;
        for (int sx : {-1, 1})
            for (int sz : {-1, 1}) pts.push_back({cx + sx * hx, kLowestBody, cz + sz * hz * 0.6});
    }
    std::string s = "(default label)\n1\nRoundedPolyhedron\n0.150000\n" + std::to_string(pts.size()) + "\n";
    for (const auto& q : pts) s += fmt_f(q[0]) + "," + fmt_f(q[1]) + "," + fmt_f(q[2]) + "\n";
    return s + "\nform_collision_groups\n1\n";
}

}  // namespace

void convert_car(Install& inst, const std::vector<fs::path>& data_dirs, const std::string& car_txt,
                 const fs::path& out_dir, const fs::path& template_dir, const Bytes& mtl_template) {
    const auto lines = c1_data_lines(find(data_dirs, "CARS", car_txt));
    if (lines.empty()) throw std::runtime_error(car_txt + " is empty");
    std::string car = upper(split(trim(lines[0]), ' ')[0]);
    if (size_t t = car.find(".TXT"); t != std::string::npos) car.erase(t, 4);
    // File lists come after the three grid images ("a.PIX,b.PIX,c.PIX").
    size_t i = 0;
    for (;; i++) {
        if (i >= lines.size()) throw std::runtime_error(car_txt + ": no grid image line");
        const auto f = split(lines[i], ',');
        if (f.size() == 3 && std::all_of(f.begin(), f.end(), [](const std::string& x) {
                const std::string u = upper(trim(x));
                return u.size() >= 4 && u.compare(u.size() - 4, 4, ".PIX") == 0;
            }))
            break;
    }
    i++;
    auto take_list = [&]() {
        if (i >= lines.size()) throw std::runtime_error(car_txt + ": truncated");
        const int n = std::stoi(lines[i++]);
        if (n < 0 || i + n > lines.size()) throw std::runtime_error(car_txt + ": bad list");
        std::vector<std::string> items(lines.begin() + i, lines.begin() + i + n);
        i += n;
        return items;
    };
    const auto pix = take_list();  // three detail levels: the first one
    take_list();
    take_list();
    take_list();  // shade tables
    const auto mats = take_list();
    take_list();
    take_list();
    const auto dats = take_list();
    if (i >= lines.size()) throw std::runtime_error(car_txt + ": truncated");
    const int nact = std::stoi(lines[i++]);
    std::string actor_file;
    for (int k = 0; k < nact && i + k < lines.size(); k++) {
        const auto a = split(lines[i + k], ',');
        if (a.size() >= 2 && trim(a[0]) == "0") {
            actor_file = trim(a[1]);
            break;
        }
    }
    if (actor_file.empty()) throw std::runtime_error(car_txt + ": no actor file");

    std::map<std::string, BrModel> models;
    for (const auto& d : dats)
        for (auto& [k, v] : read_dat(find(data_dirs, "MODELS", d))) models[k] = std::move(v);
    std::map<std::string, BrMaterial> materials;
    for (const auto& m : mats)
        for (auto& [k, v] : read_mat(find(data_dirs, "MATERIAL", m))) materials[k] = std::move(v);
    const std::vector<RGB> palette = read_palette(find(data_dirs, fs::path("REG") / "PALETTES", "DRRENDER.PAL"));
    PixelmapSource pixmaps;
    pixmaps.palette = &palette;
    for (const auto& f : pix)
        for (const auto& d : data_dirs) {
            const fs::path p = d / "PIXELMAP" / f;
            if (fs::exists(p)) {
                for (auto& [k, v] : read_all_pix(p)) pixmaps.maps.emplace(k, std::move(v));
                break;
            }
        }
    auto actors = read_act(find(data_dirs, "ACTORS", actor_file));
    if (actors.empty()) throw std::runtime_error(actor_file + ": no actors");
    const BrActor& root = actors[0];

    const std::string prefix = lower(car).substr(0, 6);
    auto android_name = [&](const std::string& key) {
        std::string k = key;
        if (size_t t = k.find(".MAT"); t != std::string::npos) k.erase(t, 4);
        return prefix + "_" + lower(k);
    };
    std::vector<std::pair<std::string, std::string>> used_mats;  // PC key -> Android name, in first-use order
    auto mat_name = [&](const std::string& pc_name) {
        const std::string key = upper(pc_name);
        for (const auto& u : used_mats)
            if (u.first == key) return u.second;
        used_mats.emplace_back(key, android_name(key));
        return used_mats.back().second;
    };

    std::set<std::string> two_sided;
    std::map<std::string, std::pair<int, int>> inset;
    for (const auto& [k, m] : materials) {
        if (m.flags & 0x1000) two_sided.insert(android_name(upper(k)));
        Rgba t;
        if (!m.texture.empty() && pixmaps.rgba(m.texture, t) && has_transparency(t))
            inset[android_name(upper(k))] = {t.w, t.h};
    }
    Mesh body{{}, &two_sided, &inset};
    std::map<std::string, std::pair<M12, Mesh>> wheels;  // Android node name -> (world matrix, mesh)
    std::vector<std::string> wheel_order;

    std::function<void(const BrActor&, const M12&, const std::string&)> visit =
        [&](const BrActor& actor, const M12& parent_m, const std::string& inherited_mat) {
            const M12 m = mat_mul(actor.m, parent_m);
            const std::string mat = actor.material.empty() ? inherited_mat : actor.material;
            const auto wheel = kWheels.find(upper(actor.name));
            if (!actor.model.empty()) {
                auto mi = models.find(upper(actor.model));
                if (mi != models.end()) {
                    const std::string def = !mat.empty() ? mat_name(mat) : mat_name("DEFAULT");
                    if (wheel != kWheels.end()) {
                        Mesh mesh{{}, &two_sided, &inset};
                        M12 rot = m;
                        rot[9] = rot[10] = rot[11] = 0;
                        mesh.add_model(mi->second, rot, mat_name, def);
                        if (!wheels.count(wheel->second)) wheel_order.push_back(wheel->second);
                        wheels[wheel->second] = {m, std::move(mesh)};
                    } else {
                        body.add_model(mi->second, m, mat_name, def);
                    }
                }
            }
            for (const auto& c : actor.children) visit(c, m, mat);
        };
    visit(root, M12{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, "");
    if (body.tris.empty()) throw std::runtime_error(car + ": no body geometry");

    inst.write(out_dir / "CARBODY.MDL", write_mdl(build_mdl(body)));
    CntNode cnt;
    cnt.name = "carbody";
    cnt.kind = "MODL";
    cnt.model = "carbody";
    for (const char* name : {"whlFL", "whlFR", "whlRL", "whlRR"}) {
        auto it = wheels.find(name);
        if (it == wheels.end()) continue;
        inst.write(out_dir / (upper(name) + ".MDL"), write_mdl(build_mdl(it->second.second)));
        const M12& m = it->second.first;
        const V3 pos = to_android({m[9], m[10], m[11]});
        CntNode n;
        n.name = name;
        n.kind = "MODL";
        n.model = name;
        n.m = {1, 0, 0, 0, 1, 0, 0, 0, 1, pos[0], pos[1], pos[2]};
        cnt.children.push_back(n);
    }
    inst.write(out_dir / "CARBODY.CNT", write_cnt(cnt));

    // materials and textures
    for (const auto& [pc_name, name] : used_mats) {
        const BrMaterial* m = nullptr;
        if (auto it = materials.find(pc_name); it != materials.end()) m = &it->second;
        else {
            std::string k = pc_name;
            if (size_t t = k.find(".MAT"); t != std::string::npos) k.erase(t, 4);
            if (auto it2 = materials.find(upper(k)); it2 != materials.end()) m = &it2->second;
        }
        Rgba tex;
        const bool textured = m && !m->texture.empty() && pixmaps.rgba(m->texture, tex);
        if (!textured) {  // untextured: a small solid texture from the material colour
            std::array<u8, 4> c = m ? m->colour : std::array<u8, 4>{160, 160, 160, 255};
            if (m && c[0] == 0 && c[1] == 0 && c[2] == 0) c = {palette[0][0], palette[0][1], palette[0][2], 255};
            tex = Rgba(8, 8);
            for (int k = 0; k < 64; k++) tex.px[k * 4] = c[0], tex.px[k * 4 + 1] = c[1], tex.px[k * 4 + 2] = c[2], tex.px[k * 4 + 3] = 255;
        }
        bleed(tex);
        // Solid textures: one uncompressed plane (drawn opaque). See-through ones: RLE planes with the
        // one-bit-alpha flag, as the game's own see-through textures.
        inst.write(out_dir / (upper(name) + ".IMG"), has_transparency(tex) ? img_rle(tex, 0x06) : img_plain(tex));
        inst.write(out_dir / (upper(name) + ".MTL"), mtl_with_texture(mtl_template, name));
    }

    // CAR.TXT from the template car, with the collision shape from the new body. The checksum after it
    // is set to -1 (as in cars whose shapes were edited by hand), since the shape changed.
    const Bytes tb = read_file(template_dir / "CAR.TXT");
    std::string txt;
    for (size_t k = 0; k < tb.size(); k++) {
        if (tb[k] == '\r') {
            txt += '\n';
            if (k + 1 < tb.size() && tb[k + 1] == '\n') k++;
        } else {
            txt += (char)tb[k];
        }
    }
    const size_t a = txt.find("<Shape>"), b = txt.find("<ShapeCheckSum>");
    if (a == std::string::npos || b == std::string::npos) throw std::runtime_error("template CAR.TXT has no <Shape>");
    size_t e = txt.find('\n', txt.find('\n', b) + 1);
    if (e == std::string::npos) e = txt.size();
    V3 lo, hi;
    body.bounds(lo, hi);
    std::vector<std::array<double, 4>> wheel_spots;
    for (const auto& name : wheel_order) {
        const auto& [m, mesh] = wheels[name];
        const V3 pos = to_android({m[9], m[10], m[11]});
        V3 wlo, whi;
        mesh.bounds(wlo, whi);
        wheel_spots.push_back({pos[0], pos[2], (whi[0] - wlo[0]) / 2, (whi[1] - wlo[1]) / 2});
    }
    txt = txt.substr(0, a) + "<Shape>\n" + shape_text(lo, hi, wheel_spots) + "\n\n<ShapeCheckSum>\n-1" + txt.substr(e);
    inst.write(out_dir / "CAR.TXT", txt);
}

}  // namespace pcimport
