#include "gamedata.h"
#include <miniz.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace gamedata {
namespace {

// ---------------------------------------------------------------------------------------------
// OBB container: u32 version (1), u32 count, then per file: u32 name length, name, u32 offset,
// u32 size, u32 (0xFFFFFFFF). The game's OBB holds a single file, DATA_ANDROID.WAD.
//
// Stainless WAD (v2.2, as read by ToxicRagers' sWAD.cs and the engine's _WAD_ParseWADFile):
//   u8 0x34, u8 0x12, u8 minor, u8 major, u32 flags, u32 xml length (0), u32 names length,
//   names (NUL-terminated, sorted), [flags & 0x200: u32 count, count * (u32, u32)],
//   u32 file count, u32 folder count, u32 offset count, offsets[] (from the WAD start),
//   then the tree from the root folder:
//     folder: u32 name offset, u32 files, u32 folders, u32 0, folders..., files...
//     file:   u32 name offset, u32 stored size, u32 index | 0x01000000, u32 0
//   Stored file data: i32 unpacked length (-1 = stored raw) then the raw bytes, or a zlib stream.
// ---------------------------------------------------------------------------------------------
constexpr u32 kFlagCompressed = 1u << 1, kFlagUnknown = 1u << 6, kFlagDataTimes = 1u << 9;
constexpr const char* kWadName = "DATA_ANDROID.WAD";

u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

bool write_file(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE* f = _wfopen(path.wstring().c_str(), L"wb");
    if (!f) return false;
    const bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

bool read_file(const fs::path& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool extract_wad(const u8* w, size_t size, const fs::path& out_dir, int& files, std::string& err) {
    if (size < 16 || w[0] != 0x34 || w[1] != 0x12) { err = "the OBB does not contain a Stainless WAD"; return false; }
    const u32 flags = rd32(w + 4), xml_len = rd32(w + 8), names_len = rd32(w + 12);
    if (xml_len != 0) { err = "unsupported WAD (embedded XML)"; return false; }
    const u8* names = w + 16;
    size_t p = 16 + names_len;
    auto need = [&](size_t n) { return p + n <= size; };
    if (flags & kFlagDataTimes) {
        if (!need(4)) return false;
        p += 4 + (size_t)rd32(w + p) * 8;
    }
    if (!need(12)) { err = "truncated WAD"; return false; }
    const u32 offset_count = rd32(w + p + 8);
    p += 12;
    if (!need((size_t)offset_count * 4)) { err = "truncated WAD"; return false; }
    const u8* offsets = w + p;
    p += (size_t)offset_count * 4;
    auto name_at = [&](u32 i) -> std::string {
        if (i >= names_len) return "?";
        return std::string((const char*)names + i, strnlen((const char*)names + i, names_len - i));
    };

    std::vector<u8> buf;
    // Recursive walk of the folder tree (iterative with an explicit stack of pending counts).
    struct Level { fs::path path; u32 folders_left, files_left; };
    std::vector<Level> stack;
    auto push_folder = [&](const fs::path& parent) -> bool {
        if (!need(16)) return false;
        const std::string name = name_at(rd32(w + p));
        const u32 nfiles = rd32(w + p + 4), nfolders = rd32(w + p + 8);
        p += 16;
        stack.push_back({parent / name, nfolders, nfiles});
        return true;
    };
    if (!push_folder(out_dir)) { err = "truncated WAD"; return false; }
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.folders_left) {
            top.folders_left--;
            const fs::path parent = top.path;
            if (!push_folder(parent)) { err = "truncated WAD"; return false; }
            continue;
        }
        if (top.files_left) {
            top.files_left--;
            if (!need(16)) { err = "truncated WAD"; return false; }
            const std::string name = name_at(rd32(w + p));
            const u32 stored = rd32(w + p + 4), index = rd32(w + p + 8) & 0xFFFFFF;
            p += 16;
            if (index >= offset_count) { err = "bad WAD file index"; return false; }
            const u32 off = rd32(offsets + index * 4);
            if ((size_t)off + stored > size || stored < 4) { err = "bad WAD entry " + name; return false; }
            const int32_t length = (int32_t)rd32(w + off);
            const fs::path out = top.path / name;
            bool ok;
            if (length == -1) {
                ok = write_file(out, w + off + 4, stored - 4);
            } else if (length == 0) {
                ok = write_file(out, nullptr, 0);
            } else {
                buf.resize((size_t)length);
                size_t got = tinfl_decompress_mem_to_mem(buf.data(), buf.size(), w + off + 4, stored - 4,
                                                         TINFL_FLAG_PARSE_ZLIB_HEADER);
                if (got != (size_t)length && stored > 6)  // raw deflate after the 2-byte zlib header
                    got = tinfl_decompress_mem_to_mem(buf.data(), buf.size(), w + off + 6, stored - 6, 0);
                if (got != (size_t)length) { err = "could not decompress " + name; return false; }
                ok = write_file(out, buf.data(), buf.size());
            }
            if (!ok) { err = "could not write " + out.string(); return false; }
            files++;
            continue;
        }
        stack.pop_back();
    }
    return true;
}

// Folder tree for packing (names upper-cased, as in the original WAD).
struct Node {
    std::map<std::string, std::unique_ptr<Node>> folders;
    std::map<std::string, fs::path> files;
};

std::string upper(std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

}  // namespace

bool extract(const std::string& apk_path, const std::string& obb_path, const std::string& out_dir, std::string& err) {
    const fs::path out = fs::u8path(out_dir);
    // APK: native libraries and assets.
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, apk_path.c_str(), 0)) { err = "could not open " + apk_path; return false; }
    int apk_files = 0;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        const std::string name = st.m_filename;
        if (name.rfind("lib/armeabi-v7a/", 0) != 0 && name.rfind("assets/", 0) != 0) continue;
        std::vector<u8> data((size_t)st.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0) ||
            !write_file(out / fs::u8path(name), data.data(), data.size())) {
            mz_zip_reader_end(&zip);
            err = "could not extract " + name;
            return false;
        }
        apk_files++;
    }
    mz_zip_reader_end(&zip);

    // OBB: the WAD inside it.
    std::vector<u8> obb;
    if (!read_file(fs::u8path(obb_path), obb)) { err = "could not read " + obb_path; return false; }
    if (obb.size() < 8 || rd32(obb.data()) != 1) { err = "unrecognised OBB file"; return false; }
    size_t p = 8;
    for (u32 k = 0, count = rd32(obb.data() + 4); k < count; k++) {
        if (p + 4 > obb.size()) break;
        const u32 len = rd32(obb.data() + p);
        if (p + 4 + len + 12 > obb.size()) break;
        const std::string name((const char*)obb.data() + p + 4, len);
        const u32 off = rd32(obb.data() + p + 4 + len), size = rd32(obb.data() + p + 8 + len);
        p += 4 + len + 12;
        if ((size_t)off + size > obb.size()) { err = "truncated OBB"; return false; }
        int files = 0;
        if (!extract_wad(obb.data() + off, size, out, files, err)) return false;
        LOGI("extracted %s: %d files", name.c_str(), files);
    }
    const char* readme =
        "Carmageddon game data unpacked by carmadroid.\n\n"
        "  lib/armeabi-v7a/  the game's native libraries\n"
        "  assets/           APK assets (music)\n"
        "  DATA/             the OBB contents (DATA_ANDROID.WAD)\n\n"
        "Run the game from this folder with:  carmadroid.exe --game-dir <this folder>\n"
        "Changed files under DATA are packed into a new OBB automatically at startup.\n";
    write_file(out / "README.txt", readme, strlen(readme));
    LOGI("extracted %d APK files to %s", apk_files, out_dir.c_str());
    return true;
}

