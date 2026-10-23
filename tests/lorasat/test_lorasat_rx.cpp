// LoRa satellite receiver: the pass test signal (lorasat_gen.h: Doppler shift, Doppler rate through each frame, stretched symbol clock) through the receiver
// (lorasat_rx.h): frames at several points of a pass (approach, closest approach, recession) at 436.8 and 868.1 MHz and at SF7 - SF12, noise, a radio
// frequency error on top, the Doppler curve and rate measured from the frames, the difference tracking makes, the presets, the pass planner's name,
// the results file, and noise alone.
#include "dect2/lorasat/lorasat_gen.h"
#include "dect2/lorasat/lorasat_presets.h"
#include "dect2/lorasat/lorasat_rx.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static LoraSatTelemetry run(const LoraSatGenConfig& c, const LoraSatSettings& s, double secs, double centreHz = 0, const char* tracked = nullptr) {
    LoraSatGenerator g(c);
    LoraSatReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);
    if (centreHz > 0) r.setCenterHz(centreHz);
    if (tracked) r.setTracked(25544, tracked);
    r.setSettings(s);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.4) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    LoraSatTelemetry t;
    r.telemetry(t, 0);
    return t;
}

// frames of one setting at the given pass times, one every `gap` seconds
static std::vector<LoraSatGenFrame> framesAt(int sf, double bw, int sync, const std::vector<double>& passTimes, double gap, int bytes = 20) {
    const std::vector<LoraSatGenFrame> base = loraSatBeacons(sf, bw, sync, 1, 12, bytes);
    std::vector<LoraSatGenFrame> out;
    double t = 0.3;
    size_t i = 0;
    for (double pt : passTimes) {
        LoraSatGenFrame f = base[i++ % base.size()];
        f.startSec = t; f.passSec = pt;
        out.push_back(f);
        t += gap;
    }
    return out;
}

static LoraSatGenConfig config(double mhz, std::vector<LoraSatGenFrame> frames, double snr) {
    LoraSatGenConfig c;
    c.rate = 2e6;
    c.dialOffsetHz = -loraSatTuning().tuneOffsetHz;
    c.snrDb = snr; c.loop = false;
    c.pass.carrierMhz = mhz; c.pass.tcaSec = 300;
    c.frames = std::move(frames);
    return c;
}

static double seconds(const LoraSatGenConfig& c) {
    double end = 0;
    for (const auto& f : c.frames) end = std::max(end, f.startSec + lora::airSeconds(f.p, f.payload.size()));
    return end + 0.3;
}

// how many of the generated frames came out right (payload text equal, CRC good)
static size_t decoded(const LoraSatTelemetry& t, const LoraSatGenConfig& c) {
    size_t n = 0;
    for (const auto& g : c.frames) {
        const std::string text(g.payload.begin(), g.payload.end());
        for (const auto& f : t.log) {
            std::string a;
            for (char ch : f.ascii) a += ch;
            std::string want;
            for (char ch : text) want += (ch >= 32 && ch < 127) ? ch : '.';
            if (f.crcOk && a == want && f.sf == g.p.sf) { n++; break; }
        }
    }
    return n;
}

