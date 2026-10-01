// Registering converted cars in the game's car lists and texts (port of tools/pc2android/addcar.py).
#include "pcimport/cars.h"
#include <algorithm>
#include <stdexcept>

namespace pcimport {
namespace {

// Text files are Latin-1 with CRLF line ends.
std::vector<std::string> read_lines(const fs::path& path) {
    const Bytes b = read_file(path);
    std::string s(b.begin(), b.end());
    std::vector<std::string> lines;
    size_t p = 0;
    for (;;) {
        size_t q = s.find('\n', p);
        std::string l = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (q != std::string::npos && !l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return lines;
}

void write_lines(Install& inst, const fs::path& path, const std::vector<std::string>& lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) s += (i ? "\r\n" : "") + lines[i];
    inst.write(path, s);
}

void drop_trailing_blanks(std::vector<std::string>& lines) {
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
}

std::string latin1_to_utf8(const std::string& s) {
    std::string o;
    for (char ch : s) {
        const u8 c = (u8)ch;
        if (c < 0x80) o += ch;
        else o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 0x3F));
    }
    return o;
}

std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

// Replaces the text of every <Data ss:Type="String">...</Data> in a cell.
std::string set_cell_data(const std::string& cell, const std::string& value) {
    static const std::string open = "<Data ss:Type=\"String\">", close = "</Data>";
    std::string out;
    size_t p = 0;
    for (;;) {
        const size_t a = cell.find(open, p);
        if (a == std::string::npos) break;
        const size_t b = cell.find(close, a + open.size());
        if (b == std::string::npos) break;
        out += cell.substr(p, a + open.size() - p) + value;
        p = b;
    }
    return out + cell.substr(p);
}

}  // namespace

bool add_to_list(Install& inst, const fs::path& path, const std::string& name) {
    auto lines = read_lines(path);
    for (const auto& l : lines)
        if (lower(trim(l)) == lower(name)) return false;
    drop_trailing_blanks(lines);
    lines.push_back(name);
    write_lines(inst, path, lines);
    return true;
}

// OPPONENTLIST.TXT: car name, then its strength (1-5) on the next line.
void set_opponent(Install& inst, const fs::path& path, const std::string& name, int strength) {
    auto lines = read_lines(path);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string l = lines[i];
        if (size_t c = l.find("//"); c != std::string::npos) l.resize(c);
        if (lower(trim(l)) == lower(name)) {
            if (i + 1 < lines.size()) lines[i + 1] = std::to_string(strength);
            else lines.push_back(std::to_string(strength));
            write_lines(inst, path, lines);
            return;
        }
    }
    drop_trailing_blanks(lines);
    lines.push_back(name);
    lines.push_back(std::to_string(strength));
    write_lines(inst, path, lines);
}

// CARSPECS.TXT: tab-separated, a header row; the new row starts as a copy of the template car's.
void set_specs(Install& inst, const fs::path& path, const std::string& name, const std::string& tmpl,
               const std::string& specs) {
    auto lines = read_lines(path);
    if (lines.empty()) throw std::runtime_error("CARSPECS.TXT is empty");
    const auto header = split(lines[0], '\t');
    std::map<std::string, size_t> rows;
    for (size_t i = 0; i < lines.size(); i++)
        if (!trim(lines[i]).empty()) rows[lower(split(lines[i], '\t')[0])] = i;
    auto t = rows.find(lower(tmpl));
    if (t == rows.end()) throw std::runtime_error("CARSPECS.TXT has no " + tmpl);
    auto row = split(lines[t->second], '\t');
    row[0] = name;
    if (!specs.empty()) {
        static const char* cols[] = {"Defence", "Offence", "Power", "Softness", "BHP/ton", "Top Speed", "Mass", "NaughtToSixty"};
        const auto vals = split(specs, ',');
        for (size_t k = 0; k < vals.size() && k < 8; k++) {
            if (trim(vals[k]).empty()) continue;
            auto h = std::find(header.begin(), header.end(), cols[k]);
            if (h == header.end()) throw std::runtime_error(std::string("CARSPECS.TXT has no column ") + cols[k]);
            const size_t col = h - header.begin();
            if (col < row.size()) row[col] = trim(vals[k]);
        }
    }
    std::string text;
    for (size_t k = 0; k < row.size(); k++) text += (k ? "\t" : "") + row[k];
    if (auto e = rows.find(lower(name)); e != rows.end()) {
        lines[e->second] = text;
    } else {
        drop_trailing_blanks(lines);
        lines.push_back(text);
    }
    write_lines(inst, path, lines);
}

