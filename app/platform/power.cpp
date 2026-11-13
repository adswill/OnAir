// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Battery state and the process's load per thread (power.h). The macOS window and App Nap parts are in power_cocoa.mm.
#include "power.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

namespace power {

namespace {
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool readBattery() {
#if defined(__APPLE__)
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    const bool bat = type && CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    CFRelease(info);
    return bat;
#elif defined(_WIN32)
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
#else
    // on battery when a battery discharges and no mains supply is online
    bool mains = false, discharging = false;
    if (DIR* d = opendir("/sys/class/power_supply")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string base = std::string("/sys/class/power_supply/") + e->d_name;
            std::string type, v;
            { std::ifstream f(base + "/type"); std::getline(f, type); }
            if (type == "Mains") { std::ifstream f(base + "/online"); std::getline(f, v); if (v == "1") mains = true; }
            else if (type == "Battery") { std::ifstream f(base + "/status"); std::getline(f, v); if (v == "Discharging") discharging = true; }
        }
        closedir(d);
    }
    return discharging && !mains;
#endif
}
}   // namespace

bool onBattery() {
    static double at = -1e9;
    static bool bat = false;
    const double t = nowSec();
    if (t - at >= 10) { at = t; bat = readBattery(); }
    return bat;
}

#if !defined(__APPLE__)
bool windowOccluded(GLFWwindow*) { return false; }
void setReceiving(bool) {}
#endif

bool sampleUsage(Usage& u) {
    static double lastT = 0;
    const double t = nowSec();
    const double dt = lastT > 0 ? t - lastT : 0;
    lastT = t;
    u = Usage();
#if defined(__APPLE__)
    static std::map<uint64_t, uint64_t> lastCpu;   // thread id -> user + system time in ns
    static uint64_t lastProc = 0, lastWake = 0;
    std::map<uint64_t, uint64_t> cpu;
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            thread_extended_info_data_t ex{};
            mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
            thread_identifier_info_data_t id{};
            mach_msg_type_number_t ci = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&ex, &c) == KERN_SUCCESS &&
                thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ci) == KERN_SUCCESS) {
                const uint64_t ns = ex.pth_user_time + ex.pth_system_time;
                cpu[id.thread_id] = ns;
                auto it = lastCpu.find(id.thread_id);
                if (dt > 0 && it != lastCpu.end() && ns >= it->second) {
                    ThreadCpu tc;
                    tc.name = ex.pth_name[0] ? ex.pth_name : (i == 0 ? "main" : "(unnamed)");
                    tc.pct = (float)(100.0 * (ns - it->second) * 1e-9 / dt);
                    u.threads.push_back(std::move(tc));
                }
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
    }
    lastCpu.swap(cpu);
    rusage_info_v4 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t*)&ri) == 0) {
        const uint64_t proc = ri.ri_user_time + ri.ri_system_time;   // mach absolute units on Apple silicon: converted below
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        const uint64_t procNs = tb.denom ? proc * tb.numer / tb.denom : proc;
        const uint64_t wake = ri.ri_pkg_idle_wkups + ri.ri_interrupt_wkups;
        if (dt > 0 && lastProc) {
            u.processPct = (float)(100.0 * (procNs - lastProc) * 1e-9 / dt);
            u.wakeupsPerSec = (float)((wake - lastWake) / dt);
        }
        lastProc = procNs; lastWake = wake;
    }
#elif defined(_WIN32)
    static uint64_t lastProc = 0;
    FILETIME c, e, k, us;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
        const uint64_t v = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime);   // 100 ns
        if (dt > 0 && lastProc) u.processPct = (float)(100.0 * (v - lastProc) * 1e-7 / dt);
        lastProc = v;
    }
#else
    static std::map<long, uint64_t> lastCpu;   // tid -> utime + stime in clock ticks
    static uint64_t lastSw = 0;
    const double tick = (double)sysconf(_SC_CLK_TCK);
    std::map<long, uint64_t> cpu;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const long tid = atol(e->d_name);
            const std::string base = std::string("/proc/self/task/") + e->d_name;
            std::ifstream f(base + "/stat");
            std::string line;
            if (!std::getline(f, line)) continue;
            const size_t rp = line.rfind(')');
            if (rp == std::string::npos) continue;
            std::istringstream is(line.substr(rp + 2));
            std::string fld;
            unsigned long long ut = 0, st = 0;
            for (int i = 3; i <= 15 && is >> fld; i++) { if (i == 14) ut = std::stoull(fld); if (i == 15) st = std::stoull(fld); }
            const uint64_t v = ut + st;
            cpu[tid] = v;
            auto it = lastCpu.find(tid);
            if (dt > 0 && it != lastCpu.end() && v >= it->second) {
                ThreadCpu tc;
                std::ifstream cf(base + "/comm");
                std::getline(cf, tc.name);
                tc.pct = (float)(100.0 * (v - it->second) / tick / dt);
                u.threads.push_back(std::move(tc));
            }
        }
        closedir(d);
    }
    if (dt > 0) { float s = 0; for (const ThreadCpu& tc : u.threads) s += tc.pct; u.processPct = s; }
    lastCpu.swap(cpu);
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        uint64_t sw = 0;
        while (std::getline(f, line))
            if (line.rfind("voluntary_ctxt_switches:", 0) == 0 || line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) sw += std::stoull(line.substr(line.find(':') + 1));
        if (dt > 0 && lastSw) u.wakeupsPerSec = (float)((sw - lastSw) / dt);
        u.wakeupsName = "context switches";
        lastSw = sw;
    }
#endif
    std::sort(u.threads.begin(), u.threads.end(), [](const ThreadCpu& a, const ThreadCpu& b) { return a.pct > b.pct; });
    return dt > 0;
}

}   // namespace power
