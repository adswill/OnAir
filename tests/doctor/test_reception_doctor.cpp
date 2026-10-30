// Reception Doctor on synthetic IQ: clipped, weak, noise only, off-centre, too narrow, adjacent interferer, drops, DC spike, I/Q imbalance, healthy.
#include "dect2/doctor/reception_doctor.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::doctor;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kRate = 2e6;
static const size_t kN = 1 << 18;
struct Tone { double hz, amp; double bw = 0; };

// white noise (per-component sigma) plus bands: a band is 40 tones spread over bw, total amplitude amp
static std::vector<cf32> make(double sigma, const std::vector<Tone>& tones, uint32_t seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)sigma);
    std::vector<cf32> x(kN);
    for (size_t i = 0; i < kN; i++) x[i] = cf32(g(rng), g(rng));
    for (auto& t : tones) {
        const int k = t.bw > 0 ? 40 : 1;
        for (int j = 0; j < k; j++) {
            const double f = t.bw > 0 ? t.hz - t.bw / 2 + t.bw * (j + 0.5) / k : t.hz;
            const double ph0 = M_PI * j * j / k,   // Schroeder phases: a flat envelope, not a pulse train
                          a = t.amp / std::sqrt((double)k);
            for (size_t i = 0; i < kN; i++) { const double ph = 2 * M_PI * f * i / kRate + ph0; x[i] += cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph))); }
        }
    }
    return x;
}
static bool has(const Report& r, Code c) { for (auto& f : r.findings) if (f.code == c) return true; return false; }
static const Finding* get(const Report& r, Code c) { for (auto& f : r.findings) if (f.code == c) return &f; return nullptr; }
static Context live() { Context c; c.rateHz = kRate; c.live = true; c.gainDb = 20; c.gainMaxDb = 40; c.identify = false; return c; }

int main() {
    {   // healthy: a 100 kHz-wide signal in the channel, sane level
        auto x = make(0.01, {{0, 0.1, 100e3}});
        Context c = live(); c.modeBwHz = 100e3; c.modeOffsetHz = 0;
        Report r = diagnose(x.data(), x.size(), c);
        CHECK(r.valid && r.findings.size() == 1 && r.findings[0].code == Code::LooksFine, "healthy: %zu findings, first %d", r.findings.size(), r.findings.empty() ? -1 : (int)r.findings[0].code);
    }
    {   // clipped
        auto x = make(0.5, {{200e3, 1.0}});
        for (auto& v : x) { v = cf32(std::max(-1.f, std::min(1.f, v.real())), std::max(-1.f, std::min(1.f, v.imag()))); }
        Context c = live();
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::Clipping);
        CHECK(f && f->fix == Fix::LowerGain && f->value >= 12 && r.findings[0].code == Code::Clipping, "clipping not found/first");
    }
    {   // weak: -70 dBFS, gain room left
        auto x = make(0.0002, {});
        Context c = live();
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::WeakSignal);
        CHECK(f && f->fix == Fix::RaiseGain && f->value > 0, "weak not found");
        CHECK(has(r, Code::NoSignal), "weak noise-only: no 'nothing stands out'");
        c.gainDb = 40;   // at max gain: sentence only
        r = diagnose(x.data(), x.size(), c);
        f = get(r, Code::WeakSignal);
        CHECK(f && f->fix == Fix::None, "weak at max gain should offer no fix");
    }
    {   // noise only at a sane level: no signal
        auto x = make(0.05, {});
        Report r = diagnose(x.data(), x.size(), live());
        CHECK(has(r, Code::NoSignal) && !has(r, Code::Clipping) && !has(r, Code::WeakSignal), "noise only misjudged");
    }
    {   // off-centre by +300 kHz: mode expects it at 0
        auto x = make(0.01, {{300e3, 0.1, 100e3}});
        Context c = live(); c.modeBwHz = 100e3;
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::OffCentre);
        CHECK(f && f->fix == Fix::ApplyOffset && std::fabs(f->value - 300e3) < 10e3, "offset found %s value %.0f", f ? "yes" : "no", f ? f->value : 0.0);
        // on place: nothing
        auto y = make(0.01, {{0, 0.1, 100e3}});
        CHECK(!has(diagnose(y.data(), y.size(), c), Code::OffCentre), "false off-centre");
    }
    {   // rate too narrow
        auto x = make(0.01, {{0, 0.1, 100e3}});
        Context c = live(); c.modeBwHz = 1.9e6;
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::RateTooNarrow);
        CHECK(f && f->fix == Fix::WidenRate && f->value > kRate, "narrow rate");
    }
    {   // strong adjacent interferer 500 kHz away, 30 dB over the wanted one
        auto x = make(0.003, {{0, 0.01, 50e3}, {500e3, 0.3, 50e3}});
        Context c = live(); c.modeBwHz = 50e3;
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::Interferer);
        CHECK(f && f->fix == Fix::LowerGain, "interferer");
        auto y = make(0.003, {{0, 0.1, 50e3}, {500e3, 0.1, 50e3}});
        CHECK(!has(diagnose(y.data(), y.size(), c), Code::Interferer), "false interferer");
    }
    {   // drops
        auto x = make(0.01, {{0, 0.1, 100e3}});
        Context c = live(); c.modeBwHz = 100e3; c.deliveredPct = 80; c.cpuDrops = true;
        Report r = diagnose(x.data(), x.size(), c);
        CHECK(has(r, Code::UsbDrops) && has(r, Code::CpuDrops), "drops");
        c.live = false; c.gainMaxDb = 0;
        CHECK(!has(diagnose(x.data(), x.size(), c), Code::UsbDrops), "file has no usb drops");
    }
    {   // DC spike in the channel
        auto x = make(0.01, {{150e3, 0.1, 50e3}, {0, 0.3}});
        Context c = live(); c.modeBwHz = 400e3; c.modeOffsetHz = 0;
        Report r = diagnose(x.data(), x.size(), c);
        const Finding* f = get(r, Code::DcSpike);
        CHECK(f && f->severity == SevWarn, "dc spike");
    }
    {   // I/Q imbalance: a tone with a mirror 20 dB down
        auto x = make(0.003, {{300e3, 0.2}, {-300e3, 0.02}});
        Report r = diagnose(x.data(), x.size(), live());
        const Finding* f = get(r, Code::IqImbalance);
        CHECK(f && f->a > 15 && f->a < 25, "image rejection %.1f", f ? f->a : -1.0);
        auto y = make(0.003, {{300e3, 0.2}});
        CHECK(!has(diagnose(y.data(), y.size(), live()), Code::IqImbalance), "false imbalance");
    }
    {   // locked decoder: no complaints about offsets or noise
        auto x = make(0.01, {{300e3, 0.1, 100e3}});
        Context c = live(); c.modeBwHz = 100e3; c.locked = true;
        Report r = diagnose(x.data(), x.size(), c);
        CHECK(r.findings.size() == 1 && r.findings[0].code == Code::LooksFine, "locked should be quiet: %zu first %d", r.findings.size(), (int)r.findings[0].code);
    }
    {   // too short: invalid
        std::vector<cf32> x(100);
        CHECK(!diagnose(x.data(), x.size(), live()).valid, "short capture");
    }
    printf(fails ? "%d FAILED\n" : "reception_doctor: all passed\n", fails);
    return fails ? 1 : 0;
}
