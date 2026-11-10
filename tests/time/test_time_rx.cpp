// Time signals: the test signal of every station (time_gen.h) through the receiver (time_rx.h), started part way into a minute, with a
// known time: the decoded time and flags, the seconds tick and the computer clock offset after the path delay; a weak signal, a carrier
// and sample clock error, another rate; noise alone gives nothing.
#include "dect2/dsp/gen_util.h"
#include "dect2/time/time_gen.h"
#include "dect2/time/time_proto.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::timesig;
#include "../common/test_util.h"
#include "../common/test_long.h"

static const double kLat = 54.687, kLon = 25.280;   // Vilnius

struct Run { TimeTelemetry first, last; double firstAt = -1; };

static Run run(TimeGenConfig c, double secs, bool noiseOnly = false, int listen = -1, size_t* heard = nullptr) {
    c.dialOffsetHz = -timeTuning().tuneOffsetHz;
    double la, lo;
    stationSite(c.station, la, lo, c.wwvh);
    c.delayMs = propagationMs(c.station, distanceKm(kLat, kLon, la, lo));
    TimeGenerator g(c);
    TimeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(-timeTuning().tuneOffsetHz);
    r.setStation(c.station);
    r.setClockMode(2, c.startUtc);
    r.setReceiverLocation(kLat, kLon, true);
    if (listen >= 0) { r.setSilent(true); r.setListen(listen != 0); r.setListenTap([heard](const float*, size_t n) { *heard += n; }); }
    genutil::NoiseSource ns(7);
    std::vector<cf32> buf(8192);
    Run out;
    const size_t total = (size_t)(secs * c.rate);
    uint64_t seq = 0;
    for (size_t done = 0; done < total; done += buf.size()) {
        if (noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.05f); } else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        TimeTelemetry t;
        if (r.telemetry(t, seq)) {
            seq = t.seq;
            if (t.timeValid && out.firstAt < 0) { out.firstAt = (double)(done + buf.size()) / c.rate; out.first = t; }
            out.last = std::move(t);
        }
    }
    return out;
}

static bool g_ut1 = false;   // BPM: the ticks follow UT1

static void station(int st, bool wwvh, double rate, double snr, double cfo, double ppm, const char* what) {
    TimeGenConfig c;
    c.station = st; c.wwvh = wwvh; c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm;
    // 2026-10-12 14:02:37 UTC: 23 s of a minute, then whole minutes
    c.startUtc = (double)(daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 2 * 60 + 37);
    c.opts.dut1 = -2;
    c.opts.ut1 = g_ut1;
    const double secs = st == Chu ? 75 : 175;
    const Run R = run(c, secs);
    const TimeTelemetry& t = R.last;
    printf("%-11s %-22s first time after %5.1f s, %s, ok %llu bad %llu, tick %.2f ms rms, offset %+.2f ms (avg %+.2f), SNR %.1f dB, cfo %+.1f Hz, depth %.2f, delay %.2f ms\n",
           stationName(st), what, R.firstAt, timeSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.tickJitterMs,
           t.clockOffsetMs, t.clockOffsetAvgMs, t.snrDb, t.cfoHz, t.depth, t.propDelayMs);
    CHECK(t.timeValid, "%s %s: no time", stationName(st), what);
    if (!t.timeValid) return;
    const double expect = c.startUtc + secs - c.delayMs * 1e-3;
    CHECK(std::fabs(t.utcNow - expect) < 1.5, "%s %s: time %.1f s off", stationName(st), what, t.utcNow - expect);
    CHECK(t.blocksBad == 0, "%s %s: %llu minutes rejected", stationName(st), what, (unsigned long long)t.blocksBad);
    // the "computer clock" here is the sample count, so a sample clock error shows as a drift of ppm over the last minute's mean time
    const double drift = ppm * 1e-6 * (secs - 30) * 1000;
    CHECK(std::fabs(t.clockOffsetAvgMs - drift) < (stationIsHf(st) ? 1.0 : 2.0), "%s %s: clock offset %.2f ms, drift %.2f", stationName(st), what, t.clockOffsetAvgMs, drift);
    // the first time: only after the first whole minute (DCF77: the marker at 59 ends the frame begun at the first second 0)
    if (st != Chu) CHECK(R.firstAt > 60 + 22 && R.firstAt < 60 + 23 + 62, "%s %s: first time at %.1f s", stationName(st), what, R.firstAt);
    CHECK(t.secondOfMinute >= 0, "%s: second of minute", stationName(st));
    const Fields& f = t.fields;
    switch (st) {
    case Dcf77: CHECK(f.dst == 1 && f.utcOffsetMin == 120 && f.weekday == 1 && f.day == 12 && f.month == 10 && f.year == 2026, "DCF77 fields"); break;
    case Msf: CHECK(f.dst == 1 && f.dut1 == -2 && f.day == 12, "MSF fields"); break;
    case Wwvb: CHECK(f.dstCode == 3 && f.dut1 == -2 && f.doy == 285 && f.leapYear == false, "WWVB fields"); break;
    case Jjy40: case Jjy60: CHECK(f.utcOffsetMin == 540 && f.hour == 23 && f.weekday == 1, "JJY fields"); break;
    case Wwv: CHECK(f.dstCode == 3 && f.dut1 == -2 && f.doy == 285, "WWV fields"); CHECK(t.hawaii == wwvh, "WWV / WWVH told apart"); break;
    case Chu: CHECK(f.doy == 285 && f.hour == 14, "CHU fields"); break;
    case Bpm: CHECK(f.dut1 == -2 && f.doy == 285 && f.hour == 14 && f.ut1 == g_ut1, "BPM fields"); break;
    }
    CHECK(t.confirmed || st == Chu, "%s: not confirmed", stationName(st));
}

