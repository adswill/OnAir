// gmtime_r / localtime_r under one name on POSIX and Windows.
#pragma once
#include <ctime>

namespace dect2 {
inline void gmTime(time_t t, struct tm* out) {
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}
inline void localTime(time_t t, struct tm* out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}
} // namespace dect2
