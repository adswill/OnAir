// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small OS abstractions used by the core: thread priority.
#pragma once
#include <initializer_list>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#include <cstdio>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dect2 {

enum class ThreadPriority {
    Realtime,    // the sample path: must never wait for decoders or the UI
    High,        // playback threads, the spectrum (the display must stay smooth)
    Decode,      // the shared pool's workers: heavy decoder work that should run on the fast cores
    Background   // heavy decoding that can yield to the above (the efficiency cores)
};

#if defined(__linux__)
// big.LITTLE boards (Raspberry Pi 5 is not one; RK3588, many phones and Android boxes are): the cores with the highest
// cpu_capacity (else the highest cpuinfo_max_freq) are the big ones. Empty mask when every core is the same (no affinity is set then).
struct CoreSets { cpu_set_t big, little; bool hetero = false; };
inline const CoreSets& coreSets() {
    static const CoreSets s = [] {
        CoreSets c; CPU_ZERO(&c.big); CPU_ZERO(&c.little);
        long v[CPU_SETSIZE]; int n = 0;
        const long ncpu = sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++, n++) {
            v[i] = -1;
            for (const char* f : {"cpu_capacity", "cpufreq/cpuinfo_max_freq"}) {
                char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", i, f);
                if (FILE* fp = fopen(path, "r")) { long x = -1; if (fscanf(fp, "%ld", &x) == 1) v[i] = x; fclose(fp); }
                if (v[i] > 0) break;
            }
        }
        long hi = -1, lo = -1;
        for (int i = 0; i < n; i++) if (v[i] > 0) { hi = hi < 0 || v[i] > hi ? v[i] : hi; lo = lo < 0 || v[i] < lo ? v[i] : lo; }
        if (hi > 0 && lo > 0 && hi > lo + lo / 8) {   // more than 12 % apart: really two kinds of core, not boost clocks
            c.hetero = true;
            for (int i = 0; i < n; i++) if (v[i] > 0) CPU_SET(i, v[i] * 8 >= hi * 7 ? &c.big : &c.little);
        }
        return c;
    }();
    return s;
}
#endif

// Best effort: failures (for example missing privileges for a raised priority) are ignored.
inline void setThreadPriority(ThreadPriority p) {
#if defined(__APPLE__)
    // Apple silicon: user-interactive and user-initiated run on the performance cores, utility and background on the efficiency cores
    switch (p) {
    case ThreadPriority::Realtime: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); break;
    case ThreadPriority::High: pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); break;
    case ThreadPriority::Decode: pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); break;
    case ThreadPriority::Background: pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); break;
    }
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), p == ThreadPriority::Realtime ? THREAD_PRIORITY_TIME_CRITICAL : p == ThreadPriority::High ? THREAD_PRIORITY_ABOVE_NORMAL
                                          : p == ThreadPriority::Decode ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
    if (p == ThreadPriority::Background) {
        // EcoQoS (Windows 11): hybrid Intel processors put the thread on the efficiency cores. Looked up at run time (older systems skip it).
        struct PowerThrottling { ULONG Version, ControlMask, StateMask; };
        using Fn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        static const Fn fn = [] {
            HMODULE k = GetModuleHandleW(L"kernel32.dll");
            return k ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadInformation"))) : nullptr;
        }();
        if (fn) { PowerThrottling t{1, 1, 1}; fn(GetCurrentThread(), 3 /* ThreadPowerThrottling */, &t, sizeof t); }
    }
#elif defined(__linux__)
    // nice values are per thread on Linux; lowering the value needs privileges, raising it never does
    const int prio = p == ThreadPriority::Realtime ? -5 : p == ThreadPriority::High ? -2 : p == ThreadPriority::Decode ? 0 : 5;
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), prio);
    // big.LITTLE: the sample path and the decoders on the big cores (the shared pool has one worker per big core then, threadpool.cpp);
    // background work stays free to use any core (the DVB-T2 LDPC runs there and needs every core it can get)
    const CoreSets& cs = coreSets();
    if (cs.hetero && p != ThreadPriority::Background && CPU_COUNT(&cs.big) > 0) (void)sched_setaffinity(0, sizeof cs.big, &cs.big);
#else
    (void)p;
#endif
}

// Builds made with DECT2_WIN_AVX2=ON are compiled for AVX2 throughout: false on a processor that cannot run them (the default build runs anywhere).
inline bool cpuSupportsBuild() {
#if defined(DECT2_REQUIRES_AVX2) && defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return true;
#endif
}

} // namespace dect2