int main() {
    // quick run: clean runs another case of the same station already covers (DCF77 at 256 ksps, WWV and BPM impaired, JJY 60 for JJY 40) and two of
    // the five impaired LF/HF runs are dropped, JJY 40 noise too; ONAIR_TEST_LONG=1 runs every station and case
    const bool L = testLong();
    for (int st : {Dcf77, Wwv}) {   // "Listen": 8000 audio samples a second when on, none when off
        TimeGenConfig c; c.station = st; c.rate = 64000; c.snrDb = 40; c.startUtc = 1.7e9;
        size_t on = 0, off = 0;
        run(c, 6, false, 1, &on); run(c, 6, false, 0, &off);
        CHECK(std::fabs((double)on - 6 * 8000) < 800 && off == 0, "time listen station %d: on %zu, off %zu samples", st, on, off);
    }
    if (L) station(Dcf77, false, 64000, 40, 0, 0, "clean");
    station(Msf, false, 64000, 40, 0, 0, "clean");
    station(Wwvb, false, 64000, 40, 0, 0, "clean");
    if (L) station(Jjy40, false, 64000, 40, 0, 0, "clean");
    station(Jjy60, false, 64000, 40, 0, 0, "clean");
    if (L) station(Wwv, false, 64000, 40, 0, 0, "clean");
    station(Wwv, true, 64000, 40, 0, 0, "WWVH");
    station(Chu, false, 64000, 45, 0, 0, "clean");
    if (L) station(Bpm, false, 64000, 40, 0, 0, "clean");
    station(Bpm, false, 64000, 25, 15, 10, "25 dB, 15 Hz, 10 ppm");
    g_ut1 = true;   // the ticks 0.2 s late (DUT1 = -0.2 s): the offset allows for it
    station(Bpm, false, 64000, 40, 0, 0, "UT1 minutes");
    g_ut1 = false;
    station(Dcf77, false, 256000, 40, 0, 0, "256 ksps");
    station(Dcf77, false, 64000, 12, 4, 30, "12 dB, 4 Hz, 30 ppm");
    if (L) station(Msf, false, 64000, 12, -3, -20, "12 dB, -3 Hz, -20 ppm");
    if (L) station(Wwvb, false, 64000, 15, 2, 10, "15 dB, 2 Hz, 10 ppm");
    station(Wwv, false, 64000, 25, 20, 10, "25 dB, 20 Hz, 10 ppm");
    // noise alone: nothing
    for (int st = Dcf77; st < kNumStations; st++) {
        if (!L && st == Jjy40) continue;
        TimeGenConfig c;
        c.station = st; c.rate = 64000;
        c.startUtc = 1.7e9;
        const Run R = run(c, 150, true);
        printf("%-11s noise: %s, ok %llu bad %llu\n", stationName(st), timeSummary(R.last).c_str(), (unsigned long long)R.last.blocksOk, (unsigned long long)R.last.blocksBad);
        CHECK(R.last.blocksOk == 0 && !R.last.timeValid, "%s: a time from noise", stationName(st));
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
