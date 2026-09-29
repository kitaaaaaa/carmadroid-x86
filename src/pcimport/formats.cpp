#include "pcimport/formats.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace pcimport {

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
bool read_file(const fs::path& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

Bytes read_file(const fs::path& path) {
    Bytes b;
    if (!read_file(path, b)) throw std::runtime_error("can't read " + path.string());
    return b;
}

std::string upper(std::string s) {
    for (char& ch : s) {
        const u8 c = (u8)ch;
        if (c >= 'a' && c <= 'z') ch = (char)(c - 32);
        else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) ch = (char)(c - 32);  // Latin-1
    }
    return s;
}

std::string lower(std::string s) {
    for (char& ch : s) {
        const u8 c = (u8)ch;
        if (c >= 'A' && c <= 'Z') ch = (char)(c + 32);
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) ch = (char)(c + 32);
    }
    return s;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\v\f";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) return out;
        p = q + 1;
    }
}

void Writer::pstr(const std::string& s, int extra_pad) {
    u32_((u32)s.size());
    raw(s.data(), s.size());
    b.insert(b.end(), (size_t)((4 - s.size() % 4) % 4 + extra_pad), 0);
}

const u8* Reader::take(size_t k) {
    if (p + k > n) throw std::runtime_error("read past the end of a file");
    const u8* r = d + p;
    p += k;
    return r;
}

std::string Reader::pstr(int extra_pad) {
    const u32 len = u32_();
    const u8* s = take(len);
    take((4 - len % 4) % 4 + extra_pad);
    return std::string((const char*)s, len);
}

namespace {
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
float bef(const u8* p) { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }

// NUL-terminated string at p; returns it and moves p past the NUL.
std::string cstr(const Bytes& d, size_t& p) {
    size_t e = p;
    while (e < d.size() && d[e]) e++;
    if (e >= d.size()) throw std::runtime_error("unterminated string");
    std::string s((const char*)&d[p], e - p);
    p = e + 1;
    return s;
}

void need(const Bytes& d, size_t p, size_t n) {
    if (p + n > d.size()) throw std::runtime_error("truncated file");
}

constexpr u32 kFileHeader = 0x12;  // every BRender file starts with chunk 0x12: u32 type, u32 version
}  // namespace

// ---------------------------------------------------------------------------------------------
// C1 text: '@' lines are encrypted (two keys: one for text, one after a "//" comment marker).
// ---------------------------------------------------------------------------------------------
std::string c1_decode_line(const std::string& line) {
    static const u8 kKey[16] = {0x6C, 0x1B, 0x99, 0x5F, 0xB9, 0xCD, 0x5F, 0x13,
                                0xCB, 0x04, 0x20, 0x0E, 0x5E, 0x1C, 0xA1, 0x0E};
    static const u8 kCommentKey[16] = {0x67, 0xA8, 0xD6, 0x26, 0xB6, 0xDD, 0x45, 0x1B,
                                       0x32, 0x7E, 0x22, 0x13, 0x15, 0xC2, 0x94, 0x37};
    if (line.empty() || line[0] != '@') return line;
    std::string d = line.substr(1);
    const u8* key = kKey;
    size_t seed = d.size() % 16;
    for (size_t i = 0; i < d.size(); i++) {
        if (i >= 2 && d[i - 1] == '/' && d[i - 2] == '/') key = kCommentKey;
        u8 ch = (u8)d[i];
        if (ch == 9) ch = 0x80;
        const u8 c = (u8)(ch - 32);
        if (!(c & 0x80)) ch = (u8)((c ^ (key[seed] & 127)) + 32);
        seed = (seed + 7) % 16;
        if (ch == 0x80) ch = 9;
        d[i] = (char)ch;
    }
    return d;
}

std::vector<std::string> c1_read_text(const fs::path& path) {
    const Bytes b = read_file(path);
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        size_t q = p;
        while (q < b.size() && b[q] != '\n') q++;
        std::string raw((const char*)b.data() + p, q - p);
        while (!raw.empty() && raw.back() == '\r') raw.pop_back();
        out.push_back(c1_decode_line(raw));
        if (q >= b.size()) break;
        p = q + 1;
    }
    return out;
}

std::vector<std::string> c1_data_lines(const fs::path& path) {
    std::vector<std::string> res;
    for (std::string l : c1_read_text(path)) {
        if (size_t c = l.find("//"); c != std::string::npos) l.resize(c);
        l = trim(l);
        if (!l.empty()) res.push_back(l);
    }
    return res;
}

