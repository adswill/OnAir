// Stage 1 of the Inmarsat all-carriers mode: the carrier detector on synthetic wideband captures with offsets, Doppler, fading, adjacent carriers,
// a DC spike, noise only, different chunk sizes, and the Aero test signal. Synthetic only: needs a user test with a real capture.
#include "dect2/inmsat/inmsat_detect.h"
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_rx.h"
#include "inmsat_synth.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::inmsat;
using namespace inmsat_test;
#include "../common/test_util.h"

static std::vector<DetectedCarrier> run(double rate, std::vector<CarrierSpec> cars, double secs, size_t chunk, double dc = 0, uint64_t seed = 7) {
    WideSynth syn(rate, cars, dc, seed);
    CarrierDetector det;
    DetectorConfig cfg;
    cfg.rate = rate;
    det.configure(cfg);
    std::vector<cf32> buf(chunk);
    for (double done = 0; done < secs * rate; done += (double)chunk) { syn.generate(buf.data(), chunk); det.feed(buf.data(), chunk); }
    std::vector<DetectedCarrier> r;
    for (const auto& c : det.carriers()) if (c.confirmed) r.push_back(c);
    std::sort(r.begin(), r.end(), [](const DetectedCarrier& a, const DetectedCarrier& b) { return a.offsetHz < b.offsetHz; });
    return r;
}

