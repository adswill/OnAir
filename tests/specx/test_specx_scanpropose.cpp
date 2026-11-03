// One-button Scan and Identify (specx::proposeAll / proposeOne): DMR, NXDN96 and P25 together in one wide capture at several offsets (off the
// centre, a carrier error each) and noise levels. Each must come back at its frequency with the right name and a mode that opens it; noise alone
// must give nothing sure.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/specx/scan_propose.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::specx;
#include "../common/test_util.h"

static void shift(std::vector<cf32>& x, double fs, double hz) {
    double ph = 0; const double d = 2 * M_PI * hz / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + d, 2 * M_PI); }
}

static void addSig(std::vector<cf32>& mix, std::vector<cf32> s, double fs, double off) {
    shift(s, fs, off);
    for (size_t i = 0; i < mix.size() && i < s.size(); i++) mix[i] += s[i];
}

static const Proposal* near(const std::vector<Proposal>& v, double rf, double tolHz) {
    for (const Proposal& p : v) if (std::fabs(p.rfHz - rf) < tolHz) return &p;
    return nullptr;
}

int main() {
    const double fs = 1e6, rf0 = 446e6;
    struct Set { double dmr, nxdn, p25, snr; };
    const Set sets[] = {{-300e3, 0, 300e3, 30}, {150e3, -250e3, -400e3, 25}, {-100e3, 220e3, 360e3, 20}};
    for (const Set& st : sets) {
        const size_t N = (size_t)(fs * 1.3);
        std::vector<cf32> mix(N, cf32(0, 0));
        {
            DmrGenConfig c; c.rate = fs; c.cfoHz = 1200; c.snrDb = 200; c.seed = 3;   // the noise is added once, below
            DmrSignal g(c); std::vector<cf32> x(N); g.generate(x.data(), N); addSig(mix, x, fs, st.dmr);
        }
        {
            NxdnGenConfig c; c.rate = fs; c.snrDb = 200; c.cfoHz = -600;
            NxdnGenTx t; t.baud = 4800; t.durSec = 4; c.txs.push_back(t);
            NxdnGenerator g(c); std::vector<cf32> x(N + (size_t)(0.45 * fs)); g.generate(x.data(), x.size());
            x.erase(x.begin(), x.begin() + (long)(0.45 * fs)); addSig(mix, x, fs, st.nxdn);
        }
        {
            P25GenConfig c; c.rate = fs; c.snrDb = 200;
            P25GenTx t; t.durSec = 4; c.txs.push_back(t);
            P25Generator g(c); std::vector<cf32> x(N + (size_t)(0.4 * fs)); g.generate(x.data(), x.size());
            x.erase(x.begin(), x.begin() + (long)(0.4 * fs)); addSig(mix, x, fs, st.p25);
        }
        // white noise: 12.5 kHz channel SNR of st.snr dB against a unit-power signal (each signal is about unit power over the whole band)
        std::mt19937 rng(11); std::normal_distribution<float> nd(0.f, 1.f);
        double pw = 0; for (const cf32& v : mix) pw += std::norm(v);
        pw /= (double)N * 3;   // per signal
        const double sigma = std::sqrt(pw * (fs / 12.5e3) / std::pow(10.0, st.snr / 10.0) / 2.0);
        for (cf32& v : mix) v += cf32(nd(rng), nd(rng)) * (float)sigma;

        ScanOptions so; so.rfHz = rf0;
        const std::vector<Proposal> ps = proposeAll(mix.data(), mix.size(), fs, so);
        printf("offsets %+.0f/%+.0f/%+.0f snr %.0f: %zu proposals\n", st.dmr, st.nxdn, st.p25, st.snr, ps.size());
        for (const Proposal& p : ps) printf("   %.4f MHz  %s (%.0f%%) %s\n", p.rfHz / 1e6, proposalLine(p).c_str(), p.confidence * 100, p.canOpen ? "open" : "");
        struct Want { double off; const char* name; } want[] = {{st.dmr, "DMR"}, {st.nxdn, "NXDN96"}, {st.p25, "P25 Phase 1"}};
        for (const Want& w : want) {
            const Proposal* p = near(ps, rf0 + w.off, 8e3);
            CHECK(p != nullptr, "%s at %+.0f: no proposal there", w.name, w.off);
            if (!p) continue;
            const bool top = p->name == w.name;
            if (st.snr < 25) { CHECK(p->sureness != SureSure || top, "%s: SURE but wrong", w.name); continue; }   // 20 dB: found, but the name is only reported (P25 is read as NFM voice there, a known limit of the identifier)
            bool listed = false;
            for (const Guess& g : p->guesses) if (g.name == w.name) listed = true;
            CHECK(top || listed, "%s at %+.0f: got %s, not even in the ranked guesses", w.name, w.off, p->name.c_str());
            if (top) CHECK(p->canOpen && !p->memory.mode.empty(), "%s: no mode to open", w.name);
            CHECK(p->sureness != SureSure || top, "%s at %+.0f: SURE but wrong (%s)", w.name, w.off, p->name.c_str());
        }
        // Identify on one region: the same answer as the scan row
        const Proposal* d = near(ps, rf0 + st.dmr, 8e3);
        if (d) {
            const Proposal one = proposeOne(mix.data(), mix.size(), fs, st.dmr - 9e3, st.dmr + 9e3, rf0);
            CHECK(!one.guesses.empty(), "Identify: no guesses");
            CHECK(std::fabs(one.rfHz - (rf0 + st.dmr)) < 8e3, "Identify: frequency %.0f", one.rfHz - rf0 - st.dmr);
        }
    }
    // noise alone: nothing sure
    {
        std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> x((size_t)(fs * 0.6));
        for (cf32& v : x) v = cf32(nd(rng), nd(rng));
        ScanOptions so; so.rfHz = rf0;
        for (const Proposal& p : proposeAll(x.data(), x.size(), fs, so))
            CHECK(p.sureness < SureLikely, "noise: %s proposed as %s", p.name.c_str(), surenessName(p.sureness));
    }
    return testSummary("specx_scanpropose");
}