// ---------------------------------------------------------------------------------------------
// BRender files. Chunk lengths in C1 files are unreliable, so most chunks are parsed by type.
// ---------------------------------------------------------------------------------------------
std::map<std::string, BrModel> read_dat(const fs::path& path) {
    const Bytes d = read_file(path);
    std::map<std::string, BrModel> models;
    BrModel* cur = nullptr;
    size_t p = 0;
    auto cur_model = [&]() -> BrModel& {
        if (!cur) throw std::runtime_error("DAT data before a model in " + path.string());
        return *cur;
    };
    while (p + 8 <= d.size()) {
        const u32 id = be32(&d[p]), len = be32(&d[p + 4]);
        p += 8;
        if (id == kFileHeader) {
            p += len;
        } else if (id == 54) {  // model: u16 flags, name
            p += 2;
            const std::string name = cstr(d, p);
            cur = &models[upper(name)];
            *cur = BrModel();
            cur->name = name;
        } else if (id == 23) {  // vertices
            need(d, p, 4);
            const u32 n = be32(&d[p]);
            need(d, p + 4, (size_t)n * 12);
            auto& v = cur_model().verts;
            for (u32 i = 0; i < n; i++)
                v.push_back({bef(&d[p + 4 + i * 12]), bef(&d[p + 8 + i * 12]), bef(&d[p + 12 + i * 12])});
            p += 4 + (size_t)n * 12;
        } else if (id == 24) {  // uvs
            need(d, p, 4);
            const u32 n = be32(&d[p]);
            need(d, p + 4, (size_t)n * 8);
            auto& v = cur_model().uvs;
            for (u32 i = 0; i < n; i++) v.push_back({bef(&d[p + 4 + i * 8]), bef(&d[p + 8 + i * 8])});
            p += 4 + (size_t)n * 8;
        } else if (id == 53) {  // faces: 3 vertex indices, u16 smoothing, u8 flags
            need(d, p, 4);
            const u32 n = be32(&d[p]);
            need(d, p + 4, (size_t)n * 9);
            auto& f = cur_model().faces;
            for (u32 i = 0; i < n; i++) {
                const u8* q = &d[p + 4 + i * 9];
                f.push_back({be16(q), be16(q + 2), be16(q + 4)});
            }
            p += 4 + (size_t)n * 9;
        } else if (id == 22) {  // material names
            need(d, p, 4);
            const u32 n = be32(&d[p]);
            p += 4;
            for (u32 i = 0; i < n; i++) cur_model().materials.push_back(cstr(d, p));
        } else if (id == 26) {  // material index per face
            need(d, p, 8);
            const u32 n = be32(&d[p]), per = be32(&d[p + 4]);
            need(d, p + 8, (size_t)n * 2);
            auto& fm = cur_model().face_mats;
            for (u32 i = 0; i < n; i++) fm.push_back(be16(&d[p + 8 + i * 2]));
            p += 8 + (size_t)n * per;
        } else if (id != 0) {
            throw std::runtime_error("unknown DAT chunk " + std::to_string(id) + " in " + path.string());
        }
    }
    return models;
}

std::vector<BrActor> read_act(const fs::path& path) {
    const Bytes d = read_file(path);
    std::vector<BrActor> stack, roots;
    auto top = [&]() -> BrActor& {
        if (stack.empty()) throw std::runtime_error("bad actor file " + path.string());
        return stack.back();
    };
    size_t p = 0;
    while (p + 8 <= d.size()) {
        const u32 id = be32(&d[p]), len = be32(&d[p + 4]);
        p += 8;
        if (id == kFileHeader) {
            p += len;
        } else if (id == 35) {  // actor: u8 type, u8 render style, name
            p += 2;
            BrActor a;
            a.name = cstr(d, p);
            stack.push_back(std::move(a));
        } else if (id == 43) {  // transform
            need(d, p, 48);
            for (int i = 0; i < 12; i++) top().m[i] = bef(&d[p + i * 4]);
            p += 48;
        } else if (id == 36) {
            top().model = cstr(d, p);
        } else if (id == 38) {
            top().material = cstr(d, p);
        } else if (id == 42) {  // add child: pop the top actor into the one below
            if (stack.size() < 2) throw std::runtime_error("bad actor tree in " + path.string());
            BrActor child = std::move(stack.back());
            stack.pop_back();
            stack.back().children.push_back(std::move(child));
        } else if (id == 37 || id == 41) {
        } else if (id == 50) {
            p += 24;
        } else if (id == 0) {
            for (auto& a : stack) roots.push_back(std::move(a));
            stack.clear();
        } else {
            throw std::runtime_error("unknown ACT chunk " + std::to_string(id) + " in " + path.string());
        }
    }
    for (auto& a : stack) roots.push_back(std::move(a));
    return roots;
}

