// DRM Whole span: the finder on a wide capture of generated DRM signals, an AM station, an SSB-like band of noise, and noise; receivers per confirmed signal through the
// engine's bank. All signals are simulated (drm_gen): no wide recording of real DRM on hand.
#include "../data/drm/testkit.h"
#include "dect2/drm/drm_find.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdlib>
#include <random>
using namespace drmtest;
using namespace dect2;
#include "../common/test_util.h"

struct Emitter { Gen g; double hz; double amp; cf32 ph{1, 0}; };

static std::vector<cf32> span(std::vector<Emitter>& em, double rate, double secs, double noise, uint32_t seed) {
    const size_t n = (size_t)(rate * secs);
    std::vector<cf32> out(n, cf32(0, 0)), t(n);
    for (auto& e : em) {
        e.g(t.data(), n);
        const double w = 2 * M_PI * e.hz / rate;
        for (size_t i = 0; i < n; i++) { const double a = w * i; out[i] += t[i] * cf32((float)(e.amp * std::cos(a)), (float)(e.amp * std::sin(a))); }
    }
    std::mt19937 rng(seed); std::normal_distribution<float> gn(0, (float)noise);
    for (auto& v : out) v += cf32(gn(rng), gn(rng));
    return out;
}

// AM with a noise-like programme limited to +-4.5 kHz, at the same power per Hz as a broadcast signal
static Gen amGen(double rate, uint32_t seed) {
    auto rng = std::make_shared<std::mt19937>(seed); auto a1 = std::make_shared<float>(0); auto a2 = std::make_shared<float>(0);
    return [=](cf32* o, size_t n) {
        std::normal_distribution<float> g(0, 1);
        const float k = (float)(1.0 - std::exp(-2 * M_PI * 3000.0 / rate));
        for (size_t i = 0; i < n; i++) { *a1 += k * (g(*rng) - *a1); *a2 += k * (*a1 - *a2); o[i] = cf32(0.5f + 8.f * (*a2), 0); }
    };
}

static double rmsOf(const Sig& s, double rate) {
    Gen g = synthGen(s, rate); std::vector<cf32> v(200000); g(v.data(), v.size());
    double e = 0; for (auto& c : v) e += std::norm(c);
    return std::sqrt(e / v.size());
}

int main() {
    CHECK(std::fabs(drmReferenceFromBlock(100, 4500) - 0) < 200, "half channel starts at the reference");
    CHECK(std::fabs(drmReferenceFromBlock(-5000, 5000) - 0) < 1, "centred");
    CHECK(std::fabs(drmReferenceFromBlock(-4300, 13400) - 100) < 300, "18 kHz");
    const double rate = 500000;
    for (int snr : {30, 20, 14}) {
        Sig a; a.mode = 2; a.snrDb = 60;                  // B, 10 kHz
        Sig b; b.mode = 1; b.occ = 2; b.snrDb = 60;       // A, 5 kHz
        Sig c; c.mode = 3; c.occ = 4; c.snrDb = 60;       // C, 10 kHz
        Sig d; d.mode = 4; d.occ = 6; d.snrDb = 60;       // D, 20 kHz
        std::vector<Emitter> em;
        const double amp = 0.1;
        const double noise = amp * rmsOf(a, rate) / std::sqrt(2.0) * std::pow(10.0, -snr / 20.0) * std::sqrt(rate / 10000.0);  // SNR in 10 kHz of the same power
        em.push_back({synthGen(a, rate), -120000, amp});
        em.push_back({synthGen(b, rate), -40000, amp});
        em.push_back({synthGen(c, rate), 60000, amp});
        em.push_back({synthGen(d, rate), 150000, amp});
        em.push_back({amGen(rate, 3), 90000, amp});       // AM: must not start a receiver
        DrmFinder f; f.configure(rate, 7000000);
        auto x = span(em, rate, 8, noise, 5); printf("rms of the emitter %.3f, noise per component %.4f\n", rmsOf(a, rate), noise);
        for (size_t i = 0; i < x.size(); i += 32768) f.feed(x.data() + i, std::min<size_t>(32768, x.size() - i));
        auto h = f.hits();
        int got[4] = {}; int wrong = 0;
        for (auto& e : h) {
            bool ok = false;
            const double want[4] = {-120000, -40000, 60000, 150000};
            if (std::getenv("DRMV")) printf("hit %.0f [%.0f %.0f]\n", e.hz, e.loHz, e.hiHz);
            for (int k = 0; k < 4; k++) if (e.hiHz > want[k] - 1000 && e.loHz < want[k] + 1000) { got[k]++; ok = true; }   // drm_gen centres the occupied carriers on the emitter
            if (!ok) { wrong++; printf("extra hit %.0f width %.0f mode %d corr %.2f\n", e.hz, e.widthHz, e.mode, e.corr); }
        }
        printf("SNR %d: B10 %d A5 %d C10 %d D20 %d extra %d\n", snr, got[0], got[1], got[2], got[3], wrong);
        CHECK(wrong == 0, "no false hits at %d dB", snr);
        if (snr >= 20) CHECK(got[0] == 1 && got[1] == 1 && got[2] == 1 && got[3] == 1, "all four at %d dB", snr);
        else CHECK(got[0] + got[1] + got[2] + got[3] >= 3, "at least three at %d dB", snr);
    }
    {   // nothing but noise and AM
        std::vector<Emitter> em; em.push_back({amGen(rate, 9), 20000, 0.2});
        DrmFinder f; f.configure(rate, 0);
        auto x = span(em, rate, 6, 0.01, 8);
        f.feed(x.data(), x.size());
        CHECK(f.hits().empty(), "AM + noise: %zu hits", f.hits().size());
    }
    {   // the bank: two DRM signals decoded at once
        Sig a; a.mode = 2; a.snrDb = 60; Sig b; b.mode = 1; b.snrDb = 60;
        std::vector<Emitter> em; em.push_back({synthGen(a, rate), -60000, 0.1}); em.push_back({synthGen(b, rate), 70000, 0.1});
        WideModes w; w.setCenterHz(7000000); w.configure(11, rate, 0);
        WideTuning tun; tun.cpuBudget = 0; w.setTuning(tun);
        auto x = span(em, rate, 18, 0.0015, 6);
        uint64_t seq = 0; RxTelemetry t;
        for (size_t i = 0; i < x.size(); i += 32768) w.feed(11, x.data() + i, std::min<size_t>(32768, x.size() - i));
        CHECK(w.liveCount(11) == 2, "two receivers, got %zu", w.liveCount(11));
        CHECK(w.telemetry(11, t, seq), "telemetry");
        int locked = 0; for (const auto& r : t.wide) { printf("carrier %+.0f state %d %s\n", r.hz - 7000000, r.state, r.info.c_str()); if (r.state == 2) locked++; }
        CHECK(locked == 2, "both locked: %d", locked);
    }
    return testSummary("drm_find");
}