int main() {
    {   // presets and the pass model
        using namespace lorasat;
        const auto& v = presets();
        CHECK(v.size() >= 5, "presets: %zu", v.size());
        bool sane = true, honest = true;
        for (const auto& p : v) {
            sane &= p.mhz > 130 && p.mhz < 930 && (p.bwKhz == 41 || p.bwKhz == 62 || p.bwKhz == 125 || p.bwKhz == 250 || p.bwKhz == 500) && (p.sf == 0 || (p.sf >= 7 && p.sf <= 12)) && p.name[0] && p.note[0];
            honest &= p.conf != Documented;   // nothing here was checked against a real signal
        }
        CHECK(sane, "every preset has a plausible frequency, bandwidth and spreading factor, a name and a note");
        CHECK(honest, "no preset may claim to be documented: none was verified");
        CHECK(nearest(436.8) >= 0 && nearest(436.81, 15) == nearest(436.8) && nearest(150.0) < 0, "nearest preset");
        const double d436 = maxDopplerHz(436.8), d868 = maxDopplerHz(868.1);
        CHECK(d436 > 9500 && d436 < 11000 && d868 > 19000 && d868 < 22000, "Doppler limits %.0f %.0f Hz", d436, d868);
        const double r436 = maxDopplerRateHzS(436.8), r868 = maxDopplerRateHzS(868.1);
        CHECK(r436 > 100 && r436 < 160 && r868 > 200 && r868 < 320, "Doppler rate limits %.0f %.0f Hz/s", r436, r868);
        LoraSatPass p;   // the generator's own model must agree with those limits
        CHECK(std::fabs(p.dopplerHz(300)) < 1 && std::fabs(p.rateHzS(300) + p.carrierMhz * 1e6 * p.speedMS * p.speedMS / (299792458.0 * p.closestKm * 1e3)) < 0.5, "pass model at closest approach: %.2f Hz, %.2f Hz/s", p.dopplerHz(300), p.rateHzS(300));
        CHECK(p.dopplerHz(0) > 9000 && p.dopplerHz(600) < -9000 && p.dopplerHz(0) + p.dopplerHz(600) < 1, "pass model at the ends: %.0f %.0f", p.dopplerHz(0), p.dopplerHz(600));
    }
    {   // 436.8 MHz, SF11 / 125 kHz beacons at the approach, the closest approach and the recession; the Doppler curve and rate come out of the frames
        LoraSatGenConfig c = config(436.8, framesAt(11, 125e3, 0x12, {20, 150, 290, 300, 310, 450, 590}, 3.2), 10);
        LoraSatSettings s;
        const LoraSatTelemetry t = run(c, s, seconds(c), 0, "TEST-SAT 1");
        printf("  436.8 MHz SF11: %s, %zu points of the curve\n", loraSatSummary(t).c_str(), t.doppler.size());
        CHECK(decoded(t, c) == c.frames.size(), "436.8 MHz: %zu of %zu frames", decoded(t, c), c.frames.size());
        CHECK(t.blocksBad == 0, "%llu bad frames", (unsigned long long)t.blocksBad);
        bool curve = true, rate = true, names = !t.log.empty();
        for (const auto& f : t.log) {
            // which generated frame: the nearest start time
            const LoraSatGenFrame* g = nullptr;
            for (const auto& q : c.frames) if (!g || std::fabs(q.startSec + c.leadSec - f.sec) < std::fabs(g->startSec + c.leadSec - f.sec)) g = &q;
            const double dop = c.pass.dopplerHz(g->passSec);
            // the carrier at the first data symbol is a bit later than the frame start: the drift of about 13 symbols is allowed for
            const double tolHz = 80 + std::fabs(c.pass.rateHzS(g->passSec)) * 0.6;
            curve &= std::fabs(f.cfoStartHz - dop) < tolHz;
            rate &= std::fabs(f.rateHzS - c.pass.rateHzS(g->passSec)) < 12 + 0.3 * std::fabs(c.pass.rateHzS(g->passSec));
            names &= f.sat == "TEST-SAT 1";
        }
        CHECK(curve, "the measured carrier offsets must follow the Doppler curve");
        CHECK(rate, "the measured Doppler rates must follow the pass model");
        CHECK(names, "the pass planner's satellite name belongs on every frame");
        CHECK(t.tracked == "TEST-SAT 1" && t.trackedNorad == 25544, "tracked satellite in the telemetry");
        CHECK(t.doppler.size() == c.frames.size(), "Doppler points %zu", t.doppler.size());
    }
    {   // 137.055 MHz, SF8 / 41.7 kHz (the issue #229 settings), set by hand and through the preset at that frequency
        LoraSatGenConfig c = config(137.055, framesAt(8, 125e3 / 3, 0x12, {40, 200, 300, 420}, 1.5), 10);
        LoraSatSettings s; s.autoBw = false; s.bwKhz = 41; s.autoSf = false; s.sf = 8; s.sync = 0x12;
        const LoraSatTelemetry t = run(c, s, seconds(c));
        printf("  137.055 MHz SF8 41.7 kHz: %zu of %zu\n", decoded(t, c), c.frames.size());
        CHECK(decoded(t, c) == c.frames.size(), "137.055 MHz 41.7 kHz: %zu of %zu frames", decoded(t, c), c.frames.size());
        LoraSatSettings f;   // the preset picked by the dial
        const LoraSatTelemetry u = run(c, f, seconds(c), 137.055e6 + loraSatTuning().tuneOffsetHz);
        CHECK(decoded(u, c) == c.frames.size() && u.presetRunning >= 0, "137.055 MHz preset: %zu of %zu (preset %d)", decoded(u, c), c.frames.size(), u.presetRunning);
    }
    {   // 868.1 MHz: twice the shift and the rate; SF9 - SF12 around the closest approach (the worst case for a long frame)
        for (int sf : {9, 10, 11, 12}) {
            LoraSatGenConfig c = config(868.1, framesAt(sf, 125e3, 0x34, {285, 300, 315, 60, 540}, sf >= 11 ? 4.5 : 2.2, 16), 10);
            LoraSatSettings s;
            const LoraSatTelemetry t = run(c, s, seconds(c));
            printf("  868.1 MHz SF%d: %zu of %zu (rate at the closest approach %.0f Hz/s, shift at the ends %.0f Hz)\n", sf, decoded(t, c), c.frames.size(), c.pass.rateHzS(300), c.pass.dopplerHz(60));
            // SF12 with 276 Hz/s through a 2 s frame is the edge of what the decoder follows: one frame of five may be lost (measured: 4 of 5)
            CHECK(decoded(t, c) >= c.frames.size() - (sf == 12 ? 1 : 0), "868.1 MHz SF%d: %zu of %zu frames", sf, decoded(t, c), c.frames.size());
        }
    }
    {   // the other spreading factors and bandwidths of a 70 cm pass
        for (int sf : {7, 8}) {
            LoraSatGenConfig c = config(436.8, framesAt(sf, 125e3, 0x12, {30, 300, 570}, 1.2, 24), 8);
            LoraSatSettings s;
            const LoraSatTelemetry t = run(c, s, seconds(c));
            CHECK(decoded(t, c) == c.frames.size(), "SF%d: %zu of %zu frames", sf, decoded(t, c), c.frames.size());
        }
        LoraSatGenConfig c = config(436.8, framesAt(9, 250e3, 0x12, {100, 300, 500}, 1.2, 24), 8);
        LoraSatSettings s; s.autoBw = true;
        const LoraSatTelemetry t = run(c, s, seconds(c));
        CHECK(decoded(t, c) == c.frames.size(), "SF9 / 250 kHz with the bandwidth searched: %zu of %zu", decoded(t, c), c.frames.size());
    }
    {   // noise and a radio frequency error on top of the Doppler
        LoraSatGenConfig c = config(436.8, framesAt(11, 125e3, 0x12, {40, 300, 560}, 3.5), -5);
        c.cfoHz = 3000;
        LoraSatSettings s;
        const LoraSatTelemetry t = run(c, s, seconds(c));
        CHECK(decoded(t, c) >= 2, "-5 dB and +3 kHz radio error: %zu of %zu frames", decoded(t, c), c.frames.size());
        LoraSatGenConfig c2 = config(436.8, framesAt(12, 125e3, 0x12, {60, 300}, 6.0), -8);
        const LoraSatTelemetry t2 = run(c2, s, seconds(c2));
        CHECK(decoded(t2, c2) >= 1, "SF12 at -8 dB: %zu of %zu frames", decoded(t2, c2), c2.frames.size());
    }
    {   // what following the carrier through the frame buys: SF12 at 868 MHz at the closest approach, with and without
        LoraSatGenConfig c = config(868.1, framesAt(12, 125e3, 0x34, {296, 300, 304}, 5.0, 20), 12);
        LoraSatSettings on, off; off.track = false;
        const size_t a = decoded(run(c, on, seconds(c)), c), b = decoded(run(c, off, seconds(c)), c);
        printf("  SF12, 868.1 MHz, %.0f Hz/s: %zu of %zu with tracking, %zu without\n", c.pass.rateHzS(300), a, c.frames.size(), b);
        CHECK(a >= 2 && a > b, "tracking must decode more than a one-shot estimate: %zu vs %zu", a, b);   // the worst case of a pass: 2 of 3 measured
    }
    {   // the preset follows the dial: 436.8 MHz is the first preset (SF11, 125 kHz, sync 0x12)
        LoraSatGenConfig c = config(436.8, framesAt(11, 125e3, 0x12, {100, 300}, 3.0), 10);
        LoraSatSettings s;
        const LoraSatTelemetry t = run(c, s, seconds(c), 436.8e6 + loraSatTuning().tuneOffsetHz);
        CHECK(t.presetRunning == lorasat::nearest(436.8) && t.decoders.size() == 1 && t.decoders[0].sf == 11 && t.decoders[0].sync == 0x12, "preset %d with %zu demodulators", t.presetRunning, t.decoders.size());
        CHECK(decoded(t, c) == c.frames.size(), "frames with the preset: %zu of %zu", decoded(t, c), c.frames.size());
        CHECK(t.dialHz > 436.79e6 && t.dialHz < 436.81e6, "dial %.0f", t.dialHz);
        LoraSatSettings manual; manual.followDial = false;
        const LoraSatTelemetry t2 = run(c, manual, seconds(c), 436.8e6 + loraSatTuning().tuneOffsetHz);
        CHECK(t2.presetRunning < 0 && t2.decoders.size() == 12, "manual: %zu demodulators (SF7 - SF12 x two sync words)", t2.decoders.size());
        // far from every preset: manual
        LoraSatGenConfig c3 = config(145.0, framesAt(10, 125e3, 0x12, {300}, 3.0), 10);
        const LoraSatTelemetry t3 = run(c3, s, seconds(c3), 145.0e6 + loraSatTuning().tuneOffsetHz);
        CHECK(t3.presetRunning < 0, "no preset near 145 MHz");
    }
    {   // noise alone
        LoraSatGenConfig c = config(436.8, {}, 10);
        LoraSatSettings s;
        const LoraSatTelemetry t = run(c, s, 2.0);
        CHECK(t.blocksOk == 0 && t.log.empty(), "noise alone: %llu frames", (unsigned long long)t.blocksOk);
    }
    {   // results file
        LoraSatGenConfig c = config(436.8, framesAt(10, 125e3, 0x12, {200, 300}, 1.5), 15);
        LoraSatGenerator g(c);
        LoraSatReceiver r;
        r.configure(c.rate); r.setSignalOffset(c.dialOffsetHz);
        std::vector<cf32> buf(16384);
        const size_t total = (size_t)((seconds(c) + 0.4) * c.rate);
        for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        const std::string blob = r.saveResults();
        LoraSatReceiver r2;
        r2.configure(c.rate);
        CHECK(!blob.empty() && r2.loadResults(blob) && !r2.loadResults("junk"), "results file");
    }
    return testSummary("lorasat rx");
}