std::map<std::string, BrMaterial> read_mat(const fs::path& path) {
    const Bytes d = read_file(path);
    std::map<std::string, BrMaterial> mats;
    BrMaterial* cur = nullptr;
    size_t p = 0;
    while (p + 8 <= d.size()) {
        const u32 id = be32(&d[p]), len = be32(&d[p + 4]);
        p += 8;
        if (id == kFileHeader) {
            p += len;
        } else if (id == 0x04) {  // C1: colour, 4 lighting floats, u16 flags, 2x3 UV matrix, index base/range, name
            need(d, p, 48);
            BrMaterial m;
            memcpy(m.colour.data(), &d[p], 4);
            m.flags = be16(&d[p + 20]);
            p += 48;
            m.name = cstr(d, p);
            cur = &(mats[upper(m.name)] = m);
        } else if (id == 0x3C) {  // C2-style material
            need(d, p, 65);
            BrMaterial m;
            memcpy(m.colour.data(), &d[p], 4);
            m.flags = be32(&d[p + 20]);
            p += 65;
            m.name = cstr(d, p);
            cur = &(mats[upper(m.name)] = m);
        } else if (id == 0x1C) {  // colour map
            std::string t = cstr(d, p);
            if (cur) cur->texture = t;
        } else if (id == 0x1F) {  // shade table
            cstr(d, p);
        } else if (id != 0) {
            throw std::runtime_error("unknown MAT chunk " + std::to_string(id) + " in " + path.string());
        }
    }
    return mats;
}

// A pixelmap may carry its own palette as a nested 1x256 pixelmap (header 0x03 type 7, its data 0x21,
// then 0x22) before its own pixel data (0x21).
std::map<std::string, BrPixmap> read_all_pix(const fs::path& path) {
    const Bytes d = read_file(path);
    std::map<std::string, BrPixmap> res;
    bool have_cur = false, have_child = false;
    std::string cur_name;
    int cur_type = 0, child_type = 0;
    BrPixmap cur;
    size_t p = 0;
    while (p + 8 <= d.size()) {
        const u32 id = be32(&d[p]), len = be32(&d[p + 4]);
        if (p + 8 + len > d.size()) break;
        const u8* b = &d[p + 8];
        p += 8 + len;
        if ((id == 0x03 || id == 0x3D) && len >= 11) {
            const int type = b[0];
            if (!have_cur) {
                have_cur = true;
                cur = BrPixmap();
                cur_type = type;
                cur.row = be16(b + 1);
                cur.w = be16(b + 3);
                cur.h = be16(b + 5);
                size_t e = 11;
                while (e < len && b[e]) e++;
                cur_name = upper(std::string((const char*)b + 11, e - 11));
            } else {
                have_child = true;
                child_type = type;
            }
        } else if (id == 0x21 && have_cur && len >= 8) {
            const u32 count = be32(b), esize = be32(b + 4);
            const size_t n = std::min((size_t)count * esize, (size_t)len - 8);
            if (have_child) {
                if (child_type == 7 && esize == 4) {
                    cur.palette.clear();
                    for (u32 i = 0; i < std::min<u32>(256, count) && i * 4 + 3 < n; i++)
                        cur.palette.push_back({b[8 + i * 4 + 1], b[8 + i * 4 + 2], b[8 + i * 4 + 3]});
                }
            } else {
                if (cur_type == 3) {
                    cur.px.assign(b + 8, b + 8 + n);
                    res[cur_name] = std::move(cur);
                }
                have_cur = false;
            }
        } else if (id == 0x22) {
            have_child = false;
        } else if (id == 0) {
            have_cur = have_child = false;
        }
    }
    return res;
}

