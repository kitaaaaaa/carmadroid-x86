// The controls options screen gets a "STEERING DEADZONE" slider (the port's gamepad steering deadzone, see
// controller.cpp). The screen's layout files (UI/LAYOUT/<lang>/<type>/FRONTEND/CONTROLS_LAYOUT.LOL, compiled
// Lua) are straight-line table constructors: they are decompiled here, the steering block gets a third row,
// and they are written back as Lua source (the game's Lua still has its compiler). The screen's logic
// (UI/LOGIC/FRONTEND/CONTROLS.LOL) is replaced by an equivalent script that also shows the percentage.
#include "pcimport/cars.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace pcimport {
namespace {

// ---- a small AST for straight-line Lua ---------------------------------------------------------------
struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Text, Table, Call } kind = Text;
    std::string text;                                   // Text: source; Call: callee source
    std::vector<std::pair<std::string, ExprP>> fields;  // Table: "key = value" (key as written)
    std::vector<ExprP> array;                           // Table: positional; Call: arguments
    bool multi = false;                                 // Call left with all its results (C=0)
};
ExprP text(const std::string& s) { auto e = std::make_shared<Expr>(); e->text = s; return e; }

struct Stmt {
    std::vector<std::string> locals;  // "local a, b = expr" when non-empty
    std::string lhs;                  // "lhs = expr" when non-empty
    ExprP expr;                       // otherwise an expression statement (a call)
};

const char* const kOps[] = {"MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
                            "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD", "POW",
                            "UNM", "NOT", "LEN", "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "TESTSET", "CALL",
                            "TAILCALL", "RETURN", "FORLOOP", "FORPREP", "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE",
                            "VARARG"};

std::string num(double v) {
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%.0f", v);
    else snprintf(b, sizeof b, "%.17g", v);
    return b;
}

std::string lit(const LuaConst& k) {
    switch (k.type) {
        case LuaConst::Nil: return "nil";
        case LuaConst::Bool: return k.num ? "true" : "false";
        case LuaConst::Num: return num(k.num);
        default: break;
    }
    std::string s = "\"";
    for (unsigned char ch : k.str) {
        if (ch == '"' || ch == '\\') s += '\\', s += (char)ch;
        else if (ch == '\n') s += "\\n";
        else if (ch < 32 || ch > 126) s += "\\" + std::to_string(ch);
        else s += (char)ch;
    }
    return s + "\"";
}

