// The Splat Pack cars the Android game lacks: convert, then add to the roster and the opponents
// (port of tools/pc2android/splatpack.py).
#include "pcimport/cars.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace pcimport {

void Install::write(const fs::path& path, const Bytes& data) {
    const std::string rel = fs::relative(path, root).generic_string();
    if (rel.empty() || rel.rfind("..", 0) == 0) throw std::runtime_error("write outside the game folder: " + path.string());
    const std::string key = lower(rel);
    if (undo_active_ && !undo_.count(key)) {
        const bool existed = fs::exists(path);
        undo_[key] = {rel, existed, existed ? read_file(path) : Bytes()};
    }
    if (!touched.count(key)) {
        touched.insert(key);
        std::error_code ec;
        if (fs::exists(path)) {  // a file of the game: keep the original (unless an earlier run already did)
            const fs::path backup = backup_dir / fs::path(rel);
            if (!fs::exists(backup)) {
                fs::create_directories(backup.parent_path(), ec);
                fs::copy_file(path, backup, ec);
                if (ec) throw std::runtime_error("can't back up " + rel);
            }
            changed.push_back(rel);
            journal("changed " + rel);
        } else {
            created.push_back(rel);
            journal("created " + rel);
        }
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write((const char*)data.data(), (std::streamsize)data.size())) throw std::runtime_error("can't write " + rel);
}

void Install::rollback() {
    std::error_code ec;
    for (const auto& [key, prior] : undo_) {
        const std::string& rel = prior.rel;
        const fs::path path = root / fs::path(rel);
        if (prior.existed) {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f || !f.write((const char*)prior.data.data(), (std::streamsize)prior.data.size()))
                LOGE("pc import: couldn't restore %s", rel.c_str());
            continue;
        }
        fs::remove(path, ec);
        // folders this car created, now empty
        for (fs::path d = path.parent_path(); d.string().size() > root.string().size(); d = d.parent_path())
            if (!fs::is_directory(d, ec) || !fs::is_empty(d, ec) || !fs::remove(d, ec)) break;
        // no longer part of the install (the journal line stays: uninstall skips missing files)
        created.erase(std::remove(created.begin(), created.end(), rel), created.end());
        touched.erase(key);
    }
    undo_.clear();
    undo_active_ = false;
}

void Install::journal(const std::string& line) {
    if (journal_path.empty()) return;
    std::ofstream f(journal_path, std::ios::binary | std::ios::app);
    f << line << "\n";
}

