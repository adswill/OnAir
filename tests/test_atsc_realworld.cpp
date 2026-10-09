// ATSC 1.0 (8-VSB) against real-world faults (REAL_WORLD_CHECKLIST.md): a clean generated signal, impaired with tests/impair.h (written
// independently of the receiver), through the real AtscReceiver. Every case must give nearly only good packets.
//   test_atsc_realworld          all cases
//   test_atsc_realworld N        only case N, with details
#include "dect2/atsc_gen.h"
#include "dect2/atsc_rx.h"
#include "dect2/dvbt_gen.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
static constexpr double kRate = 8e6;
static constexpr double kPilotHz = -(3.0e6 - 0.309440559e6);

static std::vector<cf32> clean(double seconds, double rate = kRate) {
    atsc::ChannelConfig cc;   // no impairment from the generator itself
    atsc::Generator gen(dvbt::testTsSource(), cc, rate, 1);
    std::vector<cf32> x;
    gen.generate((size_t)(seconds * rate), x);
    return x;
}

// scale the pilot tone (at its nominal place, after any shift of hz) by g: 0 removes it
static void scalePilot(std::vector<cf32>& x, double g, double shiftHz = 0, double rate = kRate) {
    const double w = 2 * M_PI * (kPilotHz + shiftHz) / rate;
    const size_t B = 1 << 16;   // estimate block by block (the pilot of the generator is steady)
    for (size_t b = 0; b < x.size(); b += B) {
        const size_t e = std::min(x.size(), b + B);
        std::complex<double> a = 0;
        for (size_t n = b; n < e; n++) a += std::complex<double>(x[n]) * std::polar(1.0, -w * (double)n);
        a /= (double)(e - b);
        for (size_t n = b; n < e; n++) x[n] -= cf32(std::complex<double>((1.0 - g)) * a * std::polar(1.0, w * (double)n));
    }
}

// an overdriven 8-bit radio: rms brought to `rms` (full scale 1), then clipped and quantised
static void clipTo(std::vector<cf32>& x, double rms) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    impair::clip8(x, rms / std::sqrt(p / (double)x.size()));
}

struct Res { size_t packets = 0, good = 0, flagged = 0, wrong = 0; };

static Res receive(const std::vector<cf32>& x, double rate = kRate, bool verbose = false) {
    AtscReceiver rx;
    rx.configure(rate);
    rx.setBlocking(true);
    std::vector<uint8_t> got;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { got.insert(got.end(), p, p + n * 188); });
    for (size_t i = 0; i < x.size(); i += 32768) rx.feed(x.data() + i, std::min<size_t>(32768, x.size() - i));
    rx.flush();
    std::set<std::string> ref;
    auto src = dvbt::testTsSource();
    for (int i = 0; i < 60000; i++) { uint8_t p[188]; src(p); p[0] = 0x47; ref.insert(std::string((char*)p, 188)); }
    Res r;
    r.packets = got.size() / 188;
    for (size_t k = 0; k < r.packets; k++) {
        if (got[k * 188 + 1] & 0x80) { r.flagged++; continue; }
        if (ref.count(std::string((char*)&got[k * 188], 188))) r.good++; else r.wrong++;
    }
    if (verbose) {
        AtscTelemetry t;
        rx.telemetry(t, 0);
        printf("    level %d pilot %d seg %d field %d cfo %.0f Hz sro %.1f ppm SNR %.1f fields %llu RS fail %llu\n", rx.detectLevel(), t.pilot, t.segSync, t.fieldSync,
               t.cfoHz, t.sroPpm, t.snrDb, (unsigned long long)t.fields, (unsigned long long)t.rsFailed);
    }
    return r;
}

struct Case { const char* name; double seconds; std::function<void(std::vector<cf32>&)> fault; size_t minGood; };

int main(int argc, char** argv) {
    const int only = argc > 1 ? atoi(argv[1]) : -1;
    // a second of signal carries about 10760 packets; the receiver needs a few fields to lock
    std::vector<Case> cs = {
        {"UHF tuning error +43 kHz", 1.2, [](auto& x) { impair::shift(x, 43000, kRate); }, 9000},
        {"UHF tuning error -43 kHz", 1.2, [](auto& x) { impair::shift(x, -43000, kRate); }, 9000},
        {"channel 1.0 MHz off centre (10 Msps)", 1.2, nullptr, 9000},   // handled below
        {"sample clock +100 ppm", 1.6, [](auto& x) { x = impair::clock(x, 100); }, 12000},
        {"sample clock -100 ppm", 1.6, [](auto& x) { x = impair::clock(x, -100); }, 12000},
        {"pilot 12 dB weak", 1.2, [](auto& x) { scalePilot(x, 0.25); }, 9000},
        {"pilot 18 dB weak (an echo notch)", 1.6, [](auto& x) { scalePilot(x, 0.125); }, 12000},
        {"ghost 3 dB stronger, 6 us later", 1.6, [](auto& x) { impair::echo(x, 48, 3.0, 0.7); impair::noise(x, 32); }, 9000},
        {"I and Q swapped, +20 kHz", 1.2, [](auto& x) { impair::shift(x, 20000, kRate); impair::swapIq(x); }, 9000},
        {"DC spike, 8-bit clipped", 1.2, [](auto& x) { impair::dc(x, -15); clipTo(x, 0.45); }, 9000},
        {"start mid-field, USB drop", 1.6, [](auto& x) { impair::skip(x, 123457); impair::drop(x, 5000000, 7777); }, 10000},
        {"NaN and infinite samples", 1.6, [](auto& x) { for (size_t i = 3000000; i < 3000100; i++) x[i] = cf32(NAN, INFINITY); }, 12000},
        {"combined: +43 kHz, +80 ppm, echo, 8-bit", 1.8, [](auto& x) {
             impair::echo(x, 16, -6.0, 2.0); x = impair::clock(x, 80); impair::shift(x, 43000, kRate); clipTo(x, 0.45); }, 11000},
    };
    for (size_t i = 0; i < cs.size(); i++) {
        if (only >= 0 && (int)i != only) continue;
        auto& c = cs[i];
        Res r;
        if (!c.fault) {   // off centre at 10 Msps
            auto x = clean(c.seconds, 10e6);
            impair::shift(x, 1.0e6, 10e6);
            r = receive(x, 10e6, only >= 0);
        } else {
            auto x = clean(c.seconds);
            c.fault(x);
            r = receive(x, kRate, only >= 0);
        }
        const bool ok = r.good >= c.minGood && r.wrong <= 3;
        printf("%-44s packets %6zu good %6zu flagged %5zu wrong %3zu  %s\n", c.name, r.packets, r.good, r.flagged, r.wrong, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    }
    printf(fails ? "ATSC real-world tests FAILED\n" : "ATSC real-world tests passed\n");
    return fails ? 1 : 0;
}