bool ident(const std::string& s) {
    static const char* kw[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
                               "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '_')) return false;
    for (const char* k : kw)
        if (s == k) return false;
    return true;
}

std::string src(const ExprP& e, const std::string& ind = "") {
    if (e->kind == Expr::Text) return e->text;
    if (e->kind == Expr::Call) {
        std::string s = e->text + "(";
        for (size_t i = 0; i < e->array.size(); i++) s += (i ? ", " : "") + src(e->array[i], ind);
        return s + ")";
    }
    std::vector<std::string> parts;
    const std::string in = ind + "  ";
    for (const auto& v : e->array) parts.push_back(src(v, in));
    for (const auto& [k, v] : e->fields) parts.push_back(k + " = " + src(v, in));
    if (parts.empty()) return "{}";
    std::string s = "{\n";
    for (size_t i = 0; i < parts.size(); i++) s += in + parts[i] + (i + 1 < parts.size() ? ",\n" : "\n");
    return s + ind + "}";
}

// Decompiles the main function of a branch-free chunk (no nested functions).
std::vector<Stmt> decompile(const Bytes& d) {
    const auto funcs = lua_load(d);
    if (funcs.size() != 1) throw std::runtime_error("layout has nested functions");
    const LuaFunc& f = funcs[0];
    const auto& K = f.k;
    std::map<int, ExprP> reg;
    std::map<int, std::string> self_call;  // register -> "obj:method" for SELF
    std::vector<Stmt> out;
    int ntemp = 0, multi = -1;
    auto kc = [&](u32 i) -> const LuaConst& {
        if (i >= K.size()) throw std::runtime_error("bad constant index");
        return K[i];
    };
    auto val = [&](int r) -> ExprP {
        auto it = reg.find(r);
        if (it == reg.end()) throw std::runtime_error("read of an unset register");
        return it->second;
    };
    auto rk = [&](u32 x) { return x >= 256 ? text(lit(kc(x - 256))) : val((int)x); };
    auto key = [&](u32 x) -> std::string {
        if (x >= 256 && kc(x - 256).type == LuaConst::Str && ident(kc(x - 256).str)) return kc(x - 256).str;
        return "[" + src(rk(x)) + "]";
    };
    auto index = [&](const std::string& obj, u32 x) {
        const std::string k = key(x);
        return k[0] == '[' ? obj + k : obj + "." + k;
    };
    for (u32 pc = 0; pc < f.ncode; pc++) {
        u32 ins;
        memcpy(&ins, &d[f.code_at + 4 * pc], 4);
        const u32 op = ins & 0x3f, a = (ins >> 6) & 0xff, c = (ins >> 14) & 0x1ff, b = (ins >> 23) & 0x1ff, bx = ins >> 14;
        if (op >= 38) throw std::runtime_error("bad opcode");
        const std::string n = kOps[op];
        if (n == "MOVE") reg[a] = val(b);
        else if (n == "LOADK") reg[a] = text(lit(kc(bx)));
        else if (n == "LOADBOOL" && !c) reg[a] = text(b ? "true" : "false");
        else if (n == "LOADNIL") for (u32 r = a; r <= b; r++) reg[r] = text("nil");
        else if (n == "GETGLOBAL") reg[a] = text(kc(bx).str);
        else if (n == "GETTABLE") reg[a] = text(index(src(val(b)), c));
        else if (n == "SETGLOBAL") out.push_back({{}, kc(bx).str, val(a)});
        else if (n == "SETTABLE") {
            ExprP t = val(a);
            if (t->kind == Expr::Table) t->fields.push_back({key(b), rk(c)});
            else out.push_back({{}, index(src(t), b), rk(c)});
        } else if (n == "NEWTABLE") {
            reg[a] = std::make_shared<Expr>();
            reg[a]->kind = Expr::Table;
        } else if (n == "SELF") {
            const std::string obj = src(val(b));
            reg[a + 1] = text(obj);
            self_call[a] = obj + ":" + kc(c - 256).str;
            reg[a] = text("<self>");
        } else if (n == "UNM") reg[a] = text("-" + src(val(b)));
        else if (n == "VARARG" && b >= 2) for (u32 r = a; r < a + b - 1; r++) reg[r] = text("...");
        else if (n == "CALL") {
            auto call = std::make_shared<Expr>();
            call->kind = Expr::Call;
            const bool is_self = self_call.count(a) != 0;
            call->text = is_self ? self_call[a] : src(val(a));
            self_call.erase(a);
            const int last = b == 0 ? multi : (int)(a + b - 1);
            if (b == 0 && multi < 0) throw std::runtime_error("call to top without a multi-result call");
            for (int r = a + 1 + (is_self ? 1 : 0); r <= last; r++) call->array.push_back(val(r));
            if (c == 1) {
                out.push_back({{}, "", call});
            } else if (c == 0) {
                call->multi = true;
                reg[a] = call;
                multi = a;
            } else {
                Stmt st;
                for (u32 r = a; r < a + c - 1; r++) {
                    st.locals.push_back("t" + std::to_string(++ntemp));
                    reg[r] = text(st.locals.back());
                }
                st.expr = call;
                out.push_back(st);
            }
        } else if (n == "SETLIST" && c != 0) {
            ExprP t = val(a);
            const int last = b == 0 ? multi : (int)(a + b);
            for (int r = a + 1; r <= last; r++) t->array.push_back(val(r));
        } else if (n == "RETURN" && b == 1) {
        } else {
            throw std::runtime_error("layout uses " + n);
        }
    }
    return out;
}

std::string emit(const std::vector<Stmt>& stmts) {
    std::string s;
    for (const auto& st : stmts) {
        if (!st.locals.empty()) {
            s += "local ";
            for (size_t i = 0; i < st.locals.size(); i++) s += (i ? ", " : "") + st.locals[i];
            s += " = ";
        } else if (!st.lhs.empty()) {
            s += st.lhs + " = ";
        }
        s += src(st.expr) + "\n";
    }
    return s;
}

ExprP field(const ExprP& t, const std::string& k) {
    for (auto& [key, v] : t->fields)
        if (key == k) return v;
    return nullptr;
}
void set_field(const ExprP& t, const std::string& k, const std::string& v) {
    for (auto& [key, val] : t->fields)
        if (key == k) { val = text(v); return; }
    t->fields.push_back({k, text(v)});
}
double num_field(const ExprP& t, const std::string& k) {
    ExprP v = field(t, k);
    if (!v || v->kind != Expr::Text) throw std::runtime_error("layout item without " + k);
    return std::stod(v->text);
}
ExprP copy(const ExprP& e) {
    auto c = std::make_shared<Expr>(*e);
    for (auto& [k, v] : c->fields) v = copy(v);
    for (auto& v : c->array) v = copy(v);
    return c;
}

// The call statement "local tN = <callee>({...})" whose table has field `k` = `v` (or any, if v is empty).
Stmt* find_item(std::vector<Stmt>& stmts, const std::string& callee, const std::string& k = "", const std::string& v = "") {
    for (auto& st : stmts) {
        if (st.locals.size() != 1 || !st.expr || st.expr->kind != Expr::Call || st.expr->text != callee) continue;
        if (st.expr->array.size() != 1 || st.expr->array[0]->kind != Expr::Table) continue;
        if (k.empty()) return &st;
        ExprP f = field(st.expr->array[0], k);
        if (f && f->kind == Expr::Text && f->text == v) return &st;
    }
    return nullptr;
}

std::string add_deadzone_row(const Bytes& layout) {
    auto stmts = decompile(layout);
    Stmt* frame = find_item(stmts, "backing_frame");  // the first one frames the steering rows
    Stmt* steer = find_item(stmts, "text", "text", "txt.CONTROL_STEER");
    Stmt* sens = find_item(stmts, "text", "text", "txt.STEERING_SENSITIVITY");
    Stmt* slider = find_item(stmts, "Steering_slider");
    Stmt* list = nullptr;
    for (auto& st : stmts)
        if (st.locals.empty() && st.lhs.empty() && st.expr && st.expr->kind == Expr::Call && st.expr->text == "content") list = &st;
    if (!frame || !steer || !sens || !slider || !list) throw std::runtime_error("unexpected controls layout");
    const ExprP ft = frame->expr->array[0], st = steer->expr->array[0], se = sens->expr->array[0];
    const double steer_y = num_field(st, "y"), sens_y = num_field(se, "y"), gap = sens_y - steer_y;
    // The steering block grows upwards by a row: mode, sensitivity, then the deadzone.
    set_field(ft, "y", num(num_field(ft, "y") - gap / 2));
    set_field(ft, "h", num(num_field(ft, "h") + gap));
    set_field(st, "y", num(steer_y - gap));
    set_field(se, "y", num(steer_y));
    ExprP label = copy(se);
    label->fields.erase(std::remove_if(label->fields.begin(), label->fields.end(),
                                       [](const auto& kv) { return kv.first == "group"; }),
                        label->fields.end());
    set_field(label, "text", "\"STEERING DEADZONE\"");
    set_field(label, "y", num(sens_y));
    label->fields.insert(label->fields.begin(), {"id", text("id.steering_deadzone_text")});
    ExprP dz = copy(slider->expr->array[0]);
    dz->fields.erase(std::remove_if(dz->fields.begin(), dz->fields.end(),
                                    [](const auto& kv) { return kv.first == "group" || kv.first == "id"; }),
                     dz->fields.end());
    dz->fields.insert(dz->fields.begin(), {"id", text("id.steering_deadzone_slider")});
    set_field(dz, "value_min", "0");
    set_field(dz, "value_max", "50");
    set_field(dz, "property_watch", "\"steering_deadzone\"");
    const std::string slider_local = slider->locals[0];
    // New locals before content{}, and the two items after the steering sensitivity slider.
    const size_t at = list - &stmts[0];
    Stmt l1{{"dz_label"}, "", std::make_shared<Expr>()};
    l1.expr->kind = Expr::Call;
    l1.expr->text = "text";
    l1.expr->array = {label};
    Stmt l2{{"dz_slider"}, "", std::make_shared<Expr>()};
    l2.expr->kind = Expr::Call;
    l2.expr->text = "slider";  // the game's generic slider type (TYPES.LOL), set up for this property

    l2.expr->array = {dz};
    ExprP items = field(list->expr->array[0], "items");
    if (!items || items->kind != Expr::Table) throw std::runtime_error("controls layout: no item list");
    auto pos = std::find_if(items->array.begin(), items->array.end(),
                            [&](const ExprP& e) { return e->kind == Expr::Text && e->text == slider_local; });
    if (pos == items->array.end()) throw std::runtime_error("controls layout: slider not listed");
    items->array.insert(pos + 1, {text("dz_label"), text("dz_slider")});
    stmts.insert(stmts.begin() + at, {l1, l2});
    return "-- carmadroid: the game's controls layout with a steering deadzone slider added\n" + emit(stmts);
}

// UI/LOGIC/FRONTEND/CONTROLS.LOL, as the game's, plus the deadzone percentage on the new slider's label.
const char* const kControlsLogic = R"(-- carmadroid: the game's controls screen logic, plus the steering deadzone slider's label
module(..., lube.menu_logic)

local function deadzone_text()
  local v = properties.steering_deadzone or 12
  v = v + 0.5
  return "STEERING DEADZONE " .. (v - v % 1) .. "%"
end

on_event[id.back] = function(self, item, ev)
  if ev == event.click then
    local steering = self:send_item_message(id.steering, message.get_selection_id)
    local throttle = self:send_item_message(id.throttle, message.get_selection_id)
    game:save_control_settings(steering, throttle)
    game:save_settings()
    control:save_control_positions()
    ui:pop()
  end
end

function on_update(self)
  local steering = self:send_item_message(id.steering, message.get_selection_id)
  local throttle = self:send_item_message(id.throttle, message.get_selection_id)
  if steering ~= self.steeringmode then
    game:save_control_settings(steering, throttle)
    game:refresh_sensitivity_settings()
    self.steeringmode = steering
    update_sliders_visibility(self)
  end
  if throttle ~= self.throttlemode then
    game:save_control_settings(steering, throttle)
    game:refresh_sensitivity_settings()
    self.throttlemode = throttle
    update_sliders_visibility(self)
  end
  local dz = deadzone_text()
  if dz ~= self.deadzone_text then
    self.deadzone_text = dz
    self:send_item_message(id.steering_deadzone_text, message.set_text, {text = dz})
  end
end

function on_focus(self, focused)
  if focused then
    update_sliders_visibility(self)
  end
end

on_event[id.steering_slider] = function(self, item, ev)
  if ev == event.value_change then
    local steering = self:send_item_message(id.steering, message.get_selection_id)
    local throttle = self:send_item_message(id.throttle, message.get_selection_id)
    game:save_control_settings(steering, throttle)
  end
end

on_event[id.breaking_slider] = function(self, item, ev)
  if ev == event.value_change then
    local steering = self:send_item_message(id.steering, message.get_selection_id)
    local throttle = self:send_item_message(id.throttle, message.get_selection_id)
    game:save_control_settings(steering, throttle)
  end
end

function on_push(self)
  self.steeringmode = properties.steering_mode
  self.throttlemode = properties.throttle_mode
  self:send_item_message(id.steering, message.set_selection, {startValue = properties.steering_mode, defaultStart = 1})
  self:send_item_message(id.throttle, message.set_selection, {startValue = properties.throttle_mode, defaultStart = 1})
end

function update_sliders_visibility(self)
  if self.steeringmode == 1 then
    self:hide_group(id.steering_sensitivity_group)
  else
    self:show_group(id.steering_sensitivity_group)
  end
  if self.throttlemode == 1 then
    self:hide_group(id.breaking_sensitivity_group)
  else
    self:show_group(id.breaking_sensitivity_group)
  end
end
)";

