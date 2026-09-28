// A minimal fake JVM: object handles, strings, classes, and host-implemented methods.
#include "jni.h"
#include "hle_common.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace hle;
using mem::ptr;
using mem::str;

namespace jni {
namespace {

enum class Kind { Class, String, Object, Array, DirectBuffer };
struct Obj {
    Kind kind;
    std::string cls;   // class name (for Class: the class itself; for Object: its class)
    std::string text;  // String value
    std::vector<u8> bytes;  // Array payload
    u32 addr = 0, capacity = 0;  // DirectBuffer
};
struct MethodInfo {
    std::string cls, name, sig;
    bool is_static;
};
struct FieldInfo {
    std::string cls, name, sig;
};

std::recursive_mutex g_lock;
std::unordered_map<u32, Obj> g_objs;
u32 g_next_handle = 0x100;
std::unordered_map<std::string, u32> g_classes;
std::vector<MethodInfo> g_methods;  // methodID = index + 1
std::vector<FieldInfo> g_fields;    // fieldID = index + 1
std::unordered_map<std::string, Method> g_impls;
std::unordered_map<std::string, Value> g_static_fields;
std::unordered_set<std::string> g_warned;
u32 g_env = 0, g_vm = 0;

u32 add_obj(Obj o) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    u32 h = (g_next_handle += 4);
    g_objs[h] = std::move(o);
    return h;
}
Obj* get(u32 h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_objs.find(h);
    return it == g_objs.end() ? nullptr : &it->second;
}
std::string class_of(u32 h) {
    Obj* o = get(h);
    if (!o) return "?";
    return o->kind == Kind::String ? "java/lang/String" : o->cls;
}

// Parse a method signature into parameter type chars ('L' for objects/arrays) and return type.
void parse_sig(const std::string& sig, std::vector<char>& params, char& ret) {
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') {
        char t = sig[i];
        if (t == '[') {
            while (sig[i] == '[') i++;
            if (sig[i] == 'L') i = sig.find(';', i);
            params.push_back('L');
        } else if (t == 'L') {
            i = sig.find(';', i);
            params.push_back('L');
        } else {
            params.push_back(t);
        }
        i++;
    }
    ret = i + 1 < sig.size() ? sig[i + 1] : 'V';
    if (ret == '[') ret = 'L';
}

enum class ArgMode { Varargs, VaList, JValues };

Value invoke(Cpu& c, u32 self, u32 method_id, ArgMode mode, int first_reg) {
    MethodInfo mi;
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (method_id == 0 || method_id > g_methods.size()) {
            LOGE("JNI call with bad methodID %u", method_id);
            return Value{};
        }
        mi = g_methods[method_id - 1];
    }
    std::vector<char> ptypes;
    char ret;
    parse_sig(mi.sig, ptypes, ret);

    std::vector<Value> args;
    std::unique_ptr<Args> reader;
    u32 jv = 0;
    if (mode == ArgMode::Varargs) reader = std::make_unique<RegArgs>(c, first_reg);
    else if (mode == ArgMode::VaList) reader = std::make_unique<VaArgs>(c.r(first_reg));
    else jv = c.r(first_reg);
    for (size_t k = 0; k < ptypes.size(); k++) {
        Value v{};
        char t = ptypes[k];
        if (mode == ArgMode::JValues) {
            u32 a = jv + (u32)k * 8;
            if (t == 'J' || t == 'D') memcpy(&v.j, ptr(a), 8);
            else v.i = mem::r32(a);  // float bits for 'F'
        } else if (t == 'J') {
            v.j = reader->u64v();
        } else if (t == 'D') {
            v.d = reader->f64();
        } else if (t == 'F') {
            v.f = (float)reader->f64();  // promoted to double in C varargs
        } else {
            v.i = reader->u32v();
        }
        args.push_back(v);
    }

    std::string key = mi.cls + "." + mi.name;
    Method impl;
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_impls.find(key);
        if (it != g_impls.end()) impl = it->second;
        // Methods may be looked up on a subclass/instance class: fall back on name only.
        if (!impl) {
            std::string suffix = "." + mi.name;
            for (auto& [k, m] : g_impls)
                if (k.size() > suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    impl = m;
                    break;
                }
        }
    }
    if (!impl) {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (g_warned.insert(key).second) LOGI("JNI: unimplemented Java method %s %s", key.c_str(), mi.sig.c_str());
        return Value{};
    }
    LOGV("JNI call %s%s", key.c_str(), mi.sig.c_str());
    return impl(self, args);
}

