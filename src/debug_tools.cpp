// Debug helpers that run inside the game thread (called once per frame from eglSwapBuffers).
#include "debug_tools.h"
#include "elf_loader.h"
#include "hle/hle_common.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace debug {

std::string g_dump_wad_dir;

static void dump_wads(Cpu& c) {
    const u32 get_first_wad = loader::find_symbol("_Z18bz_WAD_GetFirstWADj");
    const u32 get_next_wad = loader::find_symbol("_Z17bz_WAD_GetNextWADj");
    const u32 get_first = loader::find_symbol("_Z20bz_WAD_GetFirstEntryjP11_bzWADEntry");
    const u32 get_next = loader::find_symbol("_Z19bz_WAD_GetNextEntryjP11_bzWADEntry");
    const u32 get_data = loader::find_symbol("_Z14bz_WAD_GetDatajjPh");
    u32 entry = mem::calloc(1, 0x200);
    int wad_no = 0, files = 0;
    (void)get_first_wad; (void)get_next_wad;
    const u32 parse = loader::find_symbol("_Z17_WAD_ParseWADFilePKcb");
    u32 name = mem::strdup("DATA_ANDROID.WAD");
    u32 details = c.call(parse, {name, 0});
    LOGI("_WAD_ParseWADFile -> 0x%x", details);
    for (u32 wad = details; wad; wad = 0) {
        int guard = 0;
        for (u32 ok = c.call(get_first, {wad, entry}); ok && guard < 200000; ok = c.call(get_next, {wad, entry}), guard++) {
            std::string path = mem::str(entry);
            u32 size = mem::r32(entry + 0x104), index = mem::r32(entry + 0x10c);
            for (auto& ch : path) if (ch == '\\') ch = '/';
            fs::path out = fs::path(g_dump_wad_dir) / ("wad" + std::to_string(wad_no)) / path;
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            u32 buf = mem::malloc(size + 16);
            c.call(get_data, {wad, index, buf});
            if (FILE* f = fopen(out.string().c_str(), "wb")) {
                fwrite(mem::ptr(buf), 1, size, f);
                fclose(f);
            }
            mem::free(buf);
            files++;
        }
        wad_no++;
    }
    mem::free(entry);
    LOGI("WAD dump: %d files from %d WAD(s) written to %s", files, wad_no, g_dump_wad_dir.c_str());
}

bool g_dump_zones = false;

static void dump_zones(Cpu& c) {
    const u32 input = 0x1084A53C;
    LOGI("race state: localHuman=0x%x gPaused=%u", mem::r32(0x108C1FAC), mem::r8(loader::find_symbol("gPaused")));
    {
        u32 st = loader::find_symbol("gState"), pz = loader::find_symbol("gPaused");
        std::string l;
        char b[16];
        for (int k = 0; k < 9; k++) { snprintf(b, sizeof b, "%x ", mem::r32(st + k * 4)); l += b; }
        LOGI("gState: %s gPaused=%u", l.c_str(), mem::r8(pz));
    }
    LOGI("modes: steer=%u throttle=%u  sens: steer=%f brake=%f", mem::r32(input + 0x5f4), mem::r32(input + 0x5f0),
         *mem::ptr<float>(0x1067CBCC), *mem::ptr<float>(0x1067CBC8));
    for (int z = 0; z < 11; z++) {
        u32 b = input + z * 88;
        std::string line;
        char buf[64];
        for (int k = 0; k < 22; k++) {
            u32 v = mem::r32(b + k * 4);
            float f;
            memcpy(&f, &v, 4);
            if (v == 0) snprintf(buf, sizeof buf, "0 ");
            else if (f > -100000 && f < 100000 && (v & 0x7f800000)) snprintf(buf, sizeof buf, "%.2f ", f);
            else snprintf(buf, sizeof buf, "%08x ", v);
            line += buf;
        }
        LOGI("zone %2d: %s", z, line.c_str());
    }
    u32 out = mem::calloc(4, 4);
    c.call(loader::find_symbol("_Z20Input_ReadTiltValuesRfS_S_S_"), {out, out + 4, out + 8, out + 12});
    float* o = mem::ptr<float>(out);
    LOGI("tilt: steer=%f throttle=%f roll=%f pitch=%f", o[0], o[1], o[2], o[3]);
    mem::free(out);
}

std::string g_dump_file;  // --dump-file NAME: read a file through the engine's file system and save it

static void dump_file(Cpu& c) {
    u32 name = mem::strdup(g_dump_file.c_str()), mode = mem::strdup("rb");
    u32 f = c.call(loader::find_symbol("_Z12bz_File_OpenPKcS0_"), {name, mode});
    if (!f) { LOGE("dump-file: cannot open %s", g_dump_file.c_str()); return; }
    std::string out = hle::g_config.root + "/extracted/" + g_dump_file;
    for (auto& ch : out) if (ch == '\\') ch = '/';
    std::error_code ec;
    fs::create_directories(fs::path(out).parent_path(), ec);
    FILE* o = fopen(out.c_str(), "wb");
    u32 buf = mem::malloc(65536), total = 0;
    for (;;) {
        u32 n = c.call(loader::find_symbol("_Z12bz_File_ReadP6bzFilePvjb"), {f, buf, 65536, 0});
        if (!n || n > 65536) break;
        fwrite(mem::ptr(buf), 1, n, o);
        total += n;
    }
    fclose(o);
    c.call(loader::find_symbol("_Z13bz_File_CloseP6bzFile"), {f});
    LOGI("dump-file: %s -> %s (%u bytes)", g_dump_file.c_str(), out.c_str(), total);
}

void on_frame(Cpu& c) {
    static int frame = 0;
    frame++;
    if (g_dump_zones && frame % 300 == 0) dump_zones(c);
    if (!g_dump_file.empty() && frame == 300) {
        std::string all = g_dump_file;  // comma-separated list
        size_t p = 0;
        while (p <= all.size()) {
            size_t q = all.find(',', p);
            if (q == std::string::npos) q = all.size();
            g_dump_file = all.substr(p, q - p);
            if (!g_dump_file.empty()) dump_file(c);
            p = q + 1;
        }
        g_dump_file.clear();
    }
    if (!g_dump_wad_dir.empty() && frame == 300) {
        dump_wads(c);
        g_dump_wad_dir.clear();
    }
}

}  // namespace debug