std::vector<RGB> read_palette(const fs::path& path) {
    const Bytes d = read_file(path);
    bool header = false;
    size_t p = 0;
    while (p + 8 <= d.size()) {
        const u32 id = be32(&d[p]), len = be32(&d[p + 4]);
        if (p + 8 + len > d.size()) break;
        const u8* b = &d[p + 8];
        p += 8 + len;
        if ((id == 0x03 || id == 0x3D) && !header) {
            header = true;
        } else if (id == 0x21 && header && len >= 8 + 1024) {
            std::vector<RGB> pal;
            for (int i = 0; i < 256; i++) pal.push_back({b[8 + i * 4 + 1], b[8 + i * 4 + 2], b[8 + i * 4 + 3]});
            return pal;
        }
    }
    throw std::runtime_error("no palette in " + path.string());
}

// ---------------------------------------------------------------------------------------------
// FLI/FLC: the first frame.
// ---------------------------------------------------------------------------------------------
FliFrame fli_first_frame(const fs::path& path) {
    const Bytes d = read_file(path);
    auto u16at = [&](size_t p) { need(d, p, 2); return (u16)(d[p] | d[p + 1] << 8); };
    auto u32at = [&](size_t p) { need(d, p, 4); u32 v; memcpy(&v, &d[p], 4); return v; };
    auto s8at = [&](size_t p) { need(d, p, 1); return (int)(int8_t)d[p]; };
    const u16 magic = u16at(4);
    if (magic != 0xAF11 && magic != 0xAF12) throw std::runtime_error("not an FLI/FLC file: " + path.string());
    FliFrame f;
    f.w = u16at(8);
    f.h = u16at(10);
    const int w = f.w, h = f.h;
    f.px.assign((size_t)w * h, 0);
    size_t p = (magic == 0xAF12 && u32at(80)) ? u32at(80) : 128;
    u32 fsize = u32at(p);
    if (u16at(p + 4) == 0xF100) {  // prefix chunk
        p += fsize;
        fsize = u32at(p);
    }
    auto put = [&](size_t at, u8 v) { if (at < f.px.size()) f.px[at] = v; };
    const u16 nchunks = u16at(p + 6);
    size_t c = p + 16;
    for (u16 ci = 0; ci < nchunks; ci++) {
        const u32 csize = u32at(c);
        const u16 ctype = u16at(c + 4);
        size_t b = c + 6;
        if (ctype == 4 || ctype == 11) {  // colour 256 / colour 64
            const u16 npk = u16at(b);
            b += 2;
            int idx = 0;
            for (u16 k = 0; k < npk; k++) {
                need(d, b, 2);
                idx += d[b];
                const int n = d[b + 1] ? d[b + 1] : 256;
                b += 2;
                for (int j = 0; j < n; j++) {
                    need(d, b, 3);
                    int r = d[b], g = d[b + 1], bl = d[b + 2];
                    if (ctype == 11) r = r * 255 / 63, g = g * 255 / 63, bl = bl * 255 / 63;
                    if (idx + j < 256) f.pal[idx + j] = {(u8)r, (u8)g, (u8)bl};
                    b += 3;
                }
                idx += n;
            }
        } else if (ctype == 15) {  // byte run
            for (int y = 0; y < h; y++) {
                b += 1;
                int x = 0;
                while (x < w) {
                    int cnt = s8at(b);
                    b += 1;
                    if (cnt > 0) {
                        need(d, b, 1);
                        for (int k = 0; k < cnt; k++) put((size_t)y * w + x + k, d[b]);
                        b += 1;
                    } else {
                        cnt = -cnt;
                        need(d, b, cnt);
                        for (int k = 0; k < cnt; k++) put((size_t)y * w + x + k, d[b + k]);
                        b += cnt;
                    }
                    x += cnt;
                    if (cnt == 0) break;
                }
            }
        } else if (ctype == 12) {  // FLI delta
            const int y0 = u16at(b), nlines = u16at(b + 2);
            b += 4;
            for (int y = y0; y < y0 + nlines; y++) {
                need(d, b, 1);
                const int npk = d[b];
                b += 1;
                int x = 0;
                for (int k = 0; k < npk; k++) {
                    need(d, b, 2);
                    x += d[b];
                    int cnt = s8at(b + 1);
                    b += 2;
                    if (cnt > 0) {
                        need(d, b, cnt);
                        for (int j = 0; j < cnt; j++) put((size_t)y * w + x + j, d[b + j]);
                        b += cnt;
                        x += cnt;
                    } else if (cnt < 0) {
                        cnt = -cnt;
                        need(d, b, 1);
                        for (int j = 0; j < cnt; j++) put((size_t)y * w + x + j, d[b]);
                        b += 1;
                        x += cnt;
                    }
                }
            }
        } else if (ctype == 7) {  // FLC delta (words)
            const int nlines = u16at(b);
            b += 2;
            int y = 0;
            for (int li = 0; li < nlines; li++) {
                u16 op;
                for (;;) {
                    op = u16at(b);
                    b += 2;
                    if ((op & 0xC000) == 0xC000) y += 0x10000 - op;
                    else if ((op & 0xC000) == 0x8000) put((size_t)y * w + w - 1, (u8)(op & 0xFF));
                    else break;
                }
                int x = 0;
                for (int k = 0; k < op; k++) {
                    need(d, b, 2);
                    x += d[b];
                    int cnt = s8at(b + 1);
                    b += 2;
                    if (cnt > 0) {
                        need(d, b, (size_t)cnt * 2);
                        for (int j = 0; j < cnt * 2; j++) put((size_t)y * w + x + j, d[b + j]);
                        b += (size_t)cnt * 2;
                        x += cnt * 2;
                    } else if (cnt < 0) {
                        cnt = -cnt;
                        need(d, b, 2);
                        for (int j = 0; j < cnt; j++) {
                            put((size_t)y * w + x + j * 2, d[b]);
                            put((size_t)y * w + x + j * 2 + 1, d[b + 1]);
                        }
                        b += 2;
                        x += cnt * 2;
                    }
                }
                y++;
            }
        } else if (ctype == 13) {
            std::fill(f.px.begin(), f.px.end(), 0);
        } else if (ctype == 16) {
            need(d, b, (size_t)w * h);
            f.px.assign(d.begin() + b, d.begin() + b + (size_t)w * h);
        }
        c += csize;
    }
    return f;
}

