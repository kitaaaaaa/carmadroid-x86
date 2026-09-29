#include "pc_import.h"
#include "pcimport/cars.h"
#include <algorithm>
#include <chrono>
#include <fstream>

namespace pc_import {
namespace {

namespace fs = std::filesystem;

// Bump when the converter's output changes: installs made by an older version are redone.
constexpr int kVersion = 1;

fs::path state_dir(const fs::path& root) { return root / "pcimport"; }
fs::path journal_file(const fs::path& root) { return state_dir(root) / "journal.txt"; }
fs::path backup_dir(const fs::path& root) { return state_dir(root) / "original"; }

std::vector<std::string> read_journal(const fs::path& root) {
    std::vector<std::string> lines;
    std::ifstream f(journal_file(root), std::ios::binary);
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

std::string folder_id(const std::string& dir) {
    if (dir.empty()) return "-";
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(dir), ec);
    return ec ? dir : p.string();
}

bool has_data(const std::string& dir) { return !dir.empty() && fs::is_directory(fs::path(dir) / "DATA"); }

}  // namespace

void uninstall(const std::string& game_dir) {
    const fs::path root = fs::path(game_dir);
    if (!fs::exists(journal_file(root))) return;
    std::error_code ec;
    std::vector<fs::path> dirs;
    int removed = 0, restored = 0;
    for (const auto& l : read_journal(root)) {
        if (l.rfind("created ", 0) == 0) {
            const fs::path p = root / fs::path(l.substr(8));
            if (fs::remove(p, ec)) removed++;
            for (fs::path d = p.parent_path(); d.string().size() > root.string().size(); d = d.parent_path())
                dirs.push_back(d);
        } else if (l.rfind("changed ", 0) == 0) {
            const std::string rel = l.substr(8);
            const fs::path backup = backup_dir(root) / fs::path(rel);
            if (fs::exists(backup)) {
                fs::copy_file(backup, root / fs::path(rel), fs::copy_options::overwrite_existing, ec);
                if (!ec) restored++;
                else LOGE("pc import: couldn't restore %s", rel.c_str());
            }
        }
    }
    // folders the install created and that are now empty, deepest first
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const auto& d : dirs)
        if (fs::is_directory(d, ec) && fs::is_empty(d, ec)) fs::remove(d, ec);
    fs::remove_all(state_dir(root), ec);
    LOGI("pc import: removed the earlier install (%d files removed, %d restored)", removed, restored);
}

void install(const std::string& game_dir, const std::string& carma, const std::string& carsplat) {
    if (carma.empty() && carsplat.empty()) return;
    const fs::path root = fs::path(game_dir);
    const std::string source = "source v" + std::to_string(kVersion) + " carma=" + folder_id(carma) +
                               " carsplat=" + folder_id(carsplat);
    const auto journal = read_journal(root);
    if (!journal.empty() && journal.front() == source && journal.back() == "done") {
        LOGI("pc import: up to date");
        return;
    }
    uninstall(game_dir);

    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::create_directories(state_dir(root), ec);
    pcimport::Install inst;
    inst.root = root;
    inst.backup_dir = backup_dir(root);
    inst.journal_path = journal_file(root);
    inst.journal(source);
    try {
        if (has_data(carma) && has_data(carsplat)) {
            const int cars = pcimport::install_splat_pack(inst, fs::path(carsplat) / "DATA", fs::path(carma) / "DATA");
            LOGI("pc import: %d Splat Pack cars installed", cars);
        } else {
            LOGI("pc import: Splat Pack cars need both the CARMA and CARSPLAT folders");
        }
        // Tracks: not converted yet; a track stage goes here.
        inst.journal("done");
    } catch (const std::exception& e) {
        LOGE("pc import: %s (will retry next start)", e.what());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("pc import: done in %.1f s (%zu files added, %zu game files changed)", secs, inst.created.size(), inst.changed.size());
}

}  // namespace pc_import
