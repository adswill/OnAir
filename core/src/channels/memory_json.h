// A small JSON reader for memory_io.cpp (the project has only a flat reader in the remote API). Reads any JSON value; limits: depth 24, 16 MB.
// Internal to core/src/channels; the writer side is plain string building in memory_io.cpp.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2::mj {

struct ValueKV;   // an object member; not std::pair<std::string, Value>, which clang rejects while Value is incomplete
struct Value {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<ValueKV> obj;   // in file order
    const Value* get(const std::string& k) const;
};
struct ValueKV { std::string first; Value second; };
inline const Value* Value::get(const std::string& k) const { for (const auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }

// false (with err) on a syntax error; `out` is then whatever was read before it (an object cut off at the end still has its first members)
bool parse(const std::string& text, Value& out, std::string& err);
std::string quote(const std::string& s);   // a JSON string literal; invalid UTF-8 bytes become '_'

} // namespace dect2::mj