// ---------------------------------------------------------------------------------------------
// IMG v1.0. Format 6: planes A, R, G, B, RLE: a count byte with the high bit set is a literal run of
// (count & 0x7f) bytes, otherwise the next byte is repeated count times.
// ---------------------------------------------------------------------------------------------
namespace {
Bytes rle(const Bytes& plane) {
    Bytes out;
    const size_t n = plane.size();
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 127 && plane[i + run] == plane[i]) run++;
        if (run >= 3) {
            out.push_back((u8)run);
            out.push_back(plane[i]);
            i += run;
            continue;
        }
        size_t j = i;
        while (j < n && j - i < 127 && !(j + 2 < n && plane[j] == plane[j + 1] && plane[j + 1] == plane[j + 2])) j++;
        out.push_back((u8)(0x80 | (j - i)));
        out.insert(out.end(), plane.begin() + i, plane.begin() + j);
        i = j;
    }
    return out;
}

Bytes unrle(const u8* raw, size_t len, size_t n) {
    Bytes out;
    size_t i = 0;
    while (i < len && out.size() < n) {
        const u8 c = raw[i++];
        if (c & 0x80) {
            const size_t k = std::min<size_t>(c & 0x7f, len - i);
            out.insert(out.end(), raw + i, raw + i + k);
            i += k;
        } else if (i < len) {
            out.insert(out.end(), c, raw[i++]);
        }
    }
    out.resize(n);
    return out;
}

void img_header(Writer& w, u8 basic, u32 fmt, u32 size, int width, int height) {
    w.raw("IMAGEMAP", 8);
    const u8 ver[4] = {0, 1, basic, 0};
    w.raw(ver, 4);
    w.u32_(fmt);
    w.u32_(size);
    w.u16_((u16)width);
    w.u16_((u16)height);
}
}  // namespace

Bytes img_rle(const Rgba& img, u8 basic) {
    const size_t n = (size_t)img.w * img.h;
    Bytes planes[4];
    for (int k = 0; k < 4; k++) planes[k].resize(n);
    for (size_t i = 0; i < n; i++) {
        planes[0][i] = img.px[i * 4 + 3];
        planes[1][i] = img.px[i * 4];
        planes[2][i] = img.px[i * 4 + 1];
        planes[3][i] = img.px[i * 4 + 2];
    }
    Bytes data[4];
    u32 total = 16;
    for (int k = 0; k < 4; k++) total += (u32)(data[k] = rle(planes[k])).size();
    Writer w;
    img_header(w, basic, 6, total, img.w, img.h);
    for (int k = 0; k < 4; k++) w.u32_((u32)data[k].size());
    for (int k = 0; k < 4; k++) w.raw(data[k]);
    return w.b;
}