bool build_obb(const std::string& dir, const std::string& obb_out, std::string& err) {
    const fs::path root = fs::u8path(dir) / "DATA";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { err = root.string() + " not found"; return false; }

    // Gather the files, and a signature of the folder to skip repacking when nothing changed.
    Node tree;
    u64 total = 0;
    size_t count = 0;
    std::string sig;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) { err = "could not scan " + root.string(); return false; }
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), root, ec);
        Node* node = &tree;
        std::vector<std::string> parts;
        for (const auto& part : rel) parts.push_back(upper(part.string()));
        for (size_t i = 0; i + 1 < parts.size(); i++) {
            auto& child = node->folders[parts[i]];
            if (!child) child = std::make_unique<Node>();
            node = child.get();
        }
        node->files[parts.back()] = it->path();
        const u64 size = it->file_size(ec);
        total += size;
        count++;
        sig += rel.string() + "|" + std::to_string(size) + "|" +
               std::to_string(it->last_write_time(ec).time_since_epoch().count()) + "\n";
    }
    char sig_hash[64];
    snprintf(sig_hash, sizeof sig_hash, "%zu files, %llu bytes, %08lx", count, (unsigned long long)total,
             (unsigned long)mz_crc32(MZ_CRC32_INIT, (const u8*)sig.data(), sig.size()));
    const fs::path out = fs::u8path(obb_out), sig_path = fs::u8path(obb_out + ".sig");
    {
        std::ifstream f(sig_path);
        std::string old;
        std::getline(f, old);
        if (old == sig_hash && fs::exists(out, ec)) {
            LOGI("game data: %s unchanged (%s)", dir.c_str(), sig_hash);
            return true;
        }
    }
    LOGI("game data: packing %s (%s)", dir.c_str(), sig_hash);

    // Name table: every folder and file name once, sorted.
    std::map<std::string, u32> name_offsets;
    std::vector<const Node*> todo = {&tree};
    name_offsets["DATA"] = 0;
    u32 files_total = 0, folders_total = 1;
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        for (auto& f : n->folders) { name_offsets[f.first] = 0; todo.push_back(f.second.get()); folders_total++; }
        for (auto& f : n->files) { name_offsets[f.first] = 0; files_total++; }
    }
    std::vector<u8> names;
    for (auto& e : name_offsets) {
        e.second = (u32)names.size();
        names.insert(names.end(), e.first.begin(), e.first.end());
        names.push_back(0);
    }
    while (names.size() % 4) names.push_back(0);

    // Tree records (folders, then files, both in reverse order as in the original), and the file
    // order that assigns data indices.
    std::vector<u8> table;
    std::vector<const fs::path*> order;
    auto put = [&](u32 v) { table.insert(table.end(), (u8*)&v, (u8*)&v + 4); };
    std::vector<size_t> size_slots;  // where each file's stored size goes
    std::function<void(const std::string&, const Node&)> folder = [&](const std::string& name, const Node& n) {
        put(name_offsets[name]);
        put((u32)n.files.size());
        put((u32)n.folders.size());
        put(0);
        for (auto it = n.folders.rbegin(); it != n.folders.rend(); ++it) folder(it->first, *it->second);
        for (auto it = n.files.rbegin(); it != n.files.rend(); ++it) {
            put(name_offsets[it->first]);
            size_slots.push_back(table.size());
            put(0);  // stored size, filled in below
            put((u32)order.size() | 0x01000000u);
            put(0);
            order.push_back(&it->second);
        }
    };
    folder("DATA", tree);

    const u32 flags = kFlagCompressed | kFlagUnknown | kFlagDataTimes;  // as the original (files stored raw)
    const size_t header = 16 + names.size() + 4 /*data times*/ + 12 + order.size() * 4 + table.size();
    const u32 wad_start = 0x28;
    fs::create_directories(out.parent_path(), ec);
    const fs::path tmp = fs::u8path(obb_out + ".tmp");
    FILE* f = _wfopen(tmp.wstring().c_str(), L"wb");
    if (!f) { err = "could not write " + tmp.string(); return false; }
    // Data first goes after the header; offsets are relative to the WAD start.
    std::vector<u32> offsets(order.size());
    u64 pos = header;
    std::vector<u32> stored(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        const u64 sz = fs::file_size(*order[i], ec);
        offsets[i] = (u32)pos;
        stored[i] = (u32)(sz + 4);
        pos += sz + 4;
    }
    if (pos + wad_start > 0xFFFFFFFFull) { fclose(f); err = "game data too large (over 4 GB)"; return false; }
    for (size_t i = 0; i < order.size(); i++) memcpy(&table[size_slots[i]], &stored[i], 4);

    auto w32 = [&](u32 v) { fwrite(&v, 4, 1, f); };
    // container
    w32(1); w32(1); w32((u32)strlen(kWadName)); fwrite(kWadName, 1, strlen(kWadName), f);
    w32(wad_start); w32((u32)pos); w32(0xFFFFFFFFu);
    // WAD header
    const u8 magic[4] = {0x34, 0x12, 2, 2};
    fwrite(magic, 1, 4, f);
    w32(flags); w32(0); w32((u32)names.size());
    fwrite(names.data(), 1, names.size(), f);
    w32(0);  // data times: none
    w32(files_total); w32(folders_total); w32((u32)offsets.size());
    fwrite(offsets.data(), 4, offsets.size(), f);
    fwrite(table.data(), 1, table.size(), f);
    std::vector<u8> data;
    for (size_t i = 0; i < order.size(); i++) {
        if (!read_file(*order[i], data)) { fclose(f); err = "could not read " + order[i]->string(); return false; }
        w32(0xFFFFFFFFu);  // stored raw
        if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    }
    const bool ok = ferror(f) == 0;
    if (fclose(f) != 0 || !ok) { err = "could not write " + tmp.string(); return false; }
    fs::rename(tmp, out, ec);
    if (ec) { err = "could not replace " + out.string(); return false; }
    std::ofstream(sig_path) << sig_hash << "\n";
    LOGI("game data: packed %u files into %s", files_total, obb_out.c_str());
    return true;
}

}  // namespace gamedata