void set_return(Cpu& c, Value v, char kind) {
    if (kind == 'J' || kind == 'D') c.ret64(v.j);
    else c.ret(v.i);
}

// ---------------------------------------------------------------------------
// JNINativeInterface function table
// ---------------------------------------------------------------------------
constexpr int kNumFns = 233;
std::vector<HleFn> g_fns(kNumFns);

void def(int idx, HleFn f) { g_fns[idx] = std::move(f); }

u32 get_method_id(Cpu& c, bool is_static) {
    // (env, clazz, name, sig)
    MethodInfo mi{class_of(c.r(1)), str(c.r(2)), str(c.r(3)), is_static};
    std::lock_guard<std::recursive_mutex> l(g_lock);
    for (size_t i = 0; i < g_methods.size(); i++)
        if (g_methods[i].cls == mi.cls && g_methods[i].name == mi.name && g_methods[i].sig == mi.sig) return (u32)i + 1;
    g_methods.push_back(mi);
    LOGV("JNI Get%sMethodID %s.%s %s", is_static ? "Static" : "", mi.cls.c_str(), mi.name.c_str(), mi.sig.c_str());
    return (u32)g_methods.size();
}

void build_table() {
    // Call<Type>Method families: base index, return kind
    struct Family { int base; char ret; };
    const Family instance[] = {{34, 'L'}, {37, 'Z'}, {40, 'B'}, {43, 'C'}, {46, 'S'}, {49, 'I'},
                               {52, 'J'}, {55, 'F'}, {58, 'D'}, {61, 'V'}};
    const Family statics[] = {{114, 'L'}, {117, 'Z'}, {120, 'B'}, {123, 'C'}, {126, 'S'}, {129, 'I'},
                              {132, 'J'}, {135, 'F'}, {138, 'D'}, {141, 'V'}};
    for (const Family& f : instance) {
        char r = f.ret;
        def(f.base + 0, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::Varargs, 3), r); });
        def(f.base + 1, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::VaList, 3), r); });
        def(f.base + 2, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::JValues, 3), r); });
    }
    for (const Family& f : statics) {
        char r = f.ret;
        def(f.base + 0, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::Varargs, 3), r); });
        def(f.base + 1, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::VaList, 3), r); });
        def(f.base + 2, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(2), ArgMode::JValues, 3), r); });
    }
    // CallNonvirtual<Type>Method (env, obj, clazz, methodID, ...)
    for (int k = 0; k < 10; k++) {
        char r = instance[k].ret;
        int b = 64 + k * 3;
        def(b + 0, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(3), ArgMode::Varargs, 4), r); });
        def(b + 1, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(3), ArgMode::VaList, 4), r); });
        def(b + 2, [r](Cpu& c) { set_return(c, invoke(c, c.r(1), c.r(3), ArgMode::JValues, 4), r); });
    }

    def(4, [](Cpu& c) { c.ret(0x00010006); });  // GetVersion
    def(6, [](Cpu& c) { c.ret(find_class(str(c.r(1)))); });  // FindClass
    def(10, [](Cpu& c) { c.ret(find_class("java/lang/Object")); });  // GetSuperclass
    def(11, [](Cpu& c) { c.ret(1); });  // IsAssignableFrom
    def(13, [](Cpu& c) { c.ret(0); });  // Throw
    def(14, [](Cpu& c) { LOGE("JNI ThrowNew: %s", str(c.r(2))); c.ret(0); });
    def(15, [](Cpu& c) { c.ret(0); });  // ExceptionOccurred
    def(16, [](Cpu&) {});               // ExceptionDescribe
    def(17, [](Cpu&) {});               // ExceptionClear
    def(18, [](Cpu& c) { fatal("JNI FatalError: %s", str(c.r(1))); });
    def(19, [](Cpu& c) { c.ret(0); });        // PushLocalFrame
    def(20, [](Cpu& c) { c.ret(c.r(1)); });   // PopLocalFrame
    def(21, [](Cpu& c) { c.ret(c.r(1)); });   // NewGlobalRef
    def(22, [](Cpu&) {});                     // DeleteGlobalRef
    def(23, [](Cpu&) {});                     // DeleteLocalRef
    def(24, [](Cpu& c) { c.ret(c.r(1) == c.r(2) ? 1 : 0); });  // IsSameObject
    def(25, [](Cpu& c) { c.ret(c.r(1)); });   // NewLocalRef
    def(26, [](Cpu& c) { c.ret(0); });        // EnsureLocalCapacity
    def(27, [](Cpu& c) { c.ret(new_object(class_of(c.r(1)))); });  // AllocObject
    auto new_obj = [](Cpu& c) {
        u32 o = new_object(class_of(c.r(1)));
        c.ret(o);
    };
    def(28, new_obj);
    def(29, new_obj);
    def(30, new_obj);
    def(31, [](Cpu& c) { c.ret(find_class(class_of(c.r(1)))); });  // GetObjectClass
    def(32, [](Cpu& c) { c.ret(1); });                              // IsInstanceOf
    def(33, [](Cpu& c) { c.ret(get_method_id(c, false)); });        // GetMethodID
    def(113, [](Cpu& c) { c.ret(get_method_id(c, true)); });        // GetStaticMethodID

    auto get_field_id = [](Cpu& c) {
        FieldInfo fi{class_of(c.r(1)), str(c.r(2)), str(c.r(3))};
        std::lock_guard<std::recursive_mutex> l(g_lock);
        for (size_t i = 0; i < g_fields.size(); i++)
            if (g_fields[i].cls == fi.cls && g_fields[i].name == fi.name) { c.ret((u32)i + 1); return; }
        g_fields.push_back(fi);
        c.ret((u32)g_fields.size());
    };
    def(94, get_field_id);
    def(144, get_field_id);
    auto get_field = [](Cpu& c) {
        FieldInfo fi;
        {
            std::lock_guard<std::recursive_mutex> l(g_lock);
            u32 id = c.r(2);
            if (!id || id > g_fields.size()) { c.ret(0); return; }
            fi = g_fields[id - 1];
            auto it = g_static_fields.find(fi.cls + "." + fi.name);
            if (it != g_static_fields.end()) {
                if (fi.sig == "J" || fi.sig == "D") c.ret64(it->second.j);
                else c.ret(it->second.i);
                return;
            }
            if (g_warned.insert("field " + fi.cls + "." + fi.name).second)
                LOGI("JNI: unknown field %s.%s (%s)", fi.cls.c_str(), fi.name.c_str(), fi.sig.c_str());
        }
        c.ret64(0);
    };
    for (int i = 95; i <= 103; i++) def(i, get_field);   // Get<Type>Field
    for (int i = 145; i <= 153; i++) def(i, get_field);  // GetStatic<Type>Field
    for (int i = 104; i <= 112; i++) def(i, [](Cpu&) {});  // Set<Type>Field
    for (int i = 154; i <= 162; i++) def(i, [](Cpu&) {});  // SetStatic<Type>Field

    // Strings
    def(163, [](Cpu& c) {  // NewString(env, const jchar*, len)
        std::u32string w;
        for (u32 i = 0; i < c.r(2); i++) w.push_back(mem::r16(c.r(1) + i * 2));
        c.ret(new_string(mem::utf8(w)));
    });
    def(164, [](Cpu& c) { c.ret((u32)string_value(c.r(1)).size()); });  // GetStringLength (ASCII assumption)
    def(165, [](Cpu& c) {  // GetStringChars
        std::string s = string_value(c.r(1));
        u32 buf = mem::malloc((u32)s.size() * 2 + 2);
        for (size_t i = 0; i < s.size(); i++) mem::w16(buf + (u32)i * 2, (u8)s[i]);
        mem::w16(buf + (u32)s.size() * 2, 0);
        if (c.r(2)) mem::w8(c.r(2), 1);
        c.ret(buf);
    });
    def(166, [](Cpu& c) { mem::free(c.r(2)); });  // ReleaseStringChars
    def(167, [](Cpu& c) { c.ret(new_string(str(c.r(1)) ? str(c.r(1)) : "")); });  // NewStringUTF
    def(168, [](Cpu& c) { c.ret((u32)string_value(c.r(1)).size()); });  // GetStringUTFLength
    def(169, [](Cpu& c) {  // GetStringUTFChars
        if (c.r(2)) mem::w8(c.r(2), 1);
        c.ret(mem::strdup(string_value(c.r(1)).c_str()));
    });
    def(170, [](Cpu& c) { mem::free(c.r(2)); });  // ReleaseStringUTFChars
    def(221, [](Cpu& c) {  // GetStringUTFRegion(env, str, start, len, buf)
        std::string s = string_value(c.r(1));
        u32 start = c.r(2), len = c.r(3), buf = c.arg(4);
        std::string sub = start < s.size() ? s.substr(start, len) : "";
        memcpy(ptr(buf), sub.c_str(), sub.size() + 1);
    });

    // Arrays (byte arrays mostly)
    def(171, [](Cpu& c) { Obj* o = get(c.r(1)); c.ret(o ? (u32)o->bytes.size() : 0); });  // GetArrayLength
    def(176, [](Cpu& c) {  // NewByteArray
        Obj o{Kind::Array, "[B"};
        o.bytes.resize(c.r(1));
        c.ret(add_obj(o));
    });
    def(200, [](Cpu& c) {  // GetByteArrayRegion(env, arr, start, len, buf)
        Obj* o = get(c.r(1));
        if (o) memcpy(ptr(c.arg(4)), o->bytes.data() + c.r(2), c.r(3));
    });
    def(208, [](Cpu& c) {  // SetByteArrayRegion
        Obj* o = get(c.r(1));
        if (o) memcpy(o->bytes.data() + c.r(2), ptr(c.arg(4)), c.r(3));
    });

    def(215, [](Cpu& c) { c.ret(0); });  // RegisterNatives
    def(217, [](Cpu& c) { c.ret(0); });  // MonitorEnter
    def(218, [](Cpu& c) { c.ret(0); });  // MonitorExit
    def(219, [](Cpu& c) { mem::w32(c.r(1), g_vm); c.ret(0); });  // GetJavaVM
    def(228, [](Cpu& c) { c.ret(0); });  // ExceptionCheck
    def(229, [](Cpu& c) { c.ret(new_direct_buffer(c.r(1), c.r(2))); });  // NewDirectByteBuffer(addr, cap)
    def(230, [](Cpu& c) { Obj* o = get(c.r(1)); c.ret(o ? o->addr : 0); });  // GetDirectBufferAddress
    def(231, [](Cpu& c) { Obj* o = get(c.r(1)); c.ret(o ? o->capacity : 0); });  // GetDirectBufferCapacity
    def(232, [](Cpu& c) { c.ret(1); });  // GetObjectRefType -> local
}

}  // namespace