Bytes img_plain(const Rgba& img) {
    const size_t n = (size_t)img.w * img.h;
    Writer w;
    img_header(w, 0, 0, (u32)(n * 4), img.w, img.h);
    for (size_t i = 0; i < n; i++) {
        const u8 px[4] = {img.px[i * 4 + 3], img.px[i * 4], img.px[i * 4 + 1], img.px[i * 4 + 2]};
        w.raw(px, 4);
    }
    return w.b;
}

Rgba read_img(const Bytes& d) {
    if (d.size() < 24 || memcmp(d.data(), "IMAGEMAP", 8)) throw std::runtime_error("not an IMG");
    u32 fmt;
    u16 w, h;
    memcpy(&fmt, &d[12], 4);
    memcpy(&w, &d[20], 2);
    memcpy(&h, &d[22], 2);
    Rgba img(w, h);
    const size_t n = (size_t)w * h;
    if (fmt == 6) {
        need(d, 24, 16);
        u32 sizes[4];
        memcpy(sizes, &d[24], 16);
        size_t p = 40;
        const int order[4] = {3, 0, 1, 2};  // A, R, G, B -> rgba offsets
        for (int k = 0; k < 4; k++) {
            need(d, p, sizes[k]);
            const Bytes plane = unrle(&d[p], sizes[k], n);
            for (size_t i = 0; i < n; i++) img.px[i * 4 + order[k]] = plane[i];
            p += sizes[k];
        }
    } else {
        need(d, 24, n * 4);
        for (size_t i = 0; i < n; i++) {
            const u8* s = &d[24 + i * 4];
            img.px[i * 4] = s[1];
            img.px[i * 4 + 1] = s[2];
            img.px[i * 4 + 2] = s[3];
            img.px[i * 4 + 3] = s[0];
        }
    }
    return img;
}

