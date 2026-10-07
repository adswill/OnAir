// DRM transmitter and back end without the synchronisation: the transmitter's baseband is cut into symbols at the exact positions, transformed, and
// the cells go to the back end (FAC, SDC, time deinterleaver, multilevel decoder). Checks the structure of the signal and the formats.
#include "dect2/drm_dec.h"
#include "dect2/drm_fft.h"
#include "dect2/drm_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::drm;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Result {
    int frames = 0, exact = 0, parseBad = 0, gaps = 0;
    std::string label, text;
    bool facOk = false;
    uint64_t sdcOk = 0, sdcBad = 0;
    int occ = -1;
};

static Result run(const DrmTxConfig& cfg, int superFrames, double noiseDb = 200) {
    Result res;
    DrmTransmitter tx(cfg, std::make_unique<DrmPatternSource>(cfg.seed));   // a known pattern: the test compares frames bit for bit (nullptr would send the AAC test melody)
    CHECK(tx.ok(), "transmitter configuration rejected (mode %d occ %d qam %d)", cfg.mode, cfg.occupancy, cfg.mscQam);
    if (!tx.ok()) return res;
    std::vector<cf32> sig;
    for (int i = 0; i < superFrames; i++) tx.superFrame(sig);
    const ModeParams& mp = modeParams(cfg.mode);
    const int N = mp.tu12 * 4, G = mp.tg12 * 4, ts = N + G;
    auto L = layout(cfg.mode, cfg.occupancy);
    if (noiseDb < 100) {   // white noise at the given SNR per sample (signal power 1)
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.f, (float)std::sqrt(std::pow(10.0, -noiseDb / 10.0) / 2));
        for (auto& v : sig) v += cf32(nd(rng), nd(rng));
    }
    DrmFft fft(N);
    DrmBackend be;
    be.setMode(cfg.mode);
    std::vector<std::vector<uint8_t>> expect;
    const int nAudio = tx.audioFrames();
    const int textBytes = cfg.textMessage ? 4 : 0;
    const int len = tx.streamBytes();
    TextDecoder td;
    long lastIdx = -1;
    be.onLogical = [&](const LogicalFrame& f) {
        res.frames++;
        if (f.lenA + f.lenB != len) { res.parseBad++; return; }
        AacSuperFrame sf;
        const int lenA = f.lenA;
        if (!aacSuperFrameParse(f.data, len - textBytes, lenA, nAudio, sf)) { res.parseBad++; return; }
        const long idx = ((long)sf.frames[0][0] << 8) | sf.frames[0][1];
        std::vector<std::vector<uint8_t>> fr;
        std::vector<uint8_t> crc;
        DrmPatternSource::pattern(cfg.seed, (uint64_t)idx, nAudio, tx.audioPayload(), fr, crc);
        if (sf.frames == fr && sf.crc == crc) res.exact++; else res.parseBad++;
        if (lastIdx >= 0 && idx != lastIdx + 1) res.gaps++;
        lastIdx = idx;
        if (textBytes) td.feed(f.data + len - 4);
    };
    std::vector<cf32> bins((size_t)N);
    for (int fr = 0; fr < superFrames * L->frames; fr++) {
        CellGrid g;
        g.resize(mp.ns, L->kmin, L->kmax);
        for (int s = 0; s < mp.ns; s++) {
            const cf32* p = &sig[(size_t)(fr * mp.ns + s) * (size_t)ts + (size_t)G];
            for (int i = 0; i < N; i++) bins[(size_t)i] = p[i];
            fft.forward(bins.data());
            // flat channel: the gain is the mean ratio at the pilots
            const int gs = (fr % L->frames) * mp.ns + s;
            cf32 h(0, 0);
            int n = 0;
            for (const Pilot& pl : L->pilots[(size_t)gs]) { h += bins[(size_t)((pl.k + N) % N)] / pl.ref; n++; }
            h /= (float)n;
            for (int k = L->kmin; k <= L->kmax; k++) {
                g.z[(size_t)s * (size_t)g.width() + (size_t)(k - L->kmin)] = bins[(size_t)((k + N) % N)] / h;
                g.w[(size_t)s * (size_t)g.width() + (size_t)(k - L->kmin)] = 1.f;
            }
        }
        be.frame(g);
    }
    res.facOk = be.facValid();
    res.occ = be.occupancy();
    res.sdcOk = be.sdcOk; res.sdcBad = be.sdcBad;
    if (be.sdcValid()) res.label = be.sdc().label[0].text;
    res.text = td.text();
    if (be.facValid()) {
        const FacInfo& f = be.fac();
        CHECK(f.occupancy == cfg.occupancy && f.interleaver == (cfg.longInterleave ? 0 : 1) && f.svc[0].id == cfg.serviceId && f.svc[0].language == cfg.language &&
              f.svc[0].descriptor == cfg.programmeType, "FAC fields differ from the configuration");
    }
    return res;
}

int main() {
    struct Cfg { int mode, occ, qam, protB, partA; bool lng; const char* name; };
    const Cfg cfgs[] = {
        {kModeB, 3, 64, 1, 0, true, "B 10 kHz 64-QAM long"},   {kModeA, 2, 64, 1, 0, true, "A 9 kHz 64-QAM long"},    {kModeA, 5, 16, 0, 0, false, "A 20 kHz 16-QAM short"},
        {kModeC, 3, 64, 3, 0, true, "C 10 kHz 64-QAM PL3 long"}, {kModeD, 3, 16, 1, 0, true, "D 10 kHz 16-QAM long"}, {kModeB, 0, 16, 0, 0, true, "B 4.5 kHz 16-QAM long"},
        {kModeA, 3, 64, 1, 90, true, "A 10 kHz 64-QAM UEP"},   {kModeB, 4, 64, 2, 120, false, "B 18 kHz 64-QAM UEP short"}, {kModeD, 5, 64, 0, 0, false, "D 20 kHz 64-QAM PL0 short"},
    };
    for (const Cfg& c : cfgs) {
        DrmTxConfig t;
        t.mode = c.mode; t.occupancy = c.occ; t.mscQam = c.qam; t.protB = c.protB; t.partABytes = c.partA; t.longInterleave = c.lng;
        t.protA = 0;
        const int sfs = c.lng ? 9 : 5;
        const Result r = run(t, sfs);
        printf("%-28s FAC %d occ %d  SDC %llu/%llu '%s'  frames %d exact %d bad %d gaps %d  text '%s'\n", c.name, r.facOk, r.occ, (unsigned long long)r.sdcOk, (unsigned long long)r.sdcBad,
               r.label.c_str(), r.frames, r.exact, r.parseBad, r.gaps, r.text.c_str());
        CHECK(r.facOk && r.occ == c.occ, "%s: FAC not read", c.name);
        CHECK(r.label == "OnAir DRM", "%s: label '%s'", c.name, r.label.c_str());
        CHECK(r.frames >= (c.lng ? 3 : 8) && r.exact == r.frames && r.parseBad == 0 && r.gaps == 0, "%s: %d frames, %d exact, %d bad, %d gaps", c.name, r.frames, r.exact, r.parseBad, r.gaps);
        CHECK(r.text == t.text || r.frames < 12, "%s: text message '%s'", c.name, r.text.c_str());
    }
    // a little noise: still exact
    {
        DrmTxConfig t;
        const Result r = run(t, 9, 30);
        printf("B 10 kHz at 30 dB: frames %d exact %d\n", r.frames, r.exact);
        CHECK(r.frames > 0 && r.exact == r.frames, "decoding at 30 dB: %d of %d", r.exact, r.frames);
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