int main() {
    // 1. six carriers over 10 Msps: narrow with Doppler, medium, wide, an adjacent pair, one fading
    {
        const double rate = 10e6;
        std::vector<CarrierSpec> cars = {
            {-3.1e6, 250, 22, 0, 0},                 // narrow
            {-1.234e6, 300, 20, 25.0, 0},            // narrow with Doppler of 25 Hz/s
            {0.5e6, 700, 22, 0, 3.0},                // medium, fading 3 dB
            {2.0e6, 3000, 20, 0, 0},                 // wide
            {2.0e6 + 40e3, 2500, 20, 0, 0},          // wide, 40 kHz away
            {-0.35e6, 280, 12, 0, 6.0},              // weak and fading 6 dB
        };
        const auto r = run(rate, cars, 10, 65536);
        printf("test 1: %zu carriers\n", r.size());
        for (const auto& c : r) printf("  %+10.1f Hz  width %6.0f  snr %5.1f dB  %s  seen %llu\n", c.offsetHz, c.widthHz, c.snrDb, carrierClassName(c.cls), (unsigned long long)c.seen);
        CHECK(r.size() == cars.size(), "found %zu of %zu", r.size(), cars.size());
        for (const auto& s : cars) {
            const DetectedCarrier* best = nullptr;
            const double want = s.offsetHz + s.dopplerHzPerSec * 5;      // mid-capture
            for (const auto& c : r) if (!best || std::fabs(c.offsetHz - want) < std::fabs(best->offsetHz - want)) best = &c;
            CHECK(best && std::fabs(best->offsetHz - want) < 700, "carrier at %+.0f Hz not found (nearest %+.0f)", s.offsetHz, best ? best->offsetHz : 0.0);
        }
        // classes (the shaped noise is wider than its 3 dB width at 20 dB SNR): the 250-300 Hz carriers are not wide, the 2.5-3 kHz ones are not narrow
        for (const auto& c : r) if (std::fabs(c.offsetHz - 2.0e6) < 80e3) CHECK(c.cls != CarrierClass::Narrow, "wide carrier called narrow");
        for (const auto& c : r) if (c.offsetHz < -1e6) CHECK(c.cls != CarrierClass::Wide, "narrow carrier called %s", carrierClassName(c.cls));
    }
    // 2. noise only, 16 s: nothing confirmed. DC spike alone: nothing.
    {
        const auto r = run(10e6, {}, 16, 65536, 0.0);
        CHECK(r.empty(), "noise only: %zu carriers", r.size());
        const auto r2 = run(10e6, {}, 8, 65536, 0.5);
        CHECK(r2.empty(), "DC spike: %zu carriers", r2.size());
    }
    // 3. the result does not depend on how the stream is cut
    {
        std::vector<CarrierSpec> cars = {{-1.0e6, 300, 20, 0, 0}, {1.5e6, 2500, 20, 0, 0}};
        const auto a = run(10e6, cars, 8, 4096);
        const auto b = run(10e6, cars, 8, 262144);
        CHECK(a.size() == 2 && b.size() == 2, "chunks %zu %zu", a.size(), b.size());
        if (a.size() == 2 && b.size() == 2) for (int i = 0; i < 2; i++) CHECK(std::fabs(a[(size_t)i].offsetHz - b[(size_t)i].offsetHz) < 300, "chunk dependence %.0f %.0f", a[(size_t)i].offsetHz, b[(size_t)i].offsetHz);
    }
    // 4. an RTL-SDR sized capture (2.4 Msps) and a HackRF Pro sized one (20 Msps)
    {
        const auto r = run(2.4e6, {{-0.6e6, 300, 20, 0, 0}, {0.4e6, 3000, 18, 0, 0}}, 8, 65536);
        CHECK(r.size() == 2, "2.4 Msps: %zu carriers", r.size());
        const auto r2 = run(20e6, {{-7.0e6, 300, 22, 0, 0}, {3.3e6, 3000, 20, 0, 0}, {8.0e6, 700, 20, 0, 0}}, 6, 131072);
        CHECK(r2.size() == 3, "20 Msps: %zu carriers", r2.size());
        for (const auto& c : r2) printf("  20 Msps: %+10.1f Hz width %5.0f snr %.1f\n", c.offsetHz, c.widthHz, c.snrDb);
    }
    // 5. the real Aero test signal (P channels at three rates), 4 Msps: the detector finds the three carrier groups where the generator put them
    {
        const double rate = 4e6;
        SynthConfig sc;
        sc.mode = 20;
        sc.modeOpt[0] = 7;
        AeroSynth syn(sc, rate);
        CarrierDetector det;
        DetectorConfig cfg;
        cfg.rate = rate;
        det.configure(cfg);
        std::vector<cf32> buf(65536);
        for (double done = 0; done < 8 * rate; done += (double)buf.size()) { syn.generate(buf.data(), buf.size()); det.feed(buf.data(), buf.size()); }
        int found = 0;
        for (const auto& ch : syn.channels()) {
            const double want = ch.offsetHz - aeroTuning().tuneOffsetHz;
            bool ok = false;
            for (const auto& c : det.carriers()) ok |= c.confirmed && std::fabs(c.offsetHz - want) < 2500;
            printf("  Aero %5d bit/s expected at %+.0f Hz: %s\n", ch.bitRate, want, ok ? "found" : "MISSING");
            found += ok;
        }
        for (const auto& c : det.carriers()) printf("  detected %+10.1f Hz width %6.0f snr %.1f %s%s\n", c.offsetHz, c.widthHz, c.snrDb, carrierClassName(c.cls), c.confirmed ? "" : " (pending)");
        for (const auto& c : det.carriers()) {   // 600 bit/s narrow, 1200 bit/s medium (the width of Inmarsat-C too), 10500 bit/s wide
            if (std::fabs(c.offsetHz + 100000) < 3000) CHECK(c.cls == CarrierClass::Wide, "10500 bit/s class %s", carrierClassName(c.cls));
            if (std::fabs(c.offsetHz - 50000) < 3000) CHECK(c.cls == CarrierClass::Narrow, "600 bit/s class %s", carrierClassName(c.cls));
            if (std::fabs(c.offsetHz - 100000) < 3000) CHECK(c.cls == CarrierClass::Medium, "1200 bit/s class %s", carrierClassName(c.cls));
        }
        CHECK(found == (int)syn.channels().size(), "Aero carriers found %d of %zu", found, syn.channels().size());
    }
    // 6. a weak 10500 bit/s P channel (its power is spread over about 10 kHz, so per bin it is far below a 600 bit/s carrier): found
    //    as a wide carrier at low Eb/N0, with a frequency offset and a neighbouring strong narrow carrier, and no ghost on noise alone
    {
        const double rate = 4e6;
        for (double ebn0 : {12.0, 7.0, 4.0}) {
            for (double cfo : {0.0, 4300.0}) {
                SynthConfig sc;
                sc.mode = 20;
                sc.modeOpt[0] = 4;
                sc.modeOpt[1] = 5;
                sc.modeVal[0] = ebn0;
                sc.cfoHz = cfo;
                AeroSynth syn(sc, rate);
                CarrierDetector det;
                DetectorConfig cfg;
                cfg.rate = rate;
                det.configure(cfg);
                std::vector<cf32> buf(65536);
                for (double done = 0; done < 12 * rate; done += (double)buf.size()) { syn.generate(buf.data(), buf.size()); det.feed(buf.data(), buf.size()); }
                const double want = syn.channels()[0].offsetHz - aeroTuning().tuneOffsetHz + cfo;
                int hit = 0, wide = 0;
                for (const auto& c : det.carriers()) if (c.confirmed && std::fabs(c.offsetHz - want) < 3500) { hit++; wide += c.cls == CarrierClass::Wide; }
                printf("  10500 bit/s Eb/N0 %.0f dB cfo %+.0f: %d carrier(s) near %+.0f, wide %d\n", ebn0, cfo, hit, want, wide);
                CHECK(hit == 1, "10500 bit/s Eb/N0 %.0f cfo %.0f: %d carriers", ebn0, cfo, hit);
                CHECK(wide == 1, "10500 bit/s Eb/N0 %.0f cfo %.0f: not called wide", ebn0, cfo);
            }
        }
        CarrierDetector det;
        DetectorConfig cfg;
        cfg.rate = rate;
        det.configure(cfg);
        WideSynth nz(rate, {}, 0, 3);
        std::vector<cf32> buf(65536);
        for (double done = 0; done < 30 * rate; done += (double)buf.size()) { nz.generate(buf.data(), buf.size()); det.feed(buf.data(), buf.size()); }
        int n = 0;
        for (const auto& c : det.carriers()) n += c.confirmed;
        CHECK(n == 0, "noise only: %d ghost carriers", n);
    }
    // 7. the rate suggested for a carrier no decoder holds yet
    CHECK(dect2::inmsat::rateGuessForWidth(10000, CarrierClass::Wide) == 10500, "wide guess");
    CHECK(dect2::inmsat::rateGuessForWidth(30000, CarrierClass::Wide) == 0, "very wide: a C channel, no guess");
    CHECK(dect2::inmsat::rateGuessForWidth(900, CarrierClass::Narrow) == 600, "narrow guess");
    CHECK(dect2::inmsat::rateGuessForWidth(2500, CarrierClass::Medium) == 0, "medium: no guess");
    return testSummary("inmsat_detect");
}
