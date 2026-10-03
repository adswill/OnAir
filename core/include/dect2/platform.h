// Small OS abstractions used by the core: thread priority.
#pragma once

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
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dect2 {

enum class ThreadPriority {
    Realtime,    // the sample path: must never wait for decoders or the UI
    High,        // playback threads
    Background   // heavy decoding that can yield to the above
};

// Best effort: failures (for example missing privileges for a raised priority) are ignored.
inline void setThreadPriority(ThreadPriority p) {
#if defined(__APPLE__)
    switch (p) {
    case ThreadPriority::Realtime: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); break;
    case ThreadPriority::High: pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); break;
    case ThreadPriority::Background: pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); break;
    }
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), p == ThreadPriority::Realtime ? THREAD_PRIORITY_TIME_CRITICAL : p == ThreadPriority::High ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    // nice values are per thread on Linux; lowering the value needs privileges, raising it never does
    const int prio = p == ThreadPriority::Realtime ? -5 : p == ThreadPriority::High ? -2 : 5;
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), prio);
#else
    (void)p;
#endif
}

// The Windows build is compiled for AVX2 (no runtime dispatch there): false on a processor that cannot run it.
inline bool cpuSupportsBuild() {
#if defined(DECT2_REQUIRES_AVX2) && defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return true;
#endif
}

} // namespace dect2
