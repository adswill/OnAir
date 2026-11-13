// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Power policy (dect2/power/power.h) and the idle wakeups of the always-on background threads.
//
// Part 1: the policy: Low power mode Off / Auto / On, Save power when hidden, rate scaling, thread QoS by name, App Nap hold counting.
// Part 2: wakeups per second with the remote API, the share server, the feeds (ADS-B servers) and the push-feed targets switched on and nobody
// connected. Counted by the OS: macOS task_power_info interrupt wakeups, Linux voluntary context switches of every thread of the process (Windows:
// not counted, the part is skipped). Prints "idle wakeups/s: N" so docs/PERFORMANCE-POWER.md can quote it; fails above a generous limit.
// Define POWER_TEST_NO_API to leave the remote API out (a standalone build without the Rust net crate).
// Environment: DECT2_IDLE_SECONDS (default 3) = measuring time.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "dect2/net/feeds.h"
#ifndef POWER_TEST_NO_API
#include "dect2/net/remote_api.h"
#endif
#include "dect2/net/share.h"
#include "dect2/power/power.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fstream>
#endif

using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static long long wakeups() {
#if defined(__APPLE__)
    task_power_info_data_t i{};
    mach_msg_type_number_t n = TASK_POWER_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO, (task_info_t)&i, &n) != KERN_SUCCESS) return -1;
    return (long long)i.task_interrupt_wakeups;
#elif defined(__linux__)
    long long sum = 0;
    DIR* d = opendir("/proc/self/task");
    if (!d) return -1;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::ifstream in(std::string("/proc/self/task/") + e->d_name + "/status");
        std::string line;
        while (std::getline(in, line)) if (line.rfind("voluntary_ctxt_switches:", 0) == 0) sum += atoll(line.c_str() + 24);
    }
    closedir(d);
    return sum;
#else
    return -1;
#endif
}

#ifndef POWER_TEST_NO_API
struct NullHost : RemoteApiHost {
    std::string statusJson() override { return "{}"; }
    std::string devicesJson() override { return "[]"; }
    std::string modesJson() override { return "[]"; }
    std::string telemetryJson(uint64_t&) override { return "{}"; }
    std::string spectrumJson(int, uint64_t&) override { return "{}"; }
    bool start(int, std::string&) override { return false; }
    bool stop(std::string&) override { return false; }
    bool setMode(int, const std::string&, std::string&) override { return false; }
    bool setFrequencyHz(double, std::string&) override { return false; }
    bool setSampleRateHz(double, std::string&) override { return false; }
    bool setGainDb(double, std::string&) override { return false; }
};

#endif

int main() {
    // ---- policy
    power::Settings s;
    s.lowPower = power::LowPowerMode::Off;
    power::setSettings(s);
    CHECK(!power::lowPower());
    CHECK(power::spectrumRateScale() == 1.0);
    CHECK(power::maxFftSize(8192) == 8192);
    s.lowPower = power::LowPowerMode::On;
    power::setSettings(s);
    CHECK(power::lowPower());
    CHECK(power::spectrumRateScale() == 0.5);
    CHECK(power::maxFftSize(8192) == 4096);
    CHECK(power::maxFftSize(1024) == 1024);   // small sizes stay
    s.lowPower = power::LowPowerMode::Auto;
    power::setSettings(s);
    power::refreshNow();
    const power::Probe pr = power::probePlatform();
    CHECK(power::lowPower() == (pr.onBattery || pr.thermalSerious));
    std::printf("this machine: onBattery=%d thermalSerious=%d\n", (int)pr.onBattery, (int)pr.thermalSerious);

    s.saveWhenHidden = power::HiddenPolicy::Always;
    power::setSettings(s);
    power::setAppHidden(false);
    CHECK(!power::pauseWhenHidden());
    power::setAppHidden(true);
    CHECK(power::pauseWhenHidden());
    s.saveWhenHidden = power::HiddenPolicy::Off;
    power::setSettings(s);
    CHECK(!power::pauseWhenHidden());
    s.saveWhenHidden = power::HiddenPolicy::OnBattery;
    power::setSettings(s);
    power::refreshNow();
    CHECK(power::pauseWhenHidden() == pr.onBattery);
    power::setAppHidden(false);

    CHECK(power::qosForThreadName("onair-msgdb") == power::Qos::Background);
    CHECK(power::qosForThreadName("onair-scanner") == power::Qos::Background);
    CHECK(power::qosForThreadName("onair-rx2") == power::Qos::Utility);
    CHECK(power::qosForThreadName("onair-ui") == power::Qos::Interactive);
    {
        CHECK(power::receivingActivityCount() == 0);
        power::ReceivingActivity a;
        { power::ReceivingActivity b; CHECK(power::receivingActivityCount() == 2); }
        CHECK(power::receivingActivityCount() == 1);
    }
    CHECK(power::receivingActivityCount() == 0);

    // ---- idle wakeups
    if (wakeups() < 0) { std::printf("wakeups not countable on this platform: skipped\n"); return fails ? 1 : 0; }
#ifndef POWER_TEST_NO_API
    NullHost host;
    RemoteApiServer api(host);
    RemoteApiConfig ac; ac.on = true; ac.port = 0;
    CHECK(api.configure(ac));
#endif
    ShareConfig sc; sc.on = true; sc.port = 0;
    ShareServer::get().configure(sc);
    FeedConfig fc;
    fc.on[kFeedSbs] = fc.on[kFeedBeast] = true;
    fc.sbsPort = fc.beastPort = fc.kissPort = 0;
    Feeds::get().configure(fc);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // let the threads settle
    const char* e = getenv("DECT2_IDLE_SECONDS");
    const int secs = e && atoi(e) > 0 ? atoi(e) : 3;
    const long long a = wakeups();
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    const long long b = wakeups();
    const double perSec = (double)(b - a) / secs;
    std::printf("idle wakeups/s: %.1f (remote API + share server + ADS-B feed servers on, nobody connected)\n", perSec);
    CHECK(perSec < 40.0);   // before the change this was about 90 (50 ms accept poll, 20 ms select, 40 ms feed loop)
    Feeds::get().stop();
    ShareConfig off; off.on = false;
    ShareServer::get().configure(off);
#ifndef POWER_TEST_NO_API
    ac.on = false;
    api.configure(ac);
#endif
    std::printf(fails ? "power idle: FAILED\n" : "power idle: OK\n");
    return fails ? 1 : 0;
}
