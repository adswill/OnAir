// CPU time for the speed limits in the tests. std::clock() is not CPU time everywhere: on Windows it is the wall clock since the program
// started (a busy runner then fails a limit that the code itself meets), on Linux and macOS the CPU time of the whole process (every thread).
// threadCpuSecs() is the time of the calling thread only: use it when the work timed runs on that thread (a receiver's feed(), a generator's
// generate()). processCpuSecs() adds every thread: use it when the code timed hands work to threads of its own.
// Windows counts thread and process times in scheduler ticks (about 15.6 ms): time stretches of a second or more there, not single blocks.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

#ifdef _WIN32
inline double cpuFiletimeSecs(const FILETIME& k, const FILETIME& u) {
    const auto v = [](const FILETIME& f) { return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime); };
    return (v(k) + v(u)) * 1e-7;   // 100 ns units
}
#endif

inline double threadCpuSecs() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return cpuFiletimeSecs(k, u);
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

inline double processCpuSecs() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return cpuFiletimeSecs(k, u);
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

// true where the clocks above tick too coarsely to time one short block
inline bool cpuClockCoarse() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
