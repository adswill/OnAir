// CubeSat telemetry: the test signal (cubesat_gen.h) through the receiver (cubesat_rx.h): each framing alone and all together, with a
// carrier offset (Doppler) and noise, at two radio rates; every frame sent comes out with the bytes sent and the right framing; frames
// are named from the satellite table (callsign, frequency, the tracked satellite); noise alone gives no frame.
#include "dect2/cubesat/cubesat_gen.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::cubesat;
#include "../common/test_util.h"

static CubeTelemetry run(int mode, double rate, double snr, double cfo, double secs, bool noiseOnly, CubesatReceiver& r) {
    SynthConfig sc;
    sc.modeOpt[0] = mode; sc.snrDb = snr; sc.cfoHz = cfo;
    std::unique_ptr<ModeSynth> g = makeCubesatSynth(sc, rate);
    r.configure(rate);
    r.setSignalOffset(-cubesatTuning().tuneOffsetHz);
    genutil::NoiseSource ns(3);
    std::vector<cf32> buf(16384);
    CubeTelemetry last;
    uint64_t seq = 0;
    const size_t total = (size_t)(secs * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        if (noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.1f); } else g->generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        CubeTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

static void framings(int mode, double rate, double snr, double cfo, const char* what) {
    CubesatReceiver r;
    const double cyc = cubesatGenCycleSec(mode);
    const CubeTelemetry t = run(mode, rate, snr, cfo, cyc * 2.2, false, r);
    const std::vector<CubeGenFrame> sent = cubesatGenCycle(mode);
    int missing = 0;
    for (const CubeGenFrame& s : sent) {
        int seen = 0;
        for (const CubeFrame& f : t.frames) if (f.framing == s.framing && f.baud == s.baud && f.data == s.data) seen++;
        if (seen < 2) { missing++; printf("    missing: %s %d baud (%d of 2)\n", framingName(s.framing), s.baud, seen); }
    }
    int wrong = 0;
    for (const CubeFrame& f : t.frames) {
        bool ok = false;
        for (const CubeGenFrame& s : sent) ok |= f.framing == s.framing && f.data == s.data;
        if (!ok) { wrong++; printf("    wrong: %s %d baud, %zu bytes, rs %d\n", framingName(f.framing), f.baud, f.data.size(), f.rsErrors); }
    }
    printf("%-34s %5.2f Msps SNR %4.1f cfo %+6.0f: %3zu frames (%zu sent x2), bad %llu, rs fixed %llu, SNR %.1f dB, cfo %+.0f Hz\n", what, rate / 1e6, snr, cfo,
           t.frames.size(), sent.size(), (unsigned long long)t.blocksBad, (unsigned long long)t.rsFixed, t.snrDb, t.cfoHz);
    CHECK(missing == 0, "%s: %d frames missing", what, missing);
    CHECK(wrong == 0, "%s: %d wrong frames", what, wrong);
}

int main() {
    const double R = cubesatTuning().sampleRate;
    for (int k = 0; k < kNumFramings; k++) framings(k + 1, R, 25, 3000, framingName(k));
    framings(0, R, 25, -4500, "every framing");
    framings(0, 2.4e6, 22, 8000, "every framing, 2.4 Msps, Doppler");
    framings(0, R, 18, -1500, "every framing, weaker");
    framings(0, R, 25, -11000, "every framing, 11 kHz Doppler");
    // 4.0 (docs/SENSITIVITY-4.0-A3.md): the Bell 202 path has a 4.5 kHz channel of its own; 3.9 lost frames from 8 dB down (every frame twice)
    framings(1, R, 6, 0, "AX.25 AFSK 1200, weak");
    framings(1, R, 6, 3000, "AX.25 AFSK 1200, weak, +3 kHz");
    framings(1, R, 6, -11000, "AX.25 AFSK 1200, weak, -11 kHz Doppler");

    {   // naming: the ISS by its callsign, the others from the tracked satellite
        CubesatReceiver r;
        r.setTracked(99001, "TEST-SAT");
        std::vector<SatDef> sats = defaultSats();
        sats.push_back({"Mode 5 sat", 99001, 437.0, Ax100Mode5, 9600, ""});
        r.setSatellites(sats);
        const CubeTelemetry t = run(0, R, 30, 0, cubesatGenCycleSec(0) * 1.2, false, r);
        int iss = 0, other = 0, unnamed = 0;
        for (const CubeFrame& f : t.frames) {
            if (f.framing == Ax25Afsk1200) iss += f.sat == "ISS (ARISS packet)" && f.norad == 25544;
            else if (f.sat == "Mode 5 sat" && f.norad == 99001) other++;
            else unnamed++;
        }
        CHECK(iss >= 1 && other >= 4 && unnamed == 0, "naming: iss %d other %d unnamed %d", iss, other, unnamed);
        CHECK(t.tracked == "TEST-SAT" && t.trackedNorad == 99001, "tracked");
    }
    {   // naming from the dial frequency
        CubesatReceiver r;
        std::vector<SatDef> sats = {{"Dial sat", 99002, 436.5, Ax25G3ruh, 9600, ""}};
        r.setSatellites(sats);
        r.setDialHz(436.505e6);
        const CubeTelemetry t = run(2, R, 30, 0, cubesatGenCycleSec(2) * 1.5, false, r);
        CHECK(!t.frames.empty() && t.frames.front().sat == "Dial sat", "naming from the dial");
    }
    {   // noise alone
        CubesatReceiver r;
        const CubeTelemetry t = run(0, R, 0, 0, 40, true, r);
        printf("noise alone: %zu frames, %llu bad, state %d\n", t.frames.size(), (unsigned long long)t.blocksBad, t.state);
        CHECK(t.frames.empty() && t.blocksOk == 0, "noise gave %zu frames", t.frames.size());
        CHECK(t.state < 2, "noise state %d", t.state);
    }
    printf(fails ? "cubesat rx: %d FAILED\n" : "cubesat rx: all passed\n", fails);
    return fails ? 1 : 0;
}