Rgba resize(const Rgba& img, int nw, int nh) {
    Rgba out(nw, nh);
    const double sx = (double)img.w / nw, sy = (double)img.h / nh;
    for (int y = 0; y < nh; y++) {
        const int y0 = (int)(y * sy), y1 = std::max((int)(y * sy) + 1, (int)((y + 1) * sy));
        for (int x = 0; x < nw; x++) {
            const int x0 = (int)(x * sx), x1 = std::max((int)(x * sx) + 1, (int)((x + 1) * sx));
            long long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = y0; yy < std::min(y1, img.h); yy++)
                for (int xx = x0; xx < std::min(x1, img.w); xx++) {
                    const u8* s = &img.px[((size_t)yy * img.w + xx) * 4];
                    r += s[0] * s[3];
                    g += s[1] * s[3];
                    b += s[2] * s[3];
                    a += s[3];
                    cnt++;
                }
            if (a) {
                u8* o = &out.px[((size_t)y * nw + x) * 4];
                o[0] = (u8)(r / a);
                o[1] = (u8)(g / a);
                o[2] = (u8)(b / a);
                o[3] = (u8)(a / cnt);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// CNT v4.0: "E#" 00 04, then a recursive node: name (pstr), flag bytes until 0, u32 0, 12 floats,
// 4-char type, type data, u32 child count, children, lumps (u32 id ... until 0).
// ---------------------------------------------------------------------------------------------
namespace {
void write_node(Writer& w, const CntNode& n) {
    w.pstr(n.name);
    w.u8_(0);
    w.u32_(0);
    w.fs(n.m);
    w.raw(n.kind.data(), 4);
    if (n.kind == "MODL" || n.kind == "SKIN") w.pstr(n.model);
    w.u32_((u32)n.children.size());
    for (const auto& c : n.children) write_node(w, c);
    w.u32_(0);
}

CntNode read_node(Reader& r) {
    CntNode n;
    n.name = r.pstr();
    while (r.u8_() != 0) {}
    if (r.u32_() != 0) throw std::runtime_error("CNT: expected 0 after flags");
    for (int i = 0; i < 12; i++) n.m[i] = r.f32();
    n.kind = std::string((const char*)r.take(4), 4);
    if (n.kind == "MODL" || n.kind == "SKIN") n.model = r.pstr();
    else if (n.kind != "NULL") throw std::runtime_error("CNT: node type " + n.kind + " not supported");
    const u32 nc = r.u32_();
    for (u32 i = 0; i < nc; i++) n.children.push_back(read_node(r));
    if (r.u32_() != 0) throw std::runtime_error("CNT: lumps not supported");
    return n;
}
}  // namespace

Bytes write_cnt(const CntNode& root) {
    Writer w;
    w.raw("E#\x00\x04", 4);
    write_node(w, root);
    return w.b;
}

CntNode read_cnt(const Bytes& data) {
    Reader r(data);
    const u8* h = r.take(4);
    if (h[0] != 'E' || h[1] != '#' || h[2] != 0 || h[3] != 4) throw std::runtime_error("not a CNT v4.0");
    return read_node(r);
}

// ---------------------------------------------------------------------------------------------
// MDL v6.2 ("E#" 02 06). The header field after the flags is the file length - 28.
// ---------------------------------------------------------------------------------------------
Bytes write_mdl(const Mdl& m) {
    Writer prep;
    prep.u32_((u32)m.faces.size());
    for (const auto& f : m.faces) {
        prep.u16_((u16)f[0]);
        prep.u16_((u16)f[1]);
        prep.u32_(f[2]);
        prep.u32_(f[3]);
        prep.u32_(f[4]);
    }
    prep.u32_((u32)m.verts.size());
    for (const auto& v : m.verts) {
        prep.raw(v.f, 40);
        prep.raw(v.c, 4);
    }
    prep.u16_((u16)m.groups.size());
    for (const auto& g : m.groups) {
        prep.fs(g.centre);
        prep.f32(g.radius);
        prep.fs(g.mn);
        prep.fs(g.mx);
        prep.u32_(g.strip_off);
        prep.u32_(g.strip_count);
        prep.u32_((u32)g.strip.size());
        for (u32 i : g.strip) prep.u32_(i);
        prep.u32_(g.list_off);
        prep.u32_(g.list_count);
        prep.u32_((u32)g.list.size());
        for (u32 i : g.list) prep.u32_(i);
    }
    Writer w;
    w.raw("E#\x02\x06", 4);
    w.u32_(m.checksum);
    w.u32_(m.flags);
    const size_t size_at = w.b.size();
    w.u32_(0);
    w.u32_(m.user_faces);
    w.u32_(m.user_verts);
    w.u32_(m.unknown);
    w.f32(m.radius);
    w.fs(m.bmin);
    w.fs(m.bmax);
    w.fs(m.centre);
    w.u16_((u16)m.materials.size());
    for (const auto& name : m.materials) w.pstr(name, 4);
    w.raw(prep.b);
    w.raw(m.tail);
    const u32 size = (u32)(w.b.size() - 28);
    memcpy(&w.b[size_at], &size, 4);
    return w.b;
}

Mdl read_mdl(const Bytes& data) {
    Reader r(data);
    const u8* h = r.take(4);
    if (h[0] != 'E' || h[1] != '#' || h[2] != 2 || h[3] != 6) throw std::runtime_error("not an MDL v6.2");
    Mdl m;
    m.checksum = r.u32_();
    m.flags = r.u32_();
    r.u32_();  // file size
    m.user_faces = r.u32_();
    m.user_verts = r.u32_();
    m.unknown = r.u32_();
    m.radius = r.f32();
    for (auto* a : {&m.bmin, &m.bmax, &m.centre})
        for (double& v : *a) v = r.f32();
    const u16 nm = r.u16_();
    for (u16 i = 0; i < nm; i++) m.materials.push_back(r.pstr(4));
    const u32 nf = r.u32_();
    for (u32 i = 0; i < nf; i++) {
        std::array<u32, 5> f;
        f[0] = r.u16_();
        f[1] = r.u16_();
        f[2] = r.u32_();
        f[3] = r.u32_();
        f[4] = r.u32_();
        m.faces.push_back(f);
    }
    const u32 nv = r.u32_();
    m.verts.resize(nv);
    for (u32 i = 0; i < nv; i++) memcpy(&m.verts[i], r.take(44), 44);
    const u16 ng = r.u16_();
    for (u16 i = 0; i < ng; i++) {
        MdlGroup g;
        for (double& v : g.centre) v = r.f32();
        g.radius = r.f32();
        for (double& v : g.mn) v = r.f32();
        for (double& v : g.mx) v = r.f32();
        g.strip_off = r.u32_();
        g.strip_count = r.u32_();
        for (u32 k = r.u32_(); k; k--) g.strip.push_back(r.u32_());
        g.list_off = r.u32_();
        g.list_count = r.u32_();
        for (u32 k = r.u32_(); k; k--) g.list.push_back(r.u32_());
        m.groups.push_back(std::move(g));
    }
    m.tail.assign(data.begin() + r.p, data.end());
    return m;
}

// MTL: 2 header bytes, u32 texture count, texture names (pstr), then the material settings.
Bytes mtl_with_texture(const Bytes& tmpl, const std::string& texture) {
    Reader r(tmpl);
    const u8* head = r.take(2);
    if (r.u32_() != 1) throw std::runtime_error("the template material must have one texture");
    r.pstr();
    Writer w;
    w.raw(head, 2);
    w.u32_(1);
    w.pstr(texture);
    w.raw(tmpl.data() + r.p, tmpl.size() - r.p);
    return w.b;
}

std::string mtl_texture(const Bytes& mtl) {
    try {
        Reader r(mtl);
        r.take(2);
        if (r.u32_() < 1) return "";
        return r.pstr();
    } catch (const std::exception&) {
        return "";
    }
}

// ---------------------------------------------------------------------------------------------
// Lua 5.1 bytecode (size_t = 4, as on ARM)
// ---------------------------------------------------------------------------------------------
namespace {
struct LuaReader {
    const Bytes& d;
    size_t p = 0;
    const u8* take(size_t n) { need(d, p, n); const u8* r = &d[p]; p += n; return r; }
    u8 u8_() { return *take(1); }
    int32_t i32() { int32_t v; memcpy(&v, take(4), 4); return v; }
    std::string str(bool* is_null = nullptr) {
        const int32_t n = i32();
        if (is_null) *is_null = n == 0;
        if (n <= 0) return "";
        const u8* s = take((size_t)n);
        return std::string((const char*)s, (size_t)n - 1);
    }
};

void read_function(LuaReader& r, std::vector<LuaFunc>& out) {
    r.str();
    r.i32();
    r.i32();
    r.take(4);  // upvalues, params, vararg, max stack
    LuaFunc f;
    f.ncode = (u32)r.i32();
    f.code_at = r.p;
    r.take((size_t)f.ncode * 4);
    f.k_start = r.p;
    const int32_t nk = r.i32();
    for (int32_t i = 0; i < nk; i++) {
        LuaConst c;
        const u8 t = r.u8_();
        if (t == 0) c.type = LuaConst::Nil;
        else if (t == 1) c.type = LuaConst::Bool, c.num = r.u8_();
        else if (t == 3) { c.type = LuaConst::Num; memcpy(&c.num, r.take(8), 8); }
        else if (t == 4) c.type = LuaConst::Str, c.str = r.str(&c.str_null);
        else throw std::runtime_error("Lua: constant type " + std::to_string(t));
        f.k.push_back(std::move(c));
    }
    f.k_end = r.p;
    const size_t index = out.size();
    out.push_back(std::move(f));
    const int32_t np = r.i32();
    for (int32_t i = 0; i < np; i++) read_function(r, out);
    for (int32_t i = r.i32(); i > 0; i--) r.i32();  // line info
    for (int32_t i = r.i32(); i > 0; i--) { r.str(); r.i32(); r.i32(); }  // locals
    for (int32_t i = r.i32(); i > 0; i--) r.str();  // upvalue names
    (void)index;
}
}  // namespace

std::vector<LuaFunc> lua_load(const Bytes& data) {
    if (data.size() < 12 || memcmp(data.data(), "\x1bLua", 4) || data[4] != 0x51)
        throw std::runtime_error("not Lua 5.1 bytecode");
    LuaReader r{data, 12};
    std::vector<LuaFunc> funcs;
    read_function(r, funcs);
    return funcs;
}

Bytes lua_const_section(const std::vector<LuaConst>& k) {
    Writer w;
    w.u32_((u32)k.size());
    for (const auto& c : k) {
        w.u8_((u8)c.type);
        if (c.type == LuaConst::Bool) w.u8_((u8)c.num);
        else if (c.type == LuaConst::Num) w.f64(c.num);
        else if (c.type == LuaConst::Str) {
            if (c.str_null) {
                w.u32_(0);
            } else {
                w.u32_((u32)c.str.size() + 1);
                w.raw(c.str.data(), c.str.size());
                w.u8_(0);
            }
        }
    }
    return w.b;
}

}  // namespace pcimport
