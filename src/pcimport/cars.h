#pragma once
// Converting PC Carmageddon cars into Android vehicles and registering them in the game's files
// (a port of tools/pc2android: carconv.py, uiimg.py, damagehud.py, addcar.py, splatpack.py).
#include "pcimport/formats.h"
#include <set>

namespace pcimport {

// Every file the import writes into the game folder goes through here, so the install can be undone:
// new files are listed, files of the game that get changed are backed up first.
struct Install {
    fs::path root;                     // the unpacked game folder
    fs::path backup_dir;               // originals of changed files, by path relative to root
    std::vector<std::string> created;  // relative paths ('/' separators)
    std::vector<std::string> changed;  // relative paths of backed-up game files
    std::set<std::string> touched;     // lower-case relative paths handled in this run
    fs::path journal_path;             // "created <path>" / "changed <path>" lines are appended as it goes

    void journal(const std::string& line);
    void write(const fs::path& path, const Bytes& data);
    void write(const fs::path& path, const std::string& text) { write(path, Bytes(text.begin(), text.end())); }
    void copy(const fs::path& from, const fs::path& to) { write(to, read_file(from)); }
};

// ---- carconv ------------------------------------------------------------------------------
// Converts the PC car described by CARS/<car_txt> (searched in data_dirs, in order) into an Android
// vehicle folder. template_dir: an Android vehicle whose CAR.TXT is used; physics_dir: one whose CAR.TXT
// [DYNAMICS] (handling, collision shape) replaces the template's; mtl_template: a one-texture material.
// cover_wheels: the collision shape is widened and lengthened to take in the tyres (huge wheels that would
// otherwise run over pedestrians without touching them).
void convert_car(Install& inst, const std::vector<fs::path>& data_dirs, const std::string& car_txt,
                 const fs::path& out_dir, const fs::path& template_dir, const fs::path& physics_dir, const Bytes& mtl_template,
                 const Bytes& car_mtl_template, bool cover_wheels = false);

// ---- pictures and damage HUD ----------------------------------------------------------------
struct CarTri {
    std::array<V3, 3> p;
    std::array<std::array<double, 2>, 3> uv;
    const Rgba* tex = nullptr;
};
struct CarModel {
    std::vector<CarTri> tris;
    std::map<std::string, std::pair<double, double>> wheels;  // "whlfl" etc. -> (x, z)
    std::map<std::string, std::unique_ptr<Rgba>> textures;
};
std::unique_ptr<CarModel> load_car(const fs::path& vehicle_dir);  // a converted vehicle folder

// Menu pictures (select screen, pre-race grid) and, given the PC mugshot animation, driver portraits.
void make_pictures(Install& inst, const fs::path& content, const std::string& name, const CarModel& car,
                   const fs::path& mug_fli);
// Damage HUD silhouette and the layouts' image name and part positions. Returns the layouts updated.
int make_damage_hud(Install& inst, const fs::path& content, const std::string& name, const CarModel& car);

// ---- the game's car lists and texts ------------------------------------------------------------
bool add_to_list(Install& inst, const fs::path& path, const std::string& name);
void set_opponent(Install& inst, const fs::path& path, const std::string& name, int strength);
void set_specs(Install& inst, const fs::path& path, const std::string& name, const std::string& tmpl,
               const std::string& specs);
void set_text_txt(Install& inst, const fs::path& path, const std::string& key, const std::string& value);
// value_latin1 == nullptr: copy every language of the template row.
void set_text_xml(Install& inst, const fs::path& path, const std::string& key, const std::string* value_latin1,
                  const std::string& template_key);
void copy_driver_pictures(Install& inst, const fs::path& content, const std::string& source, const std::string& name);
int copy_placeholders(Install& inst, const fs::path& content, const std::string& name, const std::string& tmpl);

// ---- controls screen ------------------------------------------------------------------------------
// Adds the steering deadzone slider to the controls options screen. Returns the layouts rewritten.
int install_controls_screen(Install& inst);

// ---- Splat Pack --------------------------------------------------------------------------------
// Converts the Splat Pack cars the Android game lacks and adds them to the roster and opponents.
// splat_data / base_data: the PC CARSPLAT\DATA and CARMA\DATA folders. Returns the cars installed.
int install_splat_pack(Install& inst, const fs::path& splat_data, const fs::path& base_data);

}  // namespace pcimport
