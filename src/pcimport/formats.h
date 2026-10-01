#pragma once
// File formats used when importing PC Carmageddon content (a port of tools/pc2android):
//   PC (BRender, big-endian chunks): DAT models, ACT actor trees, MAT materials, PIX pixelmaps;
//   C1 text files ('@' lines are encrypted); FLI/FLC animations (first frame only);
//   Android (Stainless): CNT node trees, MDL meshes, MTL materials, IMG textures, LOL (Lua 5.1 bytecode).
// Readers throw std::runtime_error on malformed files.
#include "common.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pcimport {

namespace fs = std::filesystem;
using Bytes = std::vector<u8>;
using V3 = std::array<double, 3>;
using RGB = std::array<u8, 3>;

bool read_file(const fs::path& path, Bytes& out);
Bytes read_file(const fs::path& path);  // throws if missing
std::string upper(std::string s);        // ASCII and Latin-1 letters
std::string lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

// Little-endian byte writer / reader.
struct Writer {
    Bytes b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const u8*)p, (const u8*)p + n); }
    void raw(const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
    void u8_(u8 v) { b.push_back(v); }
    void u16_(u16 v) { raw(&v, 2); }
    void u32_(u32 v) { raw(&v, 4); }
    void f32(double v) { float f = (float)v; raw(&f, 4); }
    void f64(double v) { raw(&v, 8); }
    template <class T> void fs(const T& vs) { for (double v : vs) f32(v); }
    void pstr(const std::string& s, int extra_pad = 0);  // u32 length, chars, padded to 4 (+extra_pad)
};
struct Reader {
    const u8* d;
    size_t n, p = 0;
    Reader(const Bytes& b, size_t pos = 0) : d(b.data()), n(b.size()), p(pos) {}
    const u8* take(size_t k);
    u8 u8_() { return *take(1); }
    u16 u16_() { u16 v; memcpy(&v, take(2), 2); return v; }
    u32 u32_() { u32 v; memcpy(&v, take(4), 4); return v; }
    float f32() { float v; memcpy(&v, take(4), 4); return v; }
    std::string pstr(int extra_pad = 0);
};

// ---- C1 text ------------------------------------------------------------------------------
std::string c1_decode_line(const std::string& line);
std::vector<std::string> c1_read_text(const fs::path& path);   // decoded lines, comments kept
std::vector<std::string> c1_data_lines(const fs::path& path);  // decoded, no comments, trimmed, non-empty

// ---- BRender -----------------------------------------------------------------------------
struct BrModel {
    std::string name;
    std::vector<std::array<float, 3>> verts;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<u16, 3>> faces;
    std::vector<std::string> materials;
    std::vector<u16> face_mats;  // 1-based material index per face (0 = none)
};
struct BrActor {
    std::string name, model, material;
    std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // rows: x, y, z axes, translation
    std::vector<BrActor> children;
};
struct BrMaterial {
    std::string name, texture;
    std::array<u8, 4> colour{255, 255, 255, 255};
    u32 flags = 0;
};
struct BrPixmap {
    int w = 0, h = 0, row = 0;
    Bytes px;
    std::vector<RGB> palette;  // own palette, if the file has one for this map
};
std::map<std::string, BrModel> read_dat(const fs::path& path);           // by upper-case name
std::vector<BrActor> read_act(const fs::path& path);                     // root actors
std::map<std::string, BrMaterial> read_mat(const fs::path& path);        // by upper-case name
std::map<std::string, BrPixmap> read_all_pix(const fs::path& path);      // by upper-case name
std::vector<RGB> read_palette(const fs::path& path);                     // 256 entries

// ---- FLI/FLC -----------------------------------------------------------------------------
struct FliFrame {
    int w = 0, h = 0;
    Bytes px;
    std::array<RGB, 256> pal{};
};
FliFrame fli_first_frame(const fs::path& path);

// ---- Android: images ------------------------------------------------------------------------
struct Rgba {
    int w = 0, h = 0;
    Bytes px;  // r, g, b, a
    Rgba() = default;
    Rgba(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4) {}
};
// IMG v1.0 format 6: 4 RLE planes (A, R, G, B). basic 0x6b as the game's menu pictures; 0x06 for
// textures with one-bit alpha.
Bytes img_rle(const Rgba& img, u8 basic = 0x6b);
Bytes img_plain(const Rgba& img);  // format 0: one A,R,G,B plane, drawn opaque
Rgba read_img(const Bytes& data);  // the two formats above
Rgba resize(const Rgba& img, int nw, int nh);  // area average, premultiplied alpha

// ---- Android: CNT / MDL / MTL ---------------------------------------------------------------
struct CntNode {
    std::string name, kind = "NULL", model;
    std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::vector<CntNode> children;
};
Bytes write_cnt(const CntNode& root);
CntNode read_cnt(const Bytes& data);  // MODL / SKIN / NULL nodes only (what the converter writes)

struct MdlVert {
    float f[10];  // x, y, z, nx, ny, nz, u, v, u2, v2
    u8 c[4];
};
struct MdlGroup {
    std::array<double, 3> centre{}, mn{}, mx{};
    double radius = 0;
    u32 strip_off = 0, strip_count = 0, list_off = 0, list_count = 0;
    std::vector<u32> strip, list;
};
struct Mdl {
    u32 checksum = 0, flags = 0, user_faces = 0, user_verts = 0, unknown = 1;
    double radius = 0;
    std::array<double, 3> bmin{}, bmax{}, centre{};
    std::vector<std::string> materials;
    std::vector<std::array<u32, 5>> faces;  // material, flags, a, b, c
    std::vector<MdlVert> verts;
    std::vector<MdlGroup> groups;
    Bytes tail;
};
Bytes write_mdl(const Mdl& m);
Mdl read_mdl(const Bytes& data);

Bytes mtl_with_texture(const Bytes& one_texture_template, const std::string& texture);
// The same with the game's car reflection: stage 1 "env" (the track's environment map) and stage 2
// "<texture>_s" (the shine mask), with those stages' settings from a stock car's body material.
// env_texture: the reflected texture ("env" is the track's ENV.IMG).
Bytes mtl_with_reflection(const Bytes& one_texture_template, const Bytes& car_template, const std::string& texture,
                          const std::string& env_texture = "env");
std::string mtl_texture(const Bytes& mtl);  // first texture name, or ""

// ---- LOL (Lua 5.1 bytecode) ---------------------------------------------------------------
struct LuaConst {
    enum Type { Nil = 0, Bool = 1, Num = 3, Str = 4 } type = Nil;
    double num = 0;  // number, or the bool value
    std::string str;
    bool str_null = false;
    bool operator==(const LuaConst& o) const {
        if (type != o.type) return false;
        if (type == Str) return str == o.str && str_null == o.str_null;
        return type == Nil || num == o.num;
    }
};
struct LuaFunc {
    std::vector<LuaConst> k;
    size_t code_at = 0, k_start = 0, k_end = 0;  // file offsets: code, constant section
    u32 ncode = 0;
};
std::vector<LuaFunc> lua_load(const Bytes& data);  // the main function first, then nested ones
Bytes lua_const_section(const std::vector<LuaConst>& k);

}  // namespace pcimport
