// The map tile server (Settings > Network and updates > Map tile server, 4.0): the address template, its check, and the cache folder of each
// server so tiles of different servers never mix. No ImGui, no App: tests/app/test_settings.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace tileurl {

constexpr const char* kDefault = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

// http(s) and all three of {z} {x} {y}
inline bool valid(const std::string& t) {
    const bool web = t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0;
    return web && t.find("{z}") != std::string::npos && t.find("{x}") != std::string::npos && t.find("{y}") != std::string::npos && t.size() < 400;
}
// the template the program uses: the user's when it is valid, else OpenStreetMap
inline std::string effective(const std::string& user) { return valid(user) ? user : std::string(kDefault); }

inline std::string expand(const std::string& t, int z, int x, int y) {
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t.compare(i, 3, "{z}") == 0) { o += std::to_string(z); i += 2; }
        else if (t.compare(i, 3, "{x}") == 0) { o += std::to_string(x); i += 2; }
        else if (t.compare(i, 3, "{y}") == 0) { o += std::to_string(y); i += 2; }
        else o += t[i];
    }
    return o;
}

// the cache folder name: "osm" for OpenStreetMap (the folder of every earlier version), "tiles-<hash>" for another server
inline std::string cacheName(const std::string& tpl) {
    if (tpl == kDefault) return "osm";
    uint32_t h = 2166136261u;
    for (unsigned char c : tpl) { h ^= c; h *= 16777619u; }
    char b[24];
    snprintf(b, sizeof b, "tiles-%08x", (unsigned)h);
    return b;
}

}  // namespace tileurl
