// The feature extractor of the signal identifier (dect2/engine/sig_ident.h) on signals made here, whose bandwidth, symbol rate, modulation,
// deviation, burst timing and symbol length are known: the measurements must come out within the tolerances below, at any frequency offset,
// with noise, and fast enough (under 200 ms). Also the region helpers (regionAt, findRegions), the calibrated reject option and the text forms.
#include "dect2/engine/sig_ident.h"
#include "../common/cpu_report.h"
#include "../common/ci_slack.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"

namespace {

std::mt19937 rng(12345);
double gauss() { static dect2::normal_distribution<double> d(0, 1); return d(rng); }

// root-raised-cosine pulse, `sps` samples per symbol, span symbols each side
std::vector<float> rrc(double beta, int sps, int span) {
    std::vector<float> h((size_t)(2 * span * sps + 1));
    for (int i = 0; i < (int)h.size(); i++) {
        const double t = (double)(i - span * sps) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - beta + 4 * beta / M_PI;
        else if (std::fabs(std::fabs(4 * beta * t) - 1) < 1e-9) v = beta / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * beta)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * beta)));
        else v = (std::sin(M_PI * t * (1 - beta)) + 4 * beta * t * std::cos(M_PI * t * (1 + beta))) / (M_PI * t * (1 - std::pow(4 * beta * t, 2)));
        h[(size_t)i] = (float)v;
    }
    return h;
}

// a linearly modulated signal: nsym symbols of `order` phases (2 BPSK, 4 QPSK), RRC shaped, at rs symbols/s, sample rate fs
std::vector<cf32> psk(int order, double rs, double fs, double secs, double beta = 0.35) {
    const int sps = (int)std::lround(fs / rs);
    const size_t nsym = (size_t)(secs * rs);
    std::vector<cf32> up(nsym * (size_t)sps, cf32(0, 0));
    for (size_t k = 0; k < nsym; k++) {
        if (order == 2) up[k * (size_t)sps] = cf32(rng() & 1 ? 1.f : -1.f, 0);
        else { const double ph = (double)(rng() % 4) * M_PI / 2 + M_PI / 4; up[k * (size_t)sps] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    }
    const std::vector<float> h = rrc(beta, sps, 6);
    std::vector<cf32> y(up.size());
    for (size_t i = 0; i < up.size(); i++) {
        cf32 a(0, 0);
        for (size_t j = 0; j < h.size(); j++) { const long idx = (long)i - (long)j + (long)h.size() / 2; if (idx >= 0 && idx < (long)up.size()) a += up[(size_t)idx] * h[j]; }
        y[i] = a;
    }
    return y;
}

// FSK: levels (Hz) chosen at random per symbol, raised-cosine-ish smoothing by a moving average over 70 % of the symbol, FM modulated
std::vector<cf32> fsk(const std::vector<double>& levels, double rs, double fs, double secs) {
    const int sps = (int)std::lround(fs / rs);
    const size_t nsym = (size_t)(secs * rs);
    std::vector<double> f(nsym * (size_t)sps);
    for (size_t k = 0; k < nsym; k++) { const double v = levels[rng() % levels.size()]; for (int j = 0; j < sps; j++) f[k * (size_t)sps + (size_t)j] = v; }
    const int w = std::max(1, (int)(0.7 * sps));
    std::vector<double> g(f.size(), 0.0);
    double acc = 0;
    for (size_t i = 0; i < f.size(); i++) { acc += f[i]; if (i >= (size_t)w) acc -= f[i - (size_t)w]; g[i] = acc / w; }
    std::vector<cf32> y(f.size());
    double ph = 0;
    for (size_t i = 0; i < f.size(); i++) { ph += 2 * M_PI * g[i] / fs; y[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    return y;
}

void shiftNoise(std::vector<cf32>& x, double fs, double offHz, double snrDb, double bwHz) {
    double p = 0;
    for (const cf32& v : x) p += std::norm(v);
    p /= x.size();
    const double sigma = std::sqrt(p / std::pow(10.0, snrDb / 10) * (fs / bwHz) / 2);
    double ph = 0; const double d = 2 * M_PI * offHz / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + d, 2 * M_PI); v += cf32((float)(sigma * gauss()), (float)(sigma * gauss())); }
}

Result id(const std::vector<cf32>& x, double fs, double lo = 0, double hi = 0, double rf = 0) {
    Options o; o.rfHz = rf; o.loHz = lo; o.hiHz = hi; o.usePriors = false;
    return identify(x.data(), x.size(), fs, o);
}

}  // namespace

