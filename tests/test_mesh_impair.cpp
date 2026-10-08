// Mesh (LoRa) under impairments, at the LoRa layer: carrier offset +-20 kHz (869 MHz: radio and node crystals), clock offset +-50 ppm
// (and +-30 ppm on SF12), 8-bit samples, a DC offset, a 20 ms gap, a reset in the middle, chunks of 1 / 7 / 4096 / 65536 samples,
// two frames at once on different frequencies, and a weak frame right after a strong one.
#include "data/mesh/testutil.h"
using namespace dect2;
using namespace meshtest;

static lora::Params longFast() { lora::Params p; p.sf = 11; p.bwHz = 250e3; p.cr = 5; p.preamble = 16; p.syncWord = 0x2B; return p; }
static lora::Params meshCore() { lora::Params p; p.sf = 8; p.bwHz = 62.5e3; p.cr = 8; p.preamble = 32; p.syncWord = 0x12; return p; }
static lora::Params sf12() { lora::Params p; p.sf = 12; p.bwHz = 125e3; p.cr = 8; p.preamble = 16; p.ldro = true; return p; }

int main() {
    const struct { const char* name; lora::Params p; } sets[] = {{"LongFast", longFast()}, {"MeshCore EU", meshCore()}, {"SF12 125 kHz", sf12()}};
    // carrier offset
    for (const auto& s : sets)
        for (double cfo : {-20e3, -12e3, -3e3, 3e3, 12e3, 20e3}) {
            Trial t;
            t.p = s.p; t.rate = 1e6; t.off = 200e3; t.cfoHz = cfo; t.snrDb = 5; t.frames = 4; t.len = 40; t.seed = 3;
            std::vector<lora::RxFrame> fr;
            const int ok = runTrial(t, &fr);
            double err = 0;
            for (const auto& f : fr) err = std::max(err, std::fabs(f.cfoHz - cfo));
            printf("%-12s cfo %+6.0f Hz: %d/4, estimate within %.1f Hz\n", s.name, cfo, ok, err);
            CHECK(ok == 4 && err < 0.05 * s.p.bwHz / (1 << s.p.sf) + 5, "%s cfo %.0f: %d/4, error %.1f Hz", s.name, cfo, ok, err);
        }
    // clock offset (the transmitter's symbols are longer or shorter); 255-byte SF12 frames last 13 s
    for (const auto& s : sets)
        for (double ppm : {-50.0, -30.0, 30.0, 50.0}) {
            Trial t;
            t.p = s.p; t.rate = 4 * s.p.bwHz; t.sroPpm = ppm; t.snrDb = 5; t.frames = 2; t.len = s.p.sf == 12 ? 255 : 120; t.seed = 4;
            std::vector<lora::RxFrame> fr;
            const int ok = runTrial(t, &fr);
            double est = fr.empty() ? 0 : fr.back().sfoPpm;
            printf("%-12s clock %+3.0f ppm, %zu-byte frames: %d/2, measured %+.1f ppm\n", s.name, ppm, t.len, ok, est);
            CHECK(ok == 2, "%s clock %.0f ppm: %d/2", s.name, ppm, ok);
        }
    // 8-bit samples, DC offset, both at a low SNR; chunk sizes; a gap; a reset
    for (const auto& s : sets) {
        Trial base;
        base.p = s.p; base.rate = 1e6; base.off = -200e3; base.snrDb = s.p.sf == 8 ? -6 : -12; base.frames = 6; base.seed = 5;
        if (s.p.sf == 12) base.snrDb = -16;
        Trial q = base; q.q8 = true; q.dc = 0.03;
        const int okQ = runTrial(q);
        printf("%-12s 8-bit + DC offset at %.0f dB: %d/6\n", s.name, base.snrDb, okQ);
        CHECK(okQ >= 5, "%s 8-bit + DC: %d/6", s.name, okQ);
        std::vector<std::vector<lora::RxFrame>> byChunk;
        for (size_t c : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
            Trial t = base; t.chunk = c; t.snrDb = 10;
            std::vector<lora::RxFrame> fr;
            const int ok = runTrial(t, &fr);
            CHECK(ok == 6, "%s chunk %zu: %d/6", s.name, c, ok);
            byChunk.push_back(fr);
        }
        bool same = true;
        for (const auto& v : byChunk) {
            same &= v.size() == byChunk[0].size();
            for (size_t i = 0; same && i < v.size(); i++) same &= v[i].payload == byChunk[0][i].payload && std::fabs(v[i].cfoHz - byChunk[0][i].cfoHz) < 0.01;
        }
        CHECK(same, "%s: the chunk size changes the result", s.name);
        // 20 ms of nothing in the middle of the second frame: it is lost, the others come
        Trial g = base; g.snrDb = 10;
        {
            std::mt19937 gg(g.seed);
            const double first = 0.03 + 0.01 * (gg() % 10);
            const double dur = lora::airSeconds(g.p, g.len);
            g.gapAt = first + dur * 1.5; g.gapSec = 0.02;
        }
        const int okG = runTrial(g);
        printf("%-12s 20 ms gap: %d/6\n", s.name, okG);
        CHECK(okG >= 5, "%s gap: %d/6", s.name, okG);
        Trial r = base; r.snrDb = 10;
        {
            std::mt19937 gg(r.seed);
            r.resetAt = 0.03 + 0.01 * (gg() % 10) + lora::airSeconds(r.p, r.len) * 2.5;
        }
        const int okR = runTrial(r);
        printf("%-12s reset in the third frame: %d/6\n", s.name, okR);
        CHECK(okR >= 5, "%s reset: %d/6", s.name, okR);
    }
    // two frames at the same time on different frequencies (LongFast and MeshCore EU are 93 kHz apart: MeshCore lies inside the
    // LongFast band), at the same SNR and with LongFast 10 and 20 dB stronger; and a weak LongFast frame 20 ms after a strong one
    for (double lfSnr : {10.0, 20.0, 30.0}) {
        const double rate = 2e6, noise = 0.05;
        std::mt19937 g(9);
        lora::Params a = longFast(), b = meshCore();
        const auto pa = randomBytes(g, 50), pb = randomBytes(g, 50), pc = randomBytes(g, 30);
        lora::TxFrame fa, fb, fc;
        fa.p = a; fa.data = lora::encode(a, pa.data(), pa.size()); fa.startSec = 0.05; fa.freqHz = -300e3; fa.amp = ampFor(lfSnr, a.bwHz, rate, noise);
        fb.p = b; fb.data = lora::encode(b, pb.data(), pb.size()); fb.startSec = 0.2; fb.freqHz = -207e3; fb.amp = ampFor(10, b.bwHz, rate, noise);
        fc.p = a; fc.data = lora::encode(a, pc.data(), pc.size()); fc.startSec = fa.endSec() + 0.02; fc.freqHz = -300e3; fc.amp = ampFor(-12, a.bwHz, rate, noise);
        const auto x = render({fa, fb, fc}, std::max(fb.endSec(), fc.endSec()) + 0.1, rate, noise, 3);
        Chain ca(rate, -300e3, a), cb(rate, -207e3, b);
        ca.feedAll(x, 4096); cb.feedAll(x, 4096);
        const bool gotA = ca.frames.size() >= 1 && ca.frames[0].crcOk && ca.frames[0].payload == pa;
        const bool gotC = ca.frames.size() >= 2 && ca.frames[1].crcOk && ca.frames[1].payload == pc;
        const bool gotB = cb.frames.size() == 1 && cb.frames[0].crcOk && cb.frames[0].payload == pb;
        printf("LongFast at %2.0f dB: decoded %d; MeshCore at 10 dB at the same time %d; LongFast at -12 dB 20 ms later %d\n", lfSnr, gotA, gotB, gotC);
        CHECK(gotA && gotC, "LongFast at %.0f dB / the weak one after it", lfSnr);
        if (lfSnr <= 20) CHECK(gotB, "MeshCore under LongFast at %.0f dB", lfSnr);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
