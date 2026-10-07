// Helpers for the GNSS tests: run the simulated sky through the receiver in chunks, with the impairments of a radio, and compare what comes out with the truth.
#pragma once
#include "gnss_rx.h"
#include "gnss_sim.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>

#include <ctime>

// a sanitised build is ten times slower than real time: the speed checks are skipped there
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GNSS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define GNSS_SANITIZED 1
#endif
#endif
#ifndef GNSS_SANITIZED
#define GNSS_SANITIZED 0
#endif

namespace dect2 {
namespace gnsstest {

// seconds of CPU time of the calling thread (the machine is shared with other builds: wall time would say how busy it is, not how heavy the receiver is)
inline double threadCpuSeconds() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

struct Run {
    GnssTelemetry tel;                                  // the last report
    std::vector<std::vector<GnssMeasurement>> meas;      // every measurement epoch
    double cpuSecs = 0;                                  // time spent in feed()
    double signalSecs = 0;
    double firstFix = -1;
    std::vector<std::string> log;
    std::vector<GnssTelemetry> reports;                  // one in about two seconds
    GpsEphemeris eph[33];                                // what the receiver had decoded at the end
    bool hasEph[33] = {};
    int week[33] = {};
    GpsAlmanac alm[33];
    GpsIono iono;
    GpsUtc utc;
    bool hasIono = false;
};

struct Options {
    double rate = 4e6;
    double secs = 40;
    size_t chunk = 65536;
    bool quantise8 = true;                               // round to 8 bits like a HackRF
    double centerMhz = 1575.42;
    // called on every chunk before the receiver gets it: sample offset of its first sample; may change or shorten it (return the number of samples to feed)
    std::function<size_t(std::vector<cf32>&, size_t)> hook;
    // called after every chunk with the receiver (for reset() in the middle and so on)
    std::function<void(GnssReceiver&, size_t)> after;
    double reportEvery = 2.0;
};

inline Run run(GnssSim& sim, const Options& o) {
    Run r;
    GnssReceiver rx;
    rx.configure(o.rate);
    rx.setCenterMhz(o.centerMhz);
    rx.setWeekReference(2400);
    rx.setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    rx.setMeasurementCallback([&](const std::vector<GnssMeasurement>& m) { r.meas.push_back(m); });
    std::vector<cf32> buf(o.chunk);
    const size_t total = (size_t)(o.secs * o.rate);
    uint64_t seq = 0;
    double nextRep = o.reportEvery;
    for (size_t done = 0; done < total; done += o.chunk) {
        const size_t n = std::min(o.chunk, total - done);
        buf.resize(n);
        sim.generate(buf.data(), n);
        if (o.quantise8)
            for (auto& v : buf) v = cf32(std::max(-1.f, std::min(1.f, std::round(v.real() * 127.f) / 127.f)), std::max(-1.f, std::min(1.f, std::round(v.imag() * 127.f) / 127.f)));
        size_t m = n;
        if (o.hook) m = o.hook(buf, done);
        const double t0 = threadCpuSeconds();
        if (m > 0) rx.feed(buf.data(), m);
        r.cpuSecs += threadCpuSeconds() - t0;
        GnssTelemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq; r.tel = t;
            if (r.firstFix < 0 && t.fix.valid) r.firstFix = t.fix.firstFixSecs;
            if (t.signalSecs >= nextRep) { r.reports.push_back(t); nextRep += o.reportEvery; }
        }
        if (o.after) o.after(rx, done + n);
    }
    r.signalSecs = o.secs;
    for (int p = 1; p <= 32; p++) {
        r.hasEph[p] = rx.getEphemeris(p, r.eph[p], &r.week[p]);
        rx.getAlmanac(p, r.alm[p]);
    }
    r.hasIono = rx.getIonoUtc(r.iono, r.utc);
    return r;
}

inline double distance(const double* a, const double* b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// horizontal and vertical error of a fix against the simulated receiver position, metres
inline void fixError(const GnssSim& sim, const GnssFix& f, double* horiz, double* vert) {
    const double* r = sim.receiverEcef();
    double lat, lon, h;
    ecefToLla(r, &lat, &lon, &h);
    const double dlat = (f.latDeg - lat) * 3.14159265358979 / 180.0 * 6378137.0;
    const double dlon = (f.lonDeg - lon) * 3.14159265358979 / 180.0 * 6378137.0 * std::cos(lat * 3.14159265358979 / 180.0);
    *horiz = std::sqrt(dlat * dlat + dlon * dlon);
    *vert = f.heightM - h;
}

// pseudorange errors of the measurements made after `from` seconds, per satellite: the receiver's transmit-time reading against the ideal one, in metres
struct RangeStats { int n = 0; double mean = 0, rms = 0, maxAbs = 0; };
inline RangeStats rangeError(const GnssSim& sim, const Run& r, int prn, double from) {
    RangeStats s;
    double sum = 0, sum2 = 0;
    for (auto& epoch : r.meas)
        for (auto& m : epoch) {
            if (m.prn != prn || m.rxTime < from) continue;
            const size_t i = (size_t)(prn - 1);
            const double tTrue = sim.trueTime(m.rxTime);
            const double e = (m.txTow - sim.svTransmitTime(i, tTrue)) * 299792458.0;
            sum += e; sum2 += e * e; s.n++;
            s.maxAbs = std::max(s.maxAbs, std::fabs(e));
        }
    if (s.n) { s.mean = sum / s.n; s.rms = std::sqrt(sum2 / s.n); }
    return s;
}

} // namespace gnsstest
} // namespace dect2
