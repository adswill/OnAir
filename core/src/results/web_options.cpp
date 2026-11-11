// Mode options for OnAir Web: see web_options.h. The schema strings come from the generated web_options_gen.cpp.
#include "dect2/results/web_options.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace webschema {

namespace {
// The object of option `name` in a schema array: [begin, end) of its text
bool findOption(const char* schema, const std::string& name, std::string& obj) {
    const std::string key = "{\"n\":\"" + name + "\",";
    const char* p = std::strstr(schema, key.c_str());
    if (!p) return false;
    int depth = 0;
    const char* q = p;
    bool str = false;
    for (; *q; q++) {
        if (str) { if (*q == '\\' && q[1]) q++; else if (*q == '"') str = false; continue; }
        if (*q == '"') str = true;
        else if (*q == '{' || *q == '[') depth++;
        else if (*q == '}' || *q == ']') { if (--depth == 0) { q++; break; } }
    }
    obj.assign(p, (size_t)(q - p));
    return true;
}
std::string kindOf(const std::string& obj) {
    const size_t k = obj.find("\"k\":\"");
    if (k == std::string::npos) return std::string();
    const size_t e = obj.find('"', k + 5);
    return obj.substr(k + 5, e - k - 5);
}
std::vector<long long> valuesOf(const std::string& obj) {
    std::vector<long long> v;
    for (size_t p = obj.find("{\"v\":"); p != std::string::npos; p = obj.find("{\"v\":", p + 5)) v.push_back(std::atoll(obj.c_str() + p + 5));
    return v;
}
} // namespace

std::string allOptionsJson() {
    std::string s = "{";
    bool first = true;
    for (int sd : optionStandards()) {
        if (!first) s += ',';
        first = false;
        s += "\"" + std::to_string(sd) + "\":";
        s += optionsJson(sd);
    }
    return s + "}";
}

bool checkOption(int standard, const std::string& name, double value, std::string& why) {
    const char* schema = optionsJson(standard);
    std::string obj;
    if (!schema || name.empty() || !findOption(schema, name, obj)) { why = "this mode has no option " + name; return false; }
    if (!std::isfinite(value) || std::fabs(value) > 1e12) { why = "not a usable number"; return false; }
    const std::string k = kindOf(obj);
    const bool whole = value == std::floor(value);
    if (k == "bool" && value != 0 && value != 1) { why = name + " takes on (1) or off (0)"; return false; }
    if ((k == "int" || k == "choice" || k == "mask") && !whole) { why = name + " takes a whole number"; return false; }
    if (k == "int" && std::fabs(value) > 2147483647.0) { why = "too large"; return false; }
    if (k == "choice") {
        bool ok = false;
        for (long long x : valuesOf(obj)) ok = ok || x == (long long)value;
        if (!ok) { why = name + ": not one of the listed values"; return false; }
    }
    if (k == "mask") {
        long long all = 0;
        for (long long x : valuesOf(obj)) all |= x;
        const long long v = (long long)value;
        if (v < 0 || (v & ~all)) { why = name + ": only the listed bits"; return false; }
    }
    return true;
}

bool applyOption(Engine& e, int standard, const std::string& name, double value, std::string& why) {
    if (!checkOption(standard, name, value, why)) return false;
    if (!applyOptionUnchecked(e, standard, name, value)) { why = "this mode has no option " + name; return false; }
    return true;
}

} // namespace webschema
} // namespace dect2
