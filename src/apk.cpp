#include "apk.h"
#include <miniz.h>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace apk {
namespace {
mz_zip_archive g_zip{};
bool g_open = false;
std::string g_dir;  // non-empty: read from this folder instead of the zip
std::mutex g_lock;  // miniz archives are not thread-safe

fs::path dir_path(const std::string& name) { return fs::u8path(g_dir) / fs::u8path(name); }
}  // namespace

bool open_dir(const std::string& dir) {
    std::lock_guard<std::mutex> l(g_lock);
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) return false;
    g_dir = dir;
    return true;
}

bool open(const std::string& path) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_open) mz_zip_reader_end(&g_zip);
    g_zip = {};
    g_open = mz_zip_reader_init_file(&g_zip, path.c_str(), 0) != 0;
    return g_open;
}

bool read(const std::string& name, std::vector<u8>& out) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_dir.empty()) {
        std::ifstream f(dir_path(name), std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
    if (!g_open) return false;
    int idx = mz_zip_reader_locate_file(&g_zip, name.c_str(), nullptr, 0);
    if (idx < 0) return false;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&g_zip, (mz_uint)idx, &st)) return false;
    out.resize((size_t)st.m_uncomp_size);
    return mz_zip_reader_extract_to_mem(&g_zip, (mz_uint)idx, out.data(), out.size(), 0) != 0;
}

bool exists(const std::string& name) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_dir.empty()) {
        std::error_code ec;
        return fs::is_regular_file(dir_path(name), ec);
    }
    return g_open && mz_zip_reader_locate_file(&g_zip, name.c_str(), nullptr, 0) >= 0;
}

u32 crc32_of(const std::string& name) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_dir.empty()) {
        std::ifstream f(dir_path(name), std::ios::binary);
        if (!f) return 0;
        std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return (u32)mz_crc32(MZ_CRC32_INIT, data.data(), data.size());
    }
    if (!g_open) return 0;
    int idx = mz_zip_reader_locate_file(&g_zip, name.c_str(), nullptr, 0);
    mz_zip_archive_file_stat st;
    if (idx < 0 || !mz_zip_reader_file_stat(&g_zip, (mz_uint)idx, &st)) return 0;
    return st.m_crc32;
}

std::vector<std::string> list(const std::string& dir_in) {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::string> out;
    if (!g_dir.empty()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_path(dir_in), ec))
            if (e.is_regular_file(ec)) out.push_back(e.path().filename().string());
        return out;
    }
    if (!g_open) return out;
    std::string dir = dir_in;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    mz_uint n = mz_zip_reader_get_num_files(&g_zip);
    for (mz_uint i = 0; i < n; i++) {
        char name[512];
        mz_zip_reader_get_filename(&g_zip, i, name, sizeof name);
        std::string s = name;
        if (s.compare(0, dir.size(), dir) != 0) continue;
        std::string rest = s.substr(dir.size());
        if (!rest.empty() && rest.find('/') == std::string::npos) out.push_back(rest);
    }
    return out;
}

}  // namespace apk