namespace {

// Splat Pack cars that are not in the Android game (PC file name -> display name, from the CWA wiki's
// list of Carmageddon vehicles).
const std::pair<const char*, const char*> kCars[] = {
    {"333", "FEARARI F999"}, {"BUGGIT", "BUGUTTI"}, {"DOOZER", "DOOZER"}, {"JAQUES", "DE GORY'UN"},
    {"JEEPY", "RAMRAIDER"}, {"MONSTER", "MONSTER MASHER"}, {"MUSCLE", "STODGE BARGER"}, {"NEWANNIE", "HAWK II"},
    {"NEWEAGLE", "EAGLE II"}, {"PARAMED", "BLOOD MOBILE"}, {"PORK", "CARRERASAUR"}, {"ROADHOG", "ROADHOG"},
    {"SEMI", "RIG O'MORTIS"}, {"SLED", "THE SLED"}, {"SPAGHETI", "STILETTO"}, {"SUBFRAME", "KILLER COOP"},
    {"TOOHORSE", "PIECE MAKER"}, {"V6SHAME", "KILLER KITTY"}, {"VLAD2", "ANNIHILATOR II"},
};
// The stock car each one takes its handling and collision shape from: the nearest in body length,
// width and height, wheelbase, track and wheel size (the Eagle II: the Eagle; the Monster Masher: the Twister,
// the big-wheeled truck whose box reaches down between its wheels).
const std::map<std::string, std::string> kCounterpart = {
    {"333", "KUTTER"},    {"BUGGIT", "GRIMM"},     {"DOOZER", "FIRE"},     {"JAQUES", "OTIS"},
    {"JEEPY", "APC"},     {"MONSTER", "SCREWIE"},     {"MUSCLE", "AGENTO"},   {"NEWANNIE", "ANNIECAR"},
    {"NEWEAGLE", "BLKEAGLE"}, {"PARAMED", "BIGAPC"}, {"PORK", "STIG"},     {"ROADHOG", "TARTLET"},
    {"SEMI", "BIGAPC"},   {"SLED", "BIGAPC"},      {"SPAGHETI", "TASHITA"}, {"SUBFRAME", "VALHELLA"},
    {"TOOHORSE", "EDHUNT"}, {"V6SHAME", "KUTTER"}, {"VLAD2", "VLAD"},
};
const std::set<std::string> kDisabled = {};  // cars not installed
// Cars whose collision shape is widened to take in their tyres (the Monster Masher's huge wheels stick out
// 1.6 m past its body: pedestrians would pass under them untouched).
const std::set<std::string> kCoverWheels = {"MONSTER"};
// The Splat Pack's player cars belong to Max Damage and Die Anna: they reuse the Android portraits and text.
const std::map<std::string, std::pair<std::string, std::string>> kPlayerCars = {
    {"NEWEAGLE", {"BlkEagle", "Max Damage"}}, {"NEWANNIE", {"AnnieCar", "Die Anna"}}};
constexpr int kMaxCars = 64;  // the game's 40-car tables are raised to 64 (roster.cpp)

struct CarInfo {
    std::string driver, mug, blurb;
    int strength = 3;
    double mph = 0, tons = 0, sixty = 0;  // 0 = unknown
};

bool parse_int(const std::string& s, int& v) {  // like Python's int(): whole string, optional sign
    const std::string t = trim(s);
    size_t i = (!t.empty() && (t[0] == '-' || t[0] == '+')) ? 1 : 0;
    if (i >= t.size()) return false;
    for (size_t k = i; k < t.size(); k++)
        if (t[k] < '0' || t[k] > '9') return false;
    v = std::atoi(t.c_str());
    return true;
}

// OPPONENT.TXT: per opponent, the name/strength/mugshot lines come before the car file name, then text
// chunks (the last is the blurb) and the stats ("TOP SPEED: ...").
std::map<std::string, CarInfo> read_opponents(const fs::path& path) {
    std::vector<std::string> L;
    for (std::string l : c1_read_text(path)) {
        if (size_t c = l.find("//"); c != std::string::npos) l.resize(c);
        while (!l.empty() && isspace((u8)l.back())) l.pop_back();
        L.push_back(l);
    }
    static const std::regex speed_re("TOP SPEED:\\s*([\\d.]+)"), weight_re("WEIGHT:\\s*([\\d.]+)"),
        sixty_re("0-60MPH:\\s*([\\d.]+)");
    std::map<std::string, CarInfo> res;
    for (size_t i = 6; i < L.size(); i++) {
        const std::string u = upper(trim(L[i]));
        if (u.size() < 4 || u.compare(u.size() - 4, 4, ".TXT") != 0) continue;
        CarInfo info;
        info.driver = trim(L[i - 6]);
        if (!parse_int(L[i - 3], info.strength)) info.strength = 3;
        info.mug = trim(L[i - 1]);
        // text chunks: count, then per chunk: x,y / frames / line count / lines
        [&] {
            size_t k = i + 2;
            int chunks, n;
            if (k >= L.size() || !parse_int(L[k], chunks)) return;
            k++;
            std::vector<std::string> text;
            for (int c = 0; c < chunks; c++) {
                k += 2;
                if (k >= L.size() || !parse_int(L[k], n)) return;
                k++;
                text.clear();
                for (size_t j = k; j < k + n && j < L.size(); j++) text.push_back(trim(L[j]));
                k += n;
                while (k < L.size() && trim(L[k]).empty()) k++;
            }
            std::string blurb;
            for (const auto& t : text)
                if (!t.empty()) blurb += (blurb.empty() ? "" : " ") + t;
            info.blurb = blurb;
        }();
        for (size_t j = i; j < i + 14 && j < L.size(); j++) {
            std::smatch m;
            if (std::regex_search(L[j], m, speed_re)) info.mph = std::atof(m[1].str().c_str());
            if (std::regex_search(L[j], m, weight_re)) info.tons = std::atof(m[1].str().c_str());
            if (std::regex_search(L[j], m, sixty_re)) info.sixty = std::atof(m[1].str().c_str());
        }
        res[u.substr(0, u.size() - 4)] = info;
    }
    return res;
}

std::string py_float(double v) {  // Python's str(float): shortest round-trip form, "5.0" for whole numbers
    char b[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, v);
        if (std::strtod(b, nullptr) == v) break;
    }
    std::string s = b;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

// The driver's first name: the first word of the name, skipping a leading "THE" (The Ashteroid).
std::string first_name(const std::string& driver) {
    std::vector<std::string> words;
    for (const auto& w : split(trim(driver), ' '))
        if (!w.empty()) words.push_back(upper(w));
    if (words.size() > 1 && words[0] == "THE") words.erase(words.begin());
    return words.empty() ? upper(driver) : words[0];
}

std::string android_name(const std::string& car) {  // "JAQUES" -> "Jaques", "333" -> "Car333"
    if (isdigit((u8)car[0])) return "Car" + car;
    return car.substr(0, 1) + lower(car.substr(1));
}

int count_cars(const fs::path& list) {
    std::ifstream f(list, std::ios::binary);
    std::string l;
    int n = 0;
    while (std::getline(f, l))
        if (!trim(l).empty()) n++;
    return n;
}

}  // namespace