// TEXT.TXT: "[KEY]" then the value on the next line.
void set_text_txt(Install& inst, const fs::path& path, const std::string& key, const std::string& value) {
    auto lines = read_lines(path);
    const std::string tag = "[" + key + "]";
    for (size_t i = 0; i < lines.size(); i++)
        if (trim(lines[i]) == tag) {
            if (i + 1 < lines.size()) lines[i + 1] = value;
            else lines.push_back(value);
            write_lines(inst, path, lines);
            return;
        }
    drop_trailing_blanks(lines);
    lines.push_back("");
    lines.push_back(tag);
    lines.push_back(value);
    write_lines(inst, path, lines);
}

// TEXT.XML is a spreadsheet: one <Row> per key, a cell per language. A new key's row is a copy of the
// template key's (cell 0: the key, cells 2+: the text in each language).
namespace {
void set_text_xml_impl(Install& inst, const fs::path& path, const std::string& key, const std::string* value_latin1,
                       const std::string& template_key) {
    const Bytes b = read_file(path);
    std::string s(b.begin(), b.end());
    auto find_row = [&](const std::string& k, size_t& start, size_t& end) {
        const std::string needle = "<Data ss:Type=\"String\">" + k + "</Data>";
        for (size_t p = s.find("<Row>"); p != std::string::npos; p = s.find("<Row>", p + 5)) {
            const size_t e = s.find("</Row>", p);
            if (e == std::string::npos) return false;
            const size_t n = s.find(needle, p);
            if (n != std::string::npos && n < e) {
                start = p;
                end = e + 6;
                return true;
            }
        }
        return false;
    };
    size_t start, end;
    const bool exists = find_row(key, start, end);
    if (!exists && !find_row(template_key, start, end))
        throw std::runtime_error("TEXT.XML: template key " + template_key + " not found");
    const std::string row = s.substr(start, end - start);
    std::string esc;
    if (value_latin1) esc = replace_all(replace_all(latin1_to_utf8(*value_latin1), "&", "&amp;"), "<", "&lt;");
    std::vector<std::string> cells;  // "<Cell ...>" up to the next "</Cell>"
    for (size_t p = row.find("<Cell"); p != std::string::npos; p = row.find("<Cell", p)) {
        const size_t gt = row.find('>', p);
        const size_t e = gt == std::string::npos ? gt : row.find("</Cell>", gt + 1);
        if (e == std::string::npos) break;
        cells.push_back(row.substr(p, e + 7 - p));
        p = e + 7;
    }
    if (cells.empty()) throw std::runtime_error("TEXT.XML: row without cells");
    for (size_t i = 0; i < cells.size(); i++) {
        if (i == 0) cells[i] = set_cell_data(cells[i], key);
        else if (i >= 2 && value_latin1) cells[i] = set_cell_data(cells[i], esc);
    }
    const std::string indent = row.substr(5, row.find("<Cell") - 5);
    std::string new_row = "<Row>" + indent;
    for (size_t i = 0; i < cells.size(); i++) new_row += (i ? indent : "") + cells[i];
    new_row += row.substr(row.rfind("</Cell>") + 7);
    if (exists) s = s.substr(0, start) + new_row + s.substr(end);
    else s = s.substr(0, end) + "\r\n   " + new_row + s.substr(end);
    inst.write(path, s);
}
}  // namespace

void set_text_xml(Install& inst, const fs::path& path, const std::string& key, const std::string* value_latin1,
                  const std::string& template_key) {
    set_text_xml_impl(inst, path, key, value_latin1, template_key);
}

// Copies another car's driver portraits (UI/ASSETS/*/DRIVERS) to this car.
void copy_driver_pictures(Install& inst, const fs::path& content, const std::string& source, const std::string& name) {
    for (const auto& e : fs::recursive_directory_iterator(content / "UI" / "ASSETS")) {
        if (!e.is_directory() || upper(e.path().filename().string()) != "DRIVERS") continue;
        const fs::path src = e.path() / (upper(source) + ".IMG");
        if (fs::exists(src)) inst.copy(src, e.path() / (upper(name) + ".IMG"));
    }
}

// The template car's UI pictures and damage HUD layouts, as placeholders for the new car.
int copy_placeholders(Install& inst, const fs::path& content, const std::string& name, const std::string& tmpl) {
    std::vector<std::pair<fs::path, fs::path>> todo;
    for (const auto& e : fs::recursive_directory_iterator(content / "UI")) {
        if (!e.is_regular_file()) continue;
        const std::string base = upper(e.path().stem().string()), ext = upper(e.path().extension().string());
        fs::path dst;
        if (base == upper(tmpl) && ext == ".IMG") dst = e.path().parent_path() / (upper(name) + ".IMG");
        else if (base == upper(tmpl) + "_LAYOUT" && ext == ".LOL") dst = e.path().parent_path() / (upper(name) + "_LAYOUT.LOL");
        else continue;
        if (!fs::exists(dst)) todo.push_back({e.path(), dst});
    }
    for (const auto& [src, dst] : todo) inst.copy(src, dst);
    return (int)todo.size();
}

}  // namespace pcimport
