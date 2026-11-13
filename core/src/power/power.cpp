// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/power/power.h"
#include "dect2/common/thread_name.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fstream>
#endif

namespace dect2::power {

// App Nap hold (macOS: power_apple.mm; nothing elsewhere)
void activityBegin();
void activityEnd();

namespace {
std::mutex gMu;
Settings gSet;
std::atomic<bool> gHidden{false};
bool gBatt = false, gHot = false, gHave = false;
std::chrono::steady_clock::time_point gAt{};
std::atomic<int> gActive{0};

void refreshIfStale() {
    std::lock_guard<std::mutex> lk(gMu);
    const auto now = std::chrono::steady_clock::now();
    if (gHave && now - gAt < std::chrono::seconds(20)) return;
    const Probe p = probePlatform();
    gBatt = p.onBattery; gHot = p.thermalSerious; gAt = now; gHave = true;
}
}  // namespace

Settings settings() { std::lock_guard<std::mutex> lk(gMu); return gSet; }
void setSettings(const Settings& s) { std::lock_guard<std::mutex> lk(gMu); gSet = s; }
void refreshNow() { std::lock_guard<std::mutex> lk(gMu); gHave = false; }

bool onBattery() { refreshIfStale(); std::lock_guard<std::mutex> lk(gMu); return gBatt; }
bool thermalSerious() { refreshIfStale(); std::lock_guard<std::mutex> lk(gMu); return gHot; }

bool lowPower() {
    const Settings s = settings();
    if (s.lowPower == LowPowerMode::On) return true;
    if (s.lowPower == LowPowerMode::Off) return false;
    return onBattery() || thermalSerious();
}
double spectrumRateScale() { return lowPower() ? 0.5 : 1.0; }
int maxFftSize(int n) { return lowPower() && n >= 2048 ? n / 2 : n; }

void setAppHidden(bool h) { gHidden.store(h, std::memory_order_relaxed); }
bool appHidden() { return gHidden.load(std::memory_order_relaxed); }
bool pauseWhenHidden() {
    if (!appHidden()) return false;
    switch (settings().saveWhenHidden) {
        case HiddenPolicy::Always: return true;
        case HiddenPolicy::OnBattery: return onBattery();
        default: return false;
    }
}

Qos qosForThreadName(const std::string& n) {
    switch (threadQosForName(n)) {
        case ThreadQos::Background: return Qos::Background;
        case ThreadQos::Utility: return Qos::Utility;
        default: return Qos::Interactive;
    }
}

void setThreadQos(Qos q) {
    dect2::setThreadQos(q == Qos::Background ? ThreadQos::Background : q == Qos::Utility ? ThreadQos::Utility : ThreadQos::Normal);
}

ReceivingActivity::ReceivingActivity() { if (gActive.fetch_add(1) == 0) activityBegin(); }
ReceivingActivity::~ReceivingActivity() { if (gActive.fetch_sub(1) == 1) activityEnd(); }
int receivingActivityCount() { return gActive.load(); }

#if !defined(__APPLE__)
void activityBegin() {}
void activityEnd() {}
#endif

#if defined(_WIN32)
Probe probePlatform() {
    Probe p;
    SYSTEM_POWER_STATUS s{};
    if (GetSystemPowerStatus(&s) && s.ACLineStatus == 0) p.onBattery = true;   // 0 = on battery; 255 = unknown stays false
    return p;
}
#elif defined(__linux__)
Probe probePlatform() {
    Probe p;
    bool anyBat = false, ac = false;
    if (DIR* d = opendir("/sys/class/power_supply")) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            auto rd = [&](const char* f) { std::ifstream in("/sys/class/power_supply/" + name + "/" + f); std::string v; std::getline(in, v); return v; };
            const std::string type = rd("type");
            if (type == "Battery") anyBat = true;
            else if (type == "Mains" && rd("online") == "1") ac = true;
        }
        closedir(d);
    }
    p.onBattery = anyBat && !ac;
    return p;
}
#elif !defined(__APPLE__)
Probe probePlatform() { return {}; }
#endif

}  // namespace dect2::power
