// General / Auto on the test signals of many modes: each mode's generator is rendered (the same ones as the identifier's matrix), moved off the centre
// by a random offset with noise, captured through the identifier thread the way the app does (Capture::startAll), and the labels go through the
// General / Auto state machine: the right label appears at the right place, Decode returns that mode, tuning away returns to General, and tuning
// back offers the label again. The modes that must pass are listed in kMust; the others are printed (the identifier's own accuracy per mode is in
// docs/IDENTIFIER.md).
#include "dect2/common/stable_random.h"
#include "dect2/engine/general_auto.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"


namespace {

struct Case {
    std::string label, modeId, subId;   // the truth: ModeTuning::id and the sub-mode id ("" none)
    int std = 0;                        // engine standard (makeModeSynth); legacy: 0 = DVB-T2, -1 = DVB-T, -2 = ATSC
    const SubMode* sub = nullptr;
    double rate = 0, mhz = 0;
};

std::vector<Case> allCases() {
    std::vector<Case> v;
    auto add = [&](const char* label, const char* id, const char* sub, int std, double rate, double mhz, const SubMode* s = nullptr) {
        Case c; c.label = label; c.modeId = id; c.subId = sub; c.std = std; c.rate = rate; c.mhz = mhz; c.sub = s; v.push_back(c);
    };
    add("dvbt2", "dvbt", "", 0, 10e6, 522);
    add("dvbt", "dvbt", "", -1, 10e6, 522);
    add("atsc", "atsc", "", -2, 10e6, 575);
    add("dab", "dab", "", 4, 2.048e6, 227.36);
    add("atsc3", "atsc3", "", 5, 8e6, 575);
    add("isdbt", "isdbt", "", 6, 8e6, 557);
    add("fm", "fm", "", 7, 1e6, 98.5);
    int nsub = 0;
    const SubMode* subs = subModes(nsub);
    std::set<int> hasSub;
    for (int i = 0; i < nsub; i++) hasSub.insert(subs[i].stdMode);
    for (int s = 8; s < 200; s++) {
        const ModeTuning* mt = modeTuning(s);
        if (!mt || hasSub.count(s)) continue;
        double rate = std::max(mt->sampleRate, mt->minSampleRate);
        if (rate > 12e6 && mt->minSampleRate <= 12e6) rate = std::max(mt->minSampleRate, 10e6);
        add(mt->id, mt->id, "", s, rate, mt->defMhz);
    }
    for (int i = 0; i < nsub; i++) {
        const ModeTuning* mt = modeTuning(subs[i].stdMode);
        if (!mt) continue;
        double rate = std::max(mt->sampleRate, mt->minSampleRate);
        if (rate > 12e6) rate = 10e6;
        add(subs[i].id, mt->id, subs[i].id, subs[i].stdMode, rate, subs[i].defMhz);
        v.back().sub = &subs[i];
    }
    return v;
}

// n samples of a mode's test signal at the rate (after a short settling stretch)
bool render(Case& c, size_t n, std::vector<cf32>& out, std::string& why) {
    SynthConfig sc;
    sc.snrDb = 60;   // clean: the noise is added afterwards, at the SNR the case asks for
    const size_t skip = (size_t)(c.rate * 0.1);
    std::vector<cf32> all;
    if (c.std <= 0) {   // the TV standards come through the synthetic source (paced fast)
        DeviceInfo d;
        TuneSettings t;
        t.centerHz = c.mhz * 1e6; t.sampleRate = c.rate; t.bandwidthMhz = 8;
        t.synth.snrDb = 60; t.synth.pace = 40;
        t.synth.atsc = c.std == -2; t.synth.dvbt = c.std == -1;
        if (c.std == -2) t.bandwidthMhz = 6;
        IqRing ring(1u << 24);
        auto src = makeSource(d);
        if (!src->start(t, ring, why)) return false;
        all.resize(skip + n);
        size_t got = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (got < all.size() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
            const size_t k = ring.read(all.data() + got, all.size() - got);
            got += k;
            if (!k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        c.rate = src->sampleRate() > 0 ? src->sampleRate() : c.rate;   // the source runs at its own rate (the native one of the standard)
        src->stop();
        if (got < all.size()) { why = "source too slow"; return false; }
    } else {
        sc.mode = c.std;
        if (c.std == 4) sc.dab = true;
        if (c.sub) applySubModeSynth(sc, *c.sub);
        auto g = makeModeSynth(c.std, sc, c.rate);
        if (!g) { why = "no generator"; return false; }
        all.resize(skip + n);
        g->generate(all.data(), all.size());
    }
    out.assign(all.begin() + (long)skip, all.end());
    return true;
}

struct Rng { std::mt19937 g; explicit Rng(uint32_t s) : g(s) {} double u(double a, double b) { return dect2::uniform_real_distribution<double>(a, b)(g); } double n() { return dect2::normal_distribution<double>(0, 1)(g); } };

// The signal the identifier is asked about is the strongest one in the clean render (a few generators put theirs off the centre, or send several
// channels). It is moved by a random frequency, noise is added so that the SNR is that signal's average power over its width, and the click
// lands near its middle.
struct Prepared { std::vector<cf32> x; double offHz = 0, bw = 0, clickOff = 0; };
Prepared prepare(const std::vector<cf32>& clean, double fs, double snrDb, Rng& r) {
    Prepared p;
    double power = 0;
    for (const cf32& v : clean) power += std::norm(v);
    power /= clean.size();
    const std::vector<Region> regs = findRegions(clean.data(), clean.size(), fs, 24, 10);
    double c0 = 0, w0 = fs * 0.1, frac = 1;
    if (!regs.empty()) {
        c0 = regs[0].centreHz; w0 = std::max(regs[0].bwHz, fs / 4096 * 4);
        double tot = 0;
        for (const Region& g : regs) tot += std::pow(10.0, g.powerDb / 10);
        frac = std::pow(10.0, regs[0].powerDb / 10) / tot;
    }
    p.bw = w0;
    const double lim = 0.45 * fs - 0.5 * w0;
    const double lo = -lim - c0, hi = lim - c0;
    p.offHz = hi > lo + 1e3 ? r.u(lo, hi) : 0;
    p.x = clean;
    double ph = 0; const double d = 2 * M_PI * p.offHz / fs;
    const double sigma = std::sqrt(power * frac / std::pow(10.0, snrDb / 10) * (fs / w0) / 2);
    for (auto& v : p.x) {
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        ph = std::fmod(ph + d, 2 * M_PI);
        v += cf32((float)(sigma * r.n()), (float)(sigma * r.n()));
    }
    p.clickOff = c0 + p.offHz + r.u(-0.2, 0.2) * std::min(w0, 25e3);   // a click lands near, not exactly on, the middle
    return p;
}
} // namespace

namespace ga = dect2::genauto;

// the modes that must come out: label, then the checks below hold for each of them (the strictest signals of the identifier's matrix)
static const char* const kMust[] = {"dvbt2", "atsc", "fm", "dmr", "pocsag"};

int main(int argc, char** argv) {
    const bool all = argc > 1 && !std::strcmp(argv[1], "--all");
    std::vector<Case> cases = allCases();
    int passed = 0, ran = 0;
    for (Case& c : cases) {
        const bool must = std::find_if(std::begin(kMust), std::end(kMust), [&](const char* m) { return c.label == m; }) != std::end(kMust);
        if (!must && !all) continue;
        std::vector<cf32> clean;
        std::string why;
        const size_t n = (size_t)std::min(c.rate * captureSeconds(c.rate), 3.0e6);
        if (!render(c, n, clean, why)) { if (must) CHECK(false, "%s: no signal (%s)", c.label.c_str(), why.c_str()); continue; }
        ran++;
        Rng r(0x777u + (uint32_t)ran);
        Prepared p = prepare(clean, c.rate, 24, r);
        const double rf0 = c.mhz * 1e6;
        // the capture as the app runs it: the whole span, every signal
        Capture cap;
        cap.startAll(c.rate, rf0, captureSeconds(c.rate), 12);
        // the capture may want more samples than the rendered stretch (10 Msps: 0.4 s): the stretch is pushed again
        for (size_t o = 0, guard = 0; cap.armed() && guard < 40 * p.x.size() / 65536 + 40; o = (o + 65536) % p.x.size(), guard++) cap.push(p.x.data() + o, std::min<size_t>(65536, p.x.size() - o));
        std::vector<Capture::Found> found; uint64_t seq = 0;
        for (int i = 0; i < 2400 && !cap.resultAll(found, seq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::vector<ga::Detection> v;
        for (const auto& f : found) { ga::Detection d; if (ga::fromFound(f, rf0, d)) v.push_back(d); }
        for (const auto& f : found) printf("    found %+9.1f kHz bw %7.1f kHz: %s\n", f.region.centreHz / 1e3, f.region.bwHz / 1e3, f.res.cands.empty() ? "-" : (f.res.cands[0].name + " " + std::to_string((int)(f.res.cands[0].confidence * 100)) + "% verdict " + std::to_string(f.res.verdict)).c_str());
        ga::GeneralAuto g;
        double t = 0;
        for (int k = 0; k < 2; k++) { t += 6; g.scanStarted(t); g.scanDone(t, rf0 - c.rate / 2, rf0 + c.rate / 2, v); }
        const ga::Label* hit = nullptr;
        for (const ga::Label* l : g.visible())
            if (l->d.modeId == c.modeId && (c.subId.empty() || l->d.subId == c.subId) && std::fabs(l->d.rfHz - (rf0 + p.clickOff)) < std::max(25e3, p.bw)) hit = l;
        bool ok = hit != nullptr;
        printf("%-14s off %+8.0f kHz bw %7.1f kHz: %zu found, %zu labels -> %s\n", c.label.c_str(), p.offHz / 1e3, p.bw / 1e3, found.size(), g.visible().size(),
               hit ? (hit->d.name + " " + std::to_string((int)(hit->confidence * 100)) + "%").c_str() : "(no matching label)");
        if (hit) {
            const int id = hit->id;
            const double rf = hit->d.rfHz;
            t += 1;
            const ga::Step s = g.decode(t, id);
            ok = ok && s.act == ga::Action::Decode && !s.mem.mode.empty() && std::fabs((double)s.mem.freqHz - rf) < 1.0;
            ok = ok && g.tick(t + 3, rf + 1e3).act == ga::Action::None;                              // tuned a hair off: keeps decoding
            ok = ok && g.tick(t + 4, rf + std::max(200e3, 2 * c.rate)).act == ga::Action::ReturnToGeneral;   // far away: back to General
            ok = ok && g.state() == ga::State::General;
            g.scanStarted(t + 20); g.scanDone(t + 20, rf0 - c.rate / 2, rf0 + c.rate / 2, v);
            bool again = false;
            for (const ga::Label* l : g.visible()) if (l->id == id) again = true;
            ok = ok && again;
        }
        if (must) CHECK(ok, "%s: label, decode, return or offer-again failed", c.label.c_str());
        if (ok) passed++;
    }
    printf("%d of %d modes passed the General / Auto round trip\n", passed, ran);
    return testSummary("genauto_modes");
}