int main() {
    // ---- linear modulations: class, symbol rate, occupied width and centre, at several offsets
    for (int order : {2, 4}) {
        for (double off : {0.0, -61e3, 118e3}) {
            const double fs = 500e3, rs = 20e3, beta = 0.35;
            std::vector<cf32> x = psk(order, rs, fs, 0.8, beta);
            shiftNoise(x, fs, off, 25, rs * (1 + beta));
            const Result r = id(x, fs);
            const char* nm = order == 2 ? "BPSK" : "QPSK";
            CHECK(r.f.valid, "%s %+.0f: not valid (%s)", nm, off, r.f.why.c_str());
            CHECK(r.f.mod == (order == 2 ? ModBpsk : ModQpsk), "%s %+.0f: class %s", nm, off, modName(r.f.mod));
            CHECK(std::fabs(r.f.symRate / rs - 1) < 0.02, "%s %+.0f: symbol rate %.1f, expected %.0f", nm, off, r.f.symRate, rs);
            CHECK(std::fabs(r.f.obwHz / (rs * (1 + beta)) - 1) < 0.15, "%s %+.0f: width %.0f, expected %.0f", nm, off, r.f.obwHz, rs * (1 + beta));
            CHECK(std::fabs(r.f.centreHz - off) < 0.02 * rs * (1 + beta) + 150, "%s %+.0f: centre %.0f", nm, off, r.f.centreHz);
            CHECK(r.f.duty > 0.97, "%s %+.0f: duty %.2f", nm, off, r.f.duty);
        }
    }
    // ---- 2FSK and 4FSK: levels and rate
    {
        const double fs = 250e3, rs = 4800;
        std::vector<cf32> x = fsk({-2400, 2400}, rs, fs, 1.0);
        shiftNoise(x, fs, 41e3, 25, 12e3);
        const Result r = id(x, fs);
        CHECK(r.f.mod == ModFsk2 || r.f.mod == ModGmsk, "2FSK: class %s", modName(r.f.mod));
        CHECK(std::fabs(r.f.symRate / rs - 1) < 0.02, "2FSK: rate %.1f", r.f.symRate);
        CHECK(r.f.fskLevels == 2 && std::fabs(r.f.devHz[1] / 2400 - 1) < 0.15, "2FSK: levels %d, %+.0f / %+.0f", r.f.fskLevels, r.f.devHz[0], r.f.devHz[1]);
        CHECK(std::fabs(r.f.centreHz - 41e3) < 400, "2FSK: centre %.0f", r.f.centreHz);
    }
    {
        const double fs = 250e3, rs = 4800;
        std::vector<cf32> x = fsk({-1944, -648, 648, 1944}, rs, fs, 1.0);
        shiftNoise(x, fs, -30e3, 28, 9e3);
        const Result r = id(x, fs);
        CHECK(r.f.mod == ModFsk4, "4FSK: class %s", modName(r.f.mod));
        CHECK(std::fabs(r.f.symRate / rs - 1) < 0.02, "4FSK: rate %.1f", r.f.symRate);
        CHECK(r.f.fskLevels == 4 && std::fabs(r.f.devHz[3] / 1944 - 1) < 0.15 && std::fabs(r.f.devHz[2] / 648 - 1) < 0.25, "4FSK: levels %+.0f %+.0f %+.0f %+.0f", r.f.devHz[0], r.f.devHz[1], r.f.devHz[2], r.f.devHz[3]);
    }
    // ---- analog: FM with a tone, AM with audio, a keyed carrier
    {
        const double fs = 250e3, dev = 2500;
        std::vector<cf32> x((size_t)(fs * 0.6));
        double ph = 0, l1 = 0, l2 = 0, l3 = 0;   // speech-like audio: white noise through three one-pole low-passes (about 2.5 kHz)
        const double a = 1 - std::exp(-2 * M_PI * 2500 / fs);
        for (size_t i = 0; i < x.size(); i++) {
            l1 += a * (gauss() - l1); l2 += a * (l1 - l2); l3 += a * (l2 - l3);
            ph += 2 * M_PI * dev * std::max(-1.5, std::min(1.5, l3 * 14.0)) / fs;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        shiftNoise(x, fs, 20e3, 30, 16e3);
        const Result r = id(x, fs);
        CHECK(r.f.mod == ModFm, "FM: class %s (%s)", modName(r.f.mod), r.f.why.c_str());
        CHECK(r.f.envKurt < 1.1, "FM: envelope %.2f is not constant", r.f.envKurt);
    }
    {
        const double fs = 250e3;
        std::vector<cf32> x((size_t)(fs * 0.6));
        for (size_t i = 0; i < x.size(); i++) { const double a = 0.3 * (1 + 0.5 * std::sin(2 * M_PI * 700 * i / fs) + 0.3 * std::sin(2 * M_PI * 1900 * i / fs + 1)); x[i] = cf32((float)a, 0); }
        shiftNoise(x, fs, -45e3, 35, 10e3);
        const Result r = id(x, fs);
        CHECK(r.f.mod == ModAm, "AM: class %s", modName(r.f.mod));
        CHECK(r.f.carrierDb > 12, "AM: carrier %.1f dB", r.f.carrierDb);
    }
    // ---- bursts: 1 ms of 4800 Bd FSK every 5 ms (a TDMA slot): duty, burst length and period
    {
        const double fs = 250e3;
        std::vector<cf32> x = fsk({-2400, 2400}, 4800, fs, 1.0);
        for (size_t i = 0; i < x.size(); i++) if (std::fmod(i / fs, 0.005) >= 0.001) x[i] = cf32(0, 0);
        shiftNoise(x, fs, 10e3, 30, 12e3);
        const Result r = id(x, fs);
        CHECK(r.f.duty > 0.1 && r.f.duty < 0.3, "bursts: duty %.2f, expected 0.2", r.f.duty);
        CHECK(std::fabs(r.f.burstMs / 1.0 - 1) < 0.5, "bursts: length %.2f ms, expected about 1", r.f.burstMs);
        CHECK(r.f.burstPeriodMs > 0 && std::fabs(r.f.burstPeriodMs / 5.0 - 1) < 0.1, "bursts: period %.2f ms, expected 5", r.f.burstPeriodMs);
    }
    // ---- OFDM: the useful symbol length from the cyclic prefix (256 carriers, 1/8 guard, 250 kHz spacing/256 -> Tu = 1.024 ms at 250 kS/s)
    {
        const double fs = 250e3;
        const int N = 256, G = 32, used = 200;
        std::vector<cf32> x;
        std::vector<cf32> sym((size_t)N), td((size_t)N);
        for (int s = 0; s < 150; s++) {
            std::fill(sym.begin(), sym.end(), cf32(0, 0));
            for (int k = 1; k <= used / 2; k++) {
                const double a = (double)(rng() % 4) * M_PI / 2 + M_PI / 4, b = (double)(rng() % 4) * M_PI / 2 + M_PI / 4;
                sym[(size_t)k] = cf32((float)std::cos(a), (float)std::sin(a)); sym[(size_t)(N - k)] = cf32((float)std::cos(b), (float)std::sin(b));
            }
            for (int n = 0; n < N; n++) { cf32 a(0, 0); for (int k = 0; k < N; k++) if (sym[(size_t)k] != cf32(0, 0)) { const double ph = 2 * M_PI * k * n / N; a += sym[(size_t)k] * cf32((float)std::cos(ph), (float)std::sin(ph)); } td[(size_t)n] = a / 12.f; }
            for (int n = N - G; n < N; n++) x.push_back(td[(size_t)n]);
            for (int n = 0; n < N; n++) x.push_back(td[(size_t)n]);
        }
        shiftNoise(x, fs, 5e3, 25, fs * used / N);
        const Result r = id(x, fs);
        CHECK(r.f.mod == ModOfdm, "OFDM: class %s", modName(r.f.mod));
        CHECK(std::fabs(r.f.ofdmTuSec / (N / fs) - 1) < 0.03, "OFDM: useful symbol %.1f us, expected %.1f", r.f.ofdmTuSec * 1e6, N / fs * 1e6);
        CHECK(std::fabs(r.f.obwHz / (fs * used / N) - 1) < 0.15, "OFDM: width %.0f, expected %.0f", r.f.obwHz, fs * used / N);
    }
    // ---- noise only, and two unrelated tones: nothing to call
    {
        std::vector<cf32> x(400000);
        for (auto& v : x) v = cf32((float)(0.01 * gauss()), (float)(0.01 * gauss()));
        const Result r = id(x, 1e6);
        CHECK(!r.f.valid && r.f.mod == ModNoise && r.verdict == VerdictUnknown, "noise: valid %d, class %s", (int)r.f.valid, modName(r.f.mod));
    }
    {
        const double fs = 500e3;
        std::vector<cf32> x((size_t)(fs * 0.8));
        double p1 = 0, p2 = 0;
        for (size_t i = 0; i < x.size(); i++) {
            p1 += 2 * M_PI * (60e3 + 40 * gauss()) / fs; p2 += 2 * M_PI * (113e3 + 40 * gauss()) / fs;
            x[i] = cf32((float)(0.1 * std::cos(p1) + 0.1 * std::cos(p2)) + (float)(0.003 * gauss()), (float)(0.1 * std::sin(p1) + 0.1 * std::sin(p2)) + (float)(0.003 * gauss()));
        }
        const Result r = id(x, fs, 40e3, 80e3);
        CHECK(r.verdict != VerdictLikely, "two tones: verdict %d, top %s (%.0f %%)", r.verdict, r.cands.empty() ? "-" : r.cands[0].name.c_str(), r.cands.empty() ? 0.0 : r.cands[0].confidence * 100);
    }
    // ---- regions: two signals, a click on either, and "all"
    {
        const double fs = 1e6;
        std::vector<cf32> a = psk(4, 20e3, fs, 0.6), b = psk(2, 50e3, fs, 0.6);
        std::vector<cf32> x(a.size());
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / fs;
            x[i] = a[i] * cf32((float)std::cos(2 * M_PI * -200e3 * t), (float)std::sin(2 * M_PI * -200e3 * t)) + 0.5f * b[i] * cf32((float)std::cos(2 * M_PI * 150e3 * t), (float)std::sin(2 * M_PI * 150e3 * t));
        }
        shiftNoise(x, fs, 0, 30, 60e3);
        const Region ra = regionAt(x.data(), x.size(), fs, -200e3), rb = regionAt(x.data(), x.size(), fs, 150e3);
        CHECK(ra.loHz < -200e3 && ra.hiHz > -200e3 && ra.bwHz < 40e3, "region A [%.0f %.0f] bw %.0f", ra.loHz, ra.hiHz, ra.bwHz);
        CHECK(rb.loHz < 150e3 && rb.hiHz > 150e3 && rb.bwHz < 90e3, "region B [%.0f %.0f] bw %.0f", rb.loHz, rb.hiHz, rb.bwHz);
        const std::vector<Region> all = findRegions(x.data(), x.size(), fs);
        CHECK(all.size() == 2, "findRegions found %zu signals, expected 2", all.size());
        if (all.size() == 2) CHECK(all[0].powerDb >= all[1].powerDb && std::fabs(all[0].centreHz + 200e3) < 5e3, "strongest first: %.0f Hz", all[0].centreHz);
        const Region nothing = regionAt(x.data(), x.size(), fs, 400e3, 25e3);
        CHECK(std::fabs(nothing.bwHz - 25e3) < 1, "no signal under the click: a 25 kHz channel, got %.0f", nothing.bwHz);
    }
    // ---- speed: 1.5 M samples of an 8 MHz wide OFDM-like noise signal and of a narrow FSK in a 2 Msps capture, under 200 ms each
    {
        const double fs = 10e6;
        std::vector<cf32> x((size_t)1.5e6);
        std::vector<cf32> sym(1024);
        size_t n = 0;
        while (n < x.size()) {
            for (auto& s : sym) s = cf32((float)gauss(), (float)gauss());
            for (size_t i = 0; i < sym.size() && n < x.size(); i++) x[n++] = sym[i];
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = id(x, fs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        cpuCal("specx_idfeatures", "wide signal", ms, "ms", '<', 200 * ciSlack());
        CHECK(ms < 200 * ciSlack(), "wide signal: %.0f ms (limit %.0f)", ms, 200 * ciSlack());
        (void)r;
    }
    {
        const double fs = 2e6;
        std::vector<cf32> x = fsk({-2400, 2400}, 4800, fs, 1.4);
        shiftNoise(x, fs, 300e3, 25, 12e3);
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = id(x, fs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        cpuCal("specx_idfeatures", "narrow signal in a 2 Msps capture", ms, "ms", '<', 200 * ciSlack());
        CHECK(ms < 200 * ciSlack(), "narrow signal in a 2 Msps capture: %.0f ms (limit %.0f)", ms, 200 * ciSlack());
        CHECK(std::fabs(r.f.symRate / 4800 - 1) < 0.02, "narrow signal in a 2 Msps capture: rate %.1f", r.f.symRate);
    }
    // ---- the text forms, the Memory of a candidate
    {
        const double fs = 250e3;
        std::vector<cf32> x = fsk({-1944, -648, 648, 1944}, 4800, fs, 1.0);
        shiftNoise(x, fs, 0, 30, 9e3);
        Options o; o.rfHz = 446.1e6; o.usePriors = true;
        const Result r = identify(x.data(), x.size(), fs, o);
        const std::string t = toText(r);
        CHECK(t.find("4FSK") != std::string::npos && t.find("sure") != std::string::npos, "text form: %s", t.c_str());
        CHECK(!r.headline.empty() && r.ms > 0, "headline / timing missing");
        CHECK(!r.priors.empty(), "446.1 MHz: the frequency list and the band plans had nothing to say");
        Options blind = o; blind.usePriors = false; blind.rfHz = 0;
        const Result rb = identify(x.data(), x.size(), fs, blind);
        CHECK(rb.priors.empty(), "blind: priors given");
        if (!r.cands.empty()) {
            const Memory m = toMemory(r.cands[0], r.f);
            CHECK(m.freqHz > 445.9e6 && m.freqHz < 446.3e6 && !m.mode.empty(), "memory of the candidate: %llu Hz, mode %s", (unsigned long long)m.freqHz, m.mode.c_str());
        }
    }
    return testSummary("specx_idfeatures");
}