int install_splat_pack(Install& inst, const fs::path& splat, const fs::path& base) {
    const fs::path content = inst.root / "DATA" / "CONTENT";
    const auto opp = read_opponents(splat / "OPPONENT.TXT");
    const Bytes mtl_template = read_file(content / "TRACKS" / "LEVELS" / "CITY_A" / "1GRILLS.MTL");
    const Bytes car_mtl_template = read_file(content / "VEHICLES" / "BLKEAGLE" / "BLKEAGLE.MTL");
    int installed = 0;
    for (const auto& [car, display] : kCars) {
        if (kDisabled.count(car)) continue;
        const std::string name = android_name(car);
        const fs::path out = content / "VEHICLES" / upper(name);
        inst.begin_undo();
        try {
            if (fs::exists(out)) LOGI("pc import: replacing %s (installed earlier, e.g. by the Python tools)", name.c_str());
            if (count_cars(content / "QUICKRACECARS.TXT") >= kMaxCars) {
                LOGE("pc import: the car list is full (%d cars), %s not added", kMaxCars, name.c_str());
                continue;
            }
            const auto it = opp.find(car);
            const CarInfo info = it != opp.end() ? it->second : CarInfo{};
            const double tons = info.tons ? info.tons : 1.5;
            // Specs (A/P/O), menu placeholders and damage HUD from the Eagle (the Dump if heavy); handling and
            // collision shape from the stock counterpart.
            const std::string tmpl = tons >= 2.5 ? "DUMP" : "BLKEAGLE";
            auto cp = kCounterpart.find(car);
            std::string phys = cp != kCounterpart.end() ? cp->second : tmpl;
            if (!fs::exists(content / "VEHICLES" / phys / "CAR.TXT")) phys = tmpl;
            convert_car(inst, {splat, base}, std::string(car) + ".TXT", out, content / "VEHICLES" / upper(tmpl),
                        content / "VEHICLES" / phys, mtl_template,
                        car_mtl_template, kCoverWheels.count(car) != 0);

            // roster, specs, opponents, texts
            int strength = info.strength;
            const bool opponent = strength >= 1;  // player cars (New Eagle, New Annie) have -1
            if (!opponent) strength = 3;
            const int defence = std::max(1, std::min(5, (int)std::nearbyint(tons * 1.2)));
            std::string specs = std::to_string(defence) + "," + std::to_string(strength) + ",,,,";
            specs += (info.mph ? std::to_string((long long)std::nearbyint(info.mph * 1.609)) : "") + ",";
            specs += (info.tons ? std::to_string((long long)(info.tons * 1000)) : "") + ",";
            specs += info.sixty ? py_float(info.sixty) : "";
            const auto player = kPlayerCars.find(car);
            const std::string driver = player != kPlayerCars.end() ? player->second.second
                                       : !info.driver.empty()     ? info.driver
                                                                  : name;
            const std::string up = upper(name);
            add_to_list(inst, content / "QUICKRACECARS.TXT", name);
            set_specs(inst, content / "CARSPECS.TXT", name, tmpl, specs);
            if (opponent) set_opponent(inst, content / "OPPONENTLIST.TXT", name, strength);
            set_text_txt(inst, content / "TEXT.TXT", "CAR_" + up, upper(display));
            set_text_txt(inst, content / "TEXT.TXT", up + "_DRIVER", upper(driver));
            const std::string disp = upper(display), drv = upper(driver);
            set_text_xml(inst, content / "TEXT" / "TEXT.XML", "CAR_" + up, &disp, "CAR_BLKEAGLE");  // (row layout only)
            set_text_xml(inst, content / "TEXT" / "TEXT.XML", up + "_DRIVER", &drv, "BLKEAGLE_DRIVER");
            copy_placeholders(inst, content, name, tmpl);

            // pictures, damage HUD, description
            const auto model = load_car(out);
            const fs::path mug = player == kPlayerCars.end() && !info.mug.empty() ? splat / "ANIM" / info.mug : fs::path();
            make_pictures(inst, content, name, *model, mug);
            make_damage_hud(inst, content, name, *model);
            // "<driver's first name> WASTED"
            {
                const std::string first = car == std::string("NEWANNIE") ? "ANNA" : first_name(driver);  // (Die Anna)
                const std::string wasted = first + " WASTED";  // (English in every language)
                set_text_xml(inst, content / "TEXT" / "TEXT.XML", up + "_SHORT", &wasted, "BLKEAGLE_SHORT");
            }
            if (player != kPlayerCars.end()) {
                copy_driver_pictures(inst, content, player->second.first, name);
                set_text_xml(inst, content / "TEXT" / "TEXT.XML", up + "_INFO", nullptr, upper(player->second.first) + "_INFO");
            } else if (!info.blurb.empty()) {
                const std::string blurb = upper(info.blurb);
                set_text_xml(inst, content / "TEXT" / "TEXT.XML", up + "_INFO", &blurb, "BLKEAGLE_INFO");
            }
            inst.commit();
            installed++;
            LOGI("pc import: added %s (%s)", name.c_str(), display);
        } catch (const std::exception& e) {
            LOGE("pc import: %s failed, left out: %s", name.c_str(), e.what());
            inst.rollback();
        }
    }
    return installed;
}

}  // namespace pcimport
