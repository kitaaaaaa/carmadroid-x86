#pragma once
#include "../common.h"
#include <functional>
#include <string>
#include <vector>

namespace jni {

// A Java value as passed to/returned from host method implementations.
union Value {
    u32 i;   // int / boolean / char / short / byte / object handle
    u64 j;
    float f;
    double d;
};

using Method = std::function<Value(u32 self, const std::vector<Value>& args)>;

void init();
u32 env();  // guest JNIEnv*
u32 vm();   // guest JavaVM*

u32 new_string(const std::string& s);
std::string string_value(u32 handle);
u32 find_class(const std::string& name);  // slashes, e.g. "java/lang/String"
u32 new_object(const std::string& class_name);
u32 new_direct_buffer(u32 guest_addr, u32 capacity);

// Implement a Java method on the host. Key: "pkg/Class.name" (signature ignored).
void define(const std::string& class_and_name, Method m);
// Static int/object fields: "pkg/Class.field"
void define_static_field(const std::string& class_and_field, Value v);

}  // namespace jni
