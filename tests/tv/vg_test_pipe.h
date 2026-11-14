// A pipe / socket name of the test's own for the video generator link (vglink.h): never the one of a running OnAir.
#pragma once
#include <cstdlib>
#include <random>
#include <string>

inline std::string vgTestPipeName() {
    std::random_device rd;
    const std::string id = std::to_string(rd() % 1000000000u);
#ifdef _WIN32
    return "\\\\.\\pipe\\onair-videogen-test-" + id;
#else
    const char* t = std::getenv("TMPDIR");
    std::string dir = t && *t && std::string(t).size() < 60 ? t : "/tmp";
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir + "/onair-vg-test-" + id + ".sock";
#endif
}