u32 new_string(const std::string& s) { return add_obj(Obj{Kind::String, "java/lang/String", s}); }

std::string string_value(u32 h) {
    Obj* o = get(h);
    return o && o->kind == Kind::String ? o->text : std::string();
}

u32 find_class(const std::string& name_in) {
    std::string name = name_in;
    for (auto& ch : name) if (ch == '.') ch = '/';
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;
    u32 h = add_obj(Obj{Kind::Class, name});
    g_classes[name] = h;
    LOGV("JNI FindClass %s -> 0x%x", name.c_str(), h);
    return h;
}

u32 new_object(const std::string& cls) { return add_obj(Obj{Kind::Object, cls}); }

u32 new_direct_buffer(u32 addr, u32 capacity) {
    Obj o{Kind::DirectBuffer, "java/nio/DirectByteBuffer"};
    o.addr = addr;
    o.capacity = capacity;
    return add_obj(o);
}

void define(const std::string& key, Method m) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_impls[key] = std::move(m);
}

void define_static_field(const std::string& key, Value v) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_static_fields[key] = v;
}

u32 env() { return g_env; }
u32 vm() { return g_vm; }

void init() {
    build_table();
    // JNIEnv: pointer to a pointer to the function table
    u32 table = mem::calloc(kNumFns, 4);
    for (int i = 0; i < kNumFns; i++) {
        HleFn f = g_fns[i];
        if (!f) {
            f = [i](Cpu& c) {
                LOGE("JNI: unimplemented JNIEnv function #%d (from %s)", i, symbolize(c.lr()).c_str());
                c.ret(0);
            };
        }
        mem::w32(table + i * 4, hle::add_dynamic("JNIEnv#" + std::to_string(i), f));
    }
    g_env = mem::malloc(4);
    mem::w32(g_env, table);

    // JavaVM: reserved0-2, DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon
    u32 vtab = mem::calloc(8, 4);
    auto give_env = [](Cpu& c) { if (c.r(1)) mem::w32(c.r(1), g_env); c.ret(0); };
    mem::w32(vtab + 3 * 4, hle::add_dynamic("JavaVM::DestroyJavaVM", [](Cpu& c) { c.ret(0); }));
    mem::w32(vtab + 4 * 4, hle::add_dynamic("JavaVM::AttachCurrentThread", give_env));
    mem::w32(vtab + 5 * 4, hle::add_dynamic("JavaVM::DetachCurrentThread", [](Cpu& c) { c.ret(0); }));
    mem::w32(vtab + 6 * 4, hle::add_dynamic("JavaVM::GetEnv", give_env));
    mem::w32(vtab + 7 * 4, hle::add_dynamic("JavaVM::AttachCurrentThreadAsDaemon", give_env));
    g_vm = mem::malloc(4);
    mem::w32(g_vm, vtab);
}

}  // namespace jni