// The game's controls logic, as this replacement expects it (its constants).
bool is_known_controls_logic(const Bytes& d) {
    try {
        const auto funcs = lua_load(d);
        if (funcs.size() != 8) return false;
        std::set<std::string> ks;
        for (const auto& f : funcs)
            for (const auto& k : f.k)
                if (k.type == LuaConst::Str) ks.insert(k.str);
        for (const char* need : {"steering_slider", "breaking_slider", "save_control_settings", "refresh_sensitivity_settings",
                                 "update_sliders_visibility", "save_control_positions", "steering_sensitivity_group"})
            if (!ks.count(need)) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int install_controls_screen(Install& inst) {
    const fs::path ui = inst.root / "DATA" / "CONTENT" / "UI";
    const fs::path logic = ui / "LOGIC" / "FRONTEND" / "CONTROLS.LOL";
    Bytes d;
    if (!read_file(logic, d) || !is_known_controls_logic(d)) {
        LOGI("pc import: controls screen not recognised, no deadzone slider");
        return 0;
    }
    // Layouts first: if any can't be converted, leave the screen alone.
    std::vector<std::pair<fs::path, std::string>> layouts;
    for (const auto& e : fs::recursive_directory_iterator(ui / "LAYOUT")) {
        if (!e.is_regular_file() || upper(e.path().filename().string()) != "CONTROLS_LAYOUT.LOL") continue;
        layouts.push_back({e.path(), add_deadzone_row(read_file(e.path()))});
    }
    if (layouts.empty()) return 0;
    for (const auto& [p, s] : layouts) inst.write(p, s);
    inst.write(logic, std::string(kControlsLogic));
    return (int)layouts.size();
}

}  // namespace pcimport
