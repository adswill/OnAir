// Optional log of the WebSocket text traffic between OnAir and SDRconnect (nRSP-ST source): one timestamped line per message sent or received,
// plus open/close events. Off by default (Settings > Advanced > Log SDRconnect websocket traffic). IQ (binary frames) is never logged.
#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace dect2::wstrace {

struct State { std::mutex mu; bool on = false; std::string path; FILE* f = nullptr; };
inline State& state() { static State s; return s; }

inline void configure(bool on, const std::string& path) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.f) { fclose(s.f); s.f = nullptr; }
    s.on = on && !path.empty();
    s.path = path;
}
inline bool enabled() { State& s = state(); std::lock_guard<std::mutex> lk(s.mu); return s.on; }
// dir: "->" sent, "<-" received, "--" an event of OnAir's own. Long messages are cut at 2000 characters.
inline void line(const char* dir, const std::string& text) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.on) return;
    if (!s.f) { s.f = fopen(s.path.c_str(), "a"); if (!s.f) { s.on = false; return; } }
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const int ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[40];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    std::string x = text.size() > 2000 ? text.substr(0, 2000) + "..." : text;
    for (char& c : x) if (c == '\n' || c == '\r') c = ' ';
    fprintf(s.f, "%s %s %s\n", ts, dir, x.c_str());
    fflush(s.f);
}

} // namespace dect2::wstrace
