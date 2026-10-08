// Helpers for the GNSS tests: run the simulated sky through the receiver in chunks, with the impairments of a radio, and compare what comes out with the truth.
#pragma once
#include "gnss_rx.h"
#include "gnss_sim.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <algorithm>
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
    double firstLock = -1;                               // signal time of the first report with a locked satellite
    double maxFeedSecs = 0;                              // the longest single feed() call (a search that blocks the stream shows here)
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
    // called once after configure() (setters such as a remembered frequency offset)
    std::function<void(GnssReceiver&)> setup;
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
    if (o.setup) o.setup(rx);
    const size_t total = (size_t)(o.secs * o.rate);
    // Making the sky costs as much as the receiver's work, so a second thread makes it a few chunks ahead (only the sim, the 8-bit rounding and the hook
    // are touched there; none of them looks at the receiver). The receiver still gets the same chunks in the same order, and cpuSecs counts feed() only.
    struct Chunk { std::vector<cf32> buf; size_t n = 0, m = 0, done = 0; };
    std::deque<Chunk> q;
    std::mutex mu;
    std::condition_variable cv;
    std::thread maker([&] {
        for (size_t done = 0; done < total; done += o.chunk) {
            Chunk c;
            c.n = std::min(o.chunk, total - done);
            c.done = done;
            c.buf.resize(c.n);
            sim.generate(c.buf.data(), c.n);
            if (o.quantise8)
                for (auto& v : c.buf) v = cf32(std::max(-1.f, std::min(1.f, std::round(v.real() * 127.f) / 127.f)), std::max(-1.f, std::min(1.f, std::round(v.imag() * 127.f) / 127.f)));
            c.m = c.n;
            if (o.hook) c.m = o.hook(c.buf, done);
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return q.size() < 6; });
            q.push_back(std::move(c));
            cv.notify_all();
        }
    });
    uint64_t seq = 0;
    double nextRep = o.reportEvery;
    for (size_t got = 0; got < total; ) {
        Chunk c;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return !q.empty(); });
            c = std::move(q.front());
            q.pop_front();
            cv.notify_all();
        }
        got += c.n;
        const size_t done = c.done, n = c.n, m = c.m;
        std::vector<cf32>& buf = c.buf;
        const double t0 = threadCpuSeconds();
        if (m > 0) rx.feed(buf.data(), m);
        const double dt = threadCpuSeconds() - t0;
        r.cpuSecs += dt;
        r.maxFeedSecs = std::max(r.maxFeedSecs, dt);
        GnssTelemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq; r.tel = t;
            if (r.firstFix < 0 && t.fix.valid) r.firstFix = t.fix.firstFixSecs;
            if (r.firstLock < 0 && t.nTracked > 0) r.firstLock = t.signalSecs;
            if (t.signalSecs >= nextRep) { r.reports.push_back(t); nextRep += o.reportEvery; }
        }
        if (o.after) o.after(rx, done + n);
    }
    maker.join();
    r.signalSecs = o.secs;
    for (int p = 1; p <= 32; p++) {
        r.hasEph[p] = rx.getEphemeris(p, r.eph[p], &r.week[p]);
        rx.getAlmanac(p, r.alm[p]);
    }
    r.hasIono = rx.getIonoUtc(r.iono, r.utc);
    return r;
}

// What a cheap 8-bit radio (an RTL-SDR) does to the samples: I and Q gain and phase imbalance, a DC offset from the mixer, and unsigned 8-bit samples
// (u - 127.5) / 127.5 at a low level. The simulated noise rms sets how many steps of the converter the noise covers.
struct Radio8 {
    double gainDb = 0.6;         // I against Q
    double phaseDeg = 3.0;       // Q not quite at 90 degrees from I
    double dcI = 0.015, dcQ = -0.010;   // full scale = 1 (about 2 and 1.3 steps)
    double driftHzPerS = 0;      // the oscillator warming up: the carrier moves this many Hz per second (the sample clock is left alone)
};
inline std::function<size_t(std::vector<cf32>&, size_t)> radio8Hook(const Radio8& r, double rate) {
    return [r, rate](std::vector<cf32>& b, size_t done) -> size_t {
        const float g = (float)std::pow(10.0, r.gainDb / 20.0);
        const float sp = (float)std::sin(r.phaseDeg * 3.14159265358979 / 180.0), cp = (float)std::cos(r.phaseDeg * 3.14159265358979 / 180.0);
        auto q8 = [](float x) { return (std::max(0.f, std::min(255.f, std::round(x * 127.5f + 127.5f))) - 127.5f) / 127.5f; };
        for (size_t k = 0; k < b.size(); k++) {
            cf32 v = b[k];
            if (r.driftHzPerS != 0) {
                const double t = (double)(done + k) / rate;
                const double a = 2 * 3.14159265358979 * 0.5 * r.driftHzPerS * t * t;
                v *= cf32((float)std::cos(a), (float)std::sin(a));
            }
            const float i = v.real() * g, q = v.imag() * cp - v.real() * sp;
            b[k] = cf32(q8(i + (float)r.dcI), q8(q + (float)r.dcQ));
        }
        return b.size();
    };
}

inline int lockedCount(const GnssTelemetry& t) {
    int n = 0;
    for (auto& c : t.channels) n += c.state >= GnssChLocked;
    return n;
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
