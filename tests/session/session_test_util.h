// Helpers shared by the session tests: a fresh temporary folder, a session factory, a wait loop.
#pragma once
#include "dect2/session/session.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace sesstest {

namespace fs = std::filesystem;

inline int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// a new empty folder under the system temp folder; removed by the destructor
struct TempDir {
    fs::path path;
    TempDir() {
        std::error_code ec;
        std::random_device rd;
        path = fs::temp_directory_path(ec) / ("onair-sess-" + std::to_string(rd()) + "-" + std::to_string(nowMs()));
        fs::create_directories(path, ec);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    std::string str() const { const auto u = path.u8string(); return std::string(reinterpret_cast<const char*>(u.c_str()), u.size()); }
    std::string file(const std::string& name) const { return str() + "/" + name; }
};

inline dect2::sess::Session mk(uint16_t mode, const std::string& key, int64_t startMs, const std::string& title = std::string(), uint16_t receiver = 0) {
    dect2::sess::Session s;
    s.mode = mode;
    s.receiver = receiver;
    s.key = key;
    s.title = title.empty() ? key : title;
    s.startWall = startMs;
    s.freqHz = 438500000;
    return s;
}

inline bool waitFor(const std::function<bool()>& f, int ms = 5000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!f()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace sesstest
