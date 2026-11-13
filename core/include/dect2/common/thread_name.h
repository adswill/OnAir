// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Names the threads of the app so profilers, debuggers, Activity Monitor, htop and crash reports show what each one does.
//
//   setThreadName("onair-dec")        names the calling thread (call first thing inside a thread function)
//   namedThread("onair-rec", fn, a..) std::thread that runs fn(a...) and is named before it starts working
//
// Linux keeps 15 characters (the kernel limit; longer names would make pthread_setname_np fail, so they are cut), macOS 63, Windows uses
// SetThreadDescription (Windows 10 1607+, looked up at run time so older systems just skip it), Android is Linux. Use short stable names
// "onair-<what>", with the receiver number appended where several run ("onair-rx2"). No behaviour change: naming never fails or throws.
#pragma once
#include <cstddef>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dect2 {

// The name as it is kept for a limit of maxLen characters (maxLen 15 = Linux)
inline std::string clipThreadName(const char* name, size_t maxLen = 15) {
    std::string s = name ? name : "";
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

// The longest name the platform keeps
constexpr size_t kThreadNameMax =
#if defined(__APPLE__)
    63;
#elif defined(_WIN32)
    256;
#else
    15;
#endif

inline void setThreadName(const char* name) {
    if (!name || !*name) return;
#if defined(_WIN32)
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const Fn fn = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription"))) : nullptr;
    }();
    if (!fn) return;
    wchar_t w[kThreadNameMax + 1];
    size_t n = 0;
    for (; name[n] && n < kThreadNameMax; n++) w[n] = (unsigned char)name[n];
    w[n] = 0;
    fn(GetCurrentThread(), w);
#elif defined(__APPLE__)
    pthread_setname_np(clipThreadName(name, kThreadNameMax).c_str());
#else
    pthread_setname_np(pthread_self(), clipThreadName(name, kThreadNameMax).c_str());
#endif
}

// The name the calling thread has now (empty when the platform cannot tell); used by the tests
inline std::string currentThreadName() {
#if defined(_WIN32)
    return {};
#else
    char b[80] = {0};
    if (pthread_getname_np(pthread_self(), b, sizeof b) != 0) return {};
    return b;
#endif
}

// The class of work a thread does by its name: 2 = background (saving, clean-up, watchdogs: may run late and slowly), 1 = utility (uploads
// and downloads nobody waits on), 0 = everything else (keeps what the system gives it; the sample path sets its own priority).
inline int threadNameClass(const char* name) {
    static const char* const kBackground[] = {"onair-results", "onair-tmsave", "onair-reap", "onair-plg-dog", "onair-scandb", "onair-msgdb",
                                              "onair-upd", "onair-ntp", "onair-discord", "onair-health"};
    static const char* const kUtility[] = {"onair-mapsend", "onair-mapsend2", "onair-feeds", "onair-feedtargets", "onair-psk", "onair-packs",
                                           "onair-updl", "onair-airport"};
    if (!name) return 0;
    for (const char* n : kBackground) if (std::string(n) == name) return 2;
    for (const char* n : kUtility) if (std::string(n) == name) return 1;
    return 0;
}

// The calling thread gets the scheduling its name asks for (macOS: a QoS class, so the efficiency cores take it and the timers coalesce;
// Linux: a higher nice value). Best effort; nothing happens for class 0.
inline void applyThreadNameClass(const char* name) {
    const int c = threadNameClass(name);
    if (!c) return;
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(c == 2 ? QOS_CLASS_BACKGROUND : QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), c == 2 ? 10 : 5);
#endif
}

// Scheduling class by name pattern (perf-power-030, docs/PERFORMANCE-POWER.md), used by dect2::power::setThreadQos() on request only.
// namedThread() below applies threadNameClass() instead (perf-ui-030): DSP threads keep what the system gives them, so the sample
// path is never moved to the efficiency cores by its name.
enum class ThreadQos { Normal, Utility, Background };
inline ThreadQos threadQosForName(const std::string& n) {
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (has("log") || has("scan") || has("msgdb") || has("hist") || has("update") || has("feed") || has("wspr") || has("psk") || has("discord") ||
        has("upload") || has("tile") || has("sweep") || has("survey"))
        return ThreadQos::Background;
    if (has("-rx") || has("-dec") || has("-dsp") || has("demod") || has("-fft") || has("-spec") || has("-pool") || has("-work")) return ThreadQos::Utility;
    return ThreadQos::Normal;
}
inline void setThreadQos(ThreadQos q) {
#if defined(__APPLE__)
    if (q == ThreadQos::Utility) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    else if (q == ThreadQos::Background) pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#elif defined(_WIN32)
    if (q == ThreadQos::Background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)q;
#endif
}

// std::thread that names itself first (and takes the scheduling class of its name, see threadNameClass). Arguments are copied like std::thread does.
template <class F, class... A>
std::thread namedThread(const char* name, F&& fn, A&&... args) {
    return std::thread([n = std::string(name ? name : ""), f = std::decay_t<F>(std::forward<F>(fn)),
                        tup = std::make_tuple(std::decay_t<A>(std::forward<A>(args))...)]() mutable {
        setThreadName(n.c_str());
        applyThreadNameClass(n.c_str());
        std::apply(std::move(f), std::move(tup));
    });
}

}  // namespace dect2
