// TETRAPOL frame coding (PAS 0001-2 clause 6), data link (blocks, HDLC, TPDU, TSDU) and the receiver on the control-channel test signal:
// both polarities (automatic and set by hand), both band codings, frequency and clock offsets, noise.
#include "dect2/tetrapol/tetrapol_codec.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_link.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/engine/modes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
namespace tp = dect2::tetrapol;
#include "../common/test_util.h"

static void testTables() {
    for (int band : {tp::kVhf, tp::kUhf})
        for (int type : {tp::kVoice, tp::kData}) {
            bool seen[tp::kDataBits] = {};
            bool perm = true;
            for (int j = 0; j < tp::kDataBits; j++) {
                const int k = tp::interleaveIndex(j, band, type);
                if (k < 0 || k >= tp::kDataBits || seen[k]) perm = false; else seen[k] = true;
            }
            CHECK(perm, "interleave band %d type %d is not a permutation", band, type);
        }
    // the first 52 positions must agree between voice and data (the type bit is read before the type is known)
    for (int band : {tp::kVhf, tp::kUhf})
        for (int j = 0; j < 52; j++)
            CHECK(tp::interleaveIndex(j, band, tp::kVoice) == tp::interleaveIndex(j, band, tp::kData), "first part differs at %d", j);
    // scrambling sequence: period 127, seven ones first
    int ones = 0;
    for (int k = 0; k < 127; k++) ones += tp::scrambleBit(k, 127);
    CHECK(ones == 64, "m-sequence weight %d", ones);
    CHECK(tp::scrambleBit(0, 127) == 1 && tp::scrambleBit(7, 127) == 0, "sequence start");
}

static void testFrames() {
    std::mt19937 rng(7);
    int bad = 0, fixedOk = 0, tries = 0;
    for (int band : {tp::kVhf, tp::kUhf})
        for (int scr = 0; scr <= 127; scr++) {
            tp::DataFrame d;
            d.scramble = scr;
            for (auto& b : d.payload) b = (uint8_t)(rng() & 1);
            d.asb[0] = (uint8_t)(rng() & 1);
            uint8_t f160[tp::kFrameBits];
            tp::encodeDataFrame(d, band, f160);
            uint8_t h = 0;
            for (int i = 0; i < 8; i++) h |= (uint8_t)(f160[i] << i);
            if (h != tp::kFrameHdr) bad++;
            tp::Frame out;
            if (!tp::decodeFrame(f160 + 8, band, scr, out) || out.type != tp::kData || out.metric != 0 ||
                memcmp(out.data.payload, d.payload, sizeof d.payload) || out.data.asb[0] != d.asb[0]) bad++;
            // a few coded bits wrong: the circular code puts them right
            uint8_t e[tp::kDataBits];
            memcpy(e, f160 + 8, sizeof e);
            for (int k = 0; k < 2; k++) e[rng() % tp::kDataBits] ^= 1;
            tries++;
            if (tp::decodeFrame(e, band, scr, out) && !memcmp(out.data.payload, d.payload, sizeof d.payload)) fixedOk++;
            // the wrong constant must not decode as this frame
            tp::Frame w;
            if (scr > 0 && tp::decodeFrame(f160 + 8, band, (scr + 1) % 128, w) && w.metric == 0) bad++;

            tp::VoiceFrame v;
            v.scramble = scr;
            for (auto& b : v.codec) b = (uint8_t)(rng() & 1);
            tp::encodeVoiceFrame(v, band, f160);
            if (!tp::decodeFrame(f160 + 8, band, scr, out) || out.type != tp::kVoice || memcmp(out.voice.codec, v.codec, sizeof v.codec)) bad++;
        }
    CHECK(bad == 0, "%d frame round trips failed", bad);
    CHECK(fixedOk >= tries * 9 / 10, "two coded bit errors corrected in %d of %d", fixedOk, tries);
}

static void testStream() {
    // the test cycle as it goes on air: header found after the differential, both polarities, 152 bits back
    for (int inv = 0; inv < 2; inv++) {
        std::vector<uint8_t> x = tetrapolTestBits(0, kTetrapolTestScramble, tp::kUhf);
        std::vector<uint8_t> e = x;
        tp::diffEncode(e.data(), (int)e.size(), 0);
        if (inv) for (auto& b : e) b ^= 1;
        int okFrames = 0, n = (int)(e.size() / tp::kFrameBits);
        for (int f = 1; f < n; f++) {   // frame 0's first bit depends on the bit before the stream
            const uint8_t* r = &e[(size_t)f * tp::kFrameBits];
            if (tp::syncErrors(r, inv == 1) != 0 || tp::syncErrors(r, inv == 0) == 0) continue;
            uint8_t b[tp::kDataBits];
            tp::frameBits152(r, inv == 1, b);
            if (!memcmp(b, &x[(size_t)f * tp::kFrameBits + 8], tp::kDataBits)) okFrames++;
        }
        CHECK(okFrames == n - 1, "polarity %d: %d of %d frames", inv, okFrames, n - 1);
    }
}

static void testLink() {
    for (int nb : {8, 16, 24, 40}) {
        std::vector<uint8_t> bytes((size_t)nb);
        for (int i = 0; i < nb; i++) bytes[(size_t)i] = (uint8_t)(i * 37 + 11);
        std::vector<tp::DataFrame> fr = tp::blockFrames(bytes, 9);
        tp::BlockAssembler as;
        std::vector<uint8_t> got;
        int done = 0;
        for (size_t i = 0; i < fr.size(); i++) done += as.push(fr[i], true, got) ? 1 : 0;
        CHECK(done == 1 && got == bytes, "block of %d bytes: %d blocks, %zu bytes back", nb, done, got.size());
        if (nb >= 24) {   // one frame lost: the parity frame repairs it
            tp::BlockAssembler a2;
            done = 0;
            for (size_t i = 0; i < fr.size(); i++) {
                tp::DataFrame f = fr[i];
                const bool lost = i == 1;
                if (lost) memset(f.payload, 0, sizeof f.payload);
                done += a2.push(f, !lost, got) ? 1 : 0;
            }
            CHECK(done == 1 && got == bytes && a2.repaired() == 1, "parity repair of a %d byte block", nb);
        }
    }
    // HDLC with the FCS, TPDU UI, TSDU
    tp::SysInfo si;
    si.country = 33; si.systemId = 7; si.locArea = 101; si.bnId = 2; si.bsId = 18; si.rswId = 3; si.uScramble = 5;
    const std::vector<uint8_t> t = tp::buildSysInfo(si);
    const int nb = tp::uiBlockBytes(t.size());
    std::vector<uint8_t> h = tp::buildHdlc(0, 7, 0xfff, tp::kCmdUi, tp::uiInfo(t, nb), nb);
    tp::Hdlc hd;
    CHECK(tp::parseHdlc(h, hd) && hd.y == 7 && hd.x == 0xfff, "hdlc");
    std::vector<uint8_t> back;
    CHECK(tp::tsduOf(hd, back) && back == t, "tsdu");
    tp::SysInfo so;
    CHECK(tp::parseSysInfo(back.data(), (int)back.size(), so) && so.country == 33 && so.systemId == 7 && so.locArea == 101 && so.bsId == 18 &&
          so.rswId == 3 && so.bnId == 2, "sysinfo fields");
    for (int bit = 0; bit < nb * 8; bit += 7) {
        std::vector<uint8_t> c = h;
        c[(size_t)(bit / 8)] ^= (uint8_t)(1u << (bit % 8));
        if (tp::fcsOk(c.data(), nb)) { CHECK(false, "FCS missed a bit error at %d", bit); break; }
    }
    // a single-frame UI (2 TSDU bytes, no length byte)
    const std::vector<uint8_t> end = tp::buildCallEnd();
    h = tp::buildHdlc(0, 7, 0, tp::kCmdUi, tp::uiInfo(end, 8), 8);
    tp::CallSignal cs;
    CHECK(tp::parseHdlc(h, hd) && tp::tsduOf(hd, back) && tp::parseCallSignal(back.data(), (int)back.size(), cs) && cs.kind == tp::CallSignal::CallEnd, "call end");
    const std::vector<uint8_t> ga = tp::buildGroupActivation(1234, 40, 5, 0x13);
    CHECK(tp::parseCallSignal(ga.data(), (int)ga.size(), cs) && cs.kind == tp::CallSignal::GroupActivation && cs.groupId == 1234 &&
          cs.channelId == 40 && cs.keyRef == 0x13 && !tp::keyRefClear(cs.keyRef), "group activation");
    const std::vector<uint8_t> st = tp::buildCallStart(0x10);
    CHECK(tp::parseCallSignal(st.data(), (int)st.size(), cs) && cs.kind == tp::CallSignal::CallStart && tp::keyRefClear(cs.keyRef), "call start clear");
    const std::vector<uint8_t> st2 = tp::buildCallStart(-1);
    CHECK(tp::parseCallSignal(st2.data(), (int)st2.size(), cs) && cs.keyRef == -1 && !tp::keyRefClear(cs.keyRef), "call start without key reference");
}

static TetrapolTelemetry run(double snr, double cfo, double ppm, double rate, int polarity, int band, double secs, int rxPol = 0, int rxBand = 0) {
    SynthConfig sc;
    sc.mode = 81; sc.snrDb = snr; sc.cfoHz = cfo; sc.sroPpm = ppm;
    sc.modeOpt[1] = 0;
    sc.modeOpt[2] = polarity;
    sc.modeOpt[3] = band;
    auto syn = makeTetrapolSynth(sc, rate);
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetrapolTestCentreHz);
    rx.setPolarity(rxPol);
    rx.setBand(rxBand);
    rx.reset();
    const size_t chunk = (size_t)std::max(1024.0, rate * 0.05);
    std::vector<cf32> buf(chunk);
    TetrapolTelemetry t;
    for (double done = 0; done < secs; done += (double)chunk / rate) {
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
    }
    rx.telemetry(t, 0);
    return t;
}

static bool cellOk(const TetrapolTelemetry& t) {
    for (const auto& c : t.cells)
        if (c.hasSysinfo && c.country == kTetrapolTestCountry && c.systemId == kTetrapolTestSystem && c.cellId == kTetrapolTestCell &&
            c.rswId == kTetrapolTestRsw && c.locArea == kTetrapolTestLa && c.bnId == kTetrapolTestBn && c.scramble == kTetrapolTestScramble)
            return true;
    return false;
}

// A retune while running: the signal moves (the tuned frequency changed inside the capture: only the offset) or the radio's centre
// changed (another carrier). The receiver must find the new carrier again by itself within a few seconds.
static double relockSecs(double rate, double cfo1, double cfo2, double newCentre, int pol2, int scr2) {
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetrapolTestCentreHz);
    rx.setSignalOffset(cfo1);
    rx.reset();
    const size_t chunk = (size_t)std::max(1024.0, rate * 0.05);
    std::vector<cf32> buf(chunk);
    uint64_t okAtRetune = 0;
    auto feedFor = [&](ModeSynth& syn, double secs, double& t0, double centre, bool watch) {
        double found = -1;
        for (double done = 0; done < secs; done += (double)chunk / rate) {
            syn.generate(buf.data(), chunk);
            rx.feed(buf.data(), chunk);
            if (watch && found < 0) {
                TetrapolTelemetry t;
                rx.telemetry(t, 0);
                // 30 good blocks after the retune, and the cell of the new centre heard with its system info
                if (t.blocksOk >= okAtRetune + 30)
                    for (const auto& c : t.cells)
                        if (c.hasSysinfo && c.freqHz == centre && c.scramble == scr2) found = done + (double)chunk / rate;
            }
        }
        t0 += secs;
        return found;
    };
    SynthConfig a;
    a.mode = 81; a.snrDb = 25; a.cfoHz = cfo1; a.sroPpm = 20;
    auto s1 = makeTetrapolSynth(a, rate);
    double t = 0;
    feedFor(*s1, 3.0, t, kTetrapolTestCentreHz, false);
    // the retune: a short gap of noise (the radio settling), then the new carrier
    SynthConfig g;
    g.mode = 81; g.snrDb = -10; g.cfoHz = 0;
    auto gap = makeTetrapolSynth(g, rate);
    feedFor(*gap, 0.3, t, 0, false);
    { TetrapolTelemetry t0; rx.telemetry(t0, 0); okAtRetune = t0.blocksOk; }
    rx.setSignalOffset(cfo2);
    rx.setCenterHz(newCentre);
    SynthConfig b;
    b.mode = 81; b.snrDb = 25; b.cfoHz = cfo2; b.sroPpm = -30;
    b.modeOpt[0] = scr2; b.modeOpt[2] = pol2;
    auto s2 = makeTetrapolSynth(b, rate);
    return feedFor(*s2, 12.0, t, newCentre, true);
}

static void testRetune() {
    struct Case { double rate, cfo1, cfo2, centre; int pol2, scr2; const char* what; };
    const Case cases[] = {
        {250000, 0, 3000, kTetrapolTestCentreHz, 0, 5, "tuned frequency +3 kHz, same centre"},
        {250000, 800, -4200, kTetrapolTestCentreHz, 0, 5, "tuned frequency -5 kHz, offset impairment"},
        {250000, 0, 0, kTetrapolTestCentreHz + 1.5e6, 0, 5, "centre changed"},
        {250000, 600, -900, kTetrapolTestCentreHz + 2.5e6, 1, 9, "centre changed, other cell, inverted, carrier offset"},
        {2400000, 0, 12500, kTetrapolTestCentreHz, 0, 5, "2.4 Msps, next channel"},
    };
    for (const Case& c : cases) {
        const double s = relockSecs(c.rate, c.cfo1, c.cfo2, c.centre, c.pol2, c.scr2);
        printf("  retune (%s): locked again after %.2f s\n", c.what, s);
        CHECK(s > 0 && s < 8.0, "no relock after a retune: %s (%.2f)", c.what, s);
    }
}

int main() {
    // traffic channel frequency from a grant: French 10 kHz raster, Swiss 12.5 kHz, unknown control channel number
    CHECK(std::fabs(tetrapolChannelHz(400.0e6, 10, 14, 10000) - 400.04e6) < 1, "10 kHz raster, +4 channels");
    CHECK(std::fabs(tetrapolChannelHz(400.0e6, 10, 6, 12500) - 399.95e6) < 1, "12.5 kHz raster, -4 channels");
    CHECK(tetrapolChannelHz(400.0e6, -1, 6, 10000) < 0, "control channel number unknown");
    testTables();
    testFrames();
    testStream();
    testLink();
    testRetune();
    const ModeTuning mt = tetrapolTuning();
    CHECK(mt.stdMode == 81 && modeTuningById("tetrapol") && modeTuning(81), "tuning table");

    struct Case { double snr, cfo, ppm, rate; int pol, band; };
    const Case cases[] = {
        {30, 0, 0, 250000, 0, 0}, {30, 0, 0, 250000, 1, 0}, {30, 0, 0, 250000, 0, 1}, {30, 0, 0, 250000, 1, 1},
        {25, 600, 0, 250000, 1, 0}, {25, -900, 40, 250000, 0, 0}, {25, 300, -60, 96000, 1, 1}, {18, 0, 0, 250000, 1, 0},
        {25, 0, 0, 64000, 0, 0},
    };
    for (const Case& c : cases) {
        const TetrapolTelemetry t = run(c.snr, c.cfo, c.ppm, c.rate, c.pol, c.band, 3.0);
        const bool ok = cellOk(t);
        int clear = 0, enc = 0, numbersOnEnc = 0;
        for (const auto& a : t.activations) {
            if (a.verdict == 1 && a.groupId == kTetrapolTestGroup && a.channelId == kTetrapolTestChannel) clear++;
            if (a.verdict == 2) { enc++; if (a.groupId >= 0 || a.channelId >= 0) numbersOnEnc++; }
        }
        printf("  snr %.0f cfo %+.0f ppm %+.0f rate %.0f pol %d band %d: %s; ok %llu bad %llu pol %d band %d scr %d, calls clear %d enc %d\n",
               c.snr, c.cfo, c.ppm, c.rate, c.pol, c.band, tetrapolSummary(t).c_str(), (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, t.polarity, t.band, t.scramble, clear, enc);
        CHECK(ok, "no cell (snr %.0f cfo %.0f ppm %.0f pol %d band %d)", c.snr, c.cfo, c.ppm, c.pol, c.band);
        CHECK(t.polarity == (c.pol ? 2 : 1), "polarity found %d, sent %d", t.polarity, c.pol);
        CHECK(t.band == (c.band ? tp::kVhf : tp::kUhf), "band found %d", t.band);
        CHECK(clear == 1 && enc == 1 && numbersOnEnc == 0, "activations clear %d enc %d (numbers kept on encrypted: %d)", clear, enc, numbersOnEnc);
    }
    // a 20 ppm radio at 400 MHz is 8 kHz out (the sweep uses a quarter of the channel, 3.1 kHz): near the threshold the frames decoded must stay close to
    // those of a signal on the centre (before the frequency control 3.1 kHz at 12 dB gave half of them)
    for (double cfo : {3125.0, -3125.0}) {
        const TetrapolTelemetry on = run(12, 0, 0, 250000, 0, 0, 3.0), off = run(12, cfo, 0, 250000, 0, 0, 3.0);
        printf("  snr 12: centre ok %llu, cfo %+.0f ok %llu\n", (unsigned long long)on.blocksOk, cfo, (unsigned long long)off.blocksOk);
        CHECK(off.blocksOk * 10 >= on.blocksOk * 8, "cfo %+.0f at 12 dB: %llu frames against %llu on the centre", cfo, (unsigned long long)off.blocksOk, (unsigned long long)on.blocksOk);
    }
    // polarity set by hand: the right one works, the wrong one never syncs
    {
        const TetrapolTelemetry r = run(30, 0, 0, 250000, 1, 0, 2.0, 2);
        CHECK(cellOk(r) && !r.polarityAuto, "inverted set by hand");
        const TetrapolTelemetry w = run(30, 0, 0, 250000, 1, 0, 2.0, 1);
        CHECK(!cellOk(w) && w.blocksOk == 0, "wrong polarity by hand decoded %llu frames", (unsigned long long)w.blocksOk);
        const TetrapolTelemetry b = run(30, 0, 0, 250000, 0, 1, 2.0, 0, 2);
        CHECK(!cellOk(b), "UHF coding forced on a VHF signal");
    }
    // noise only: no cell, no frames
    {
        const TetrapolTelemetry n = run(-200, 0, 0, 250000, 0, 0, 0.0);
        (void)n;
        SynthConfig sc;
        TetrapolReceiver rx;
        rx.configure(250000);
        rx.reset();
        std::mt19937 g(3);
        std::normal_distribution<float> nd(0.f, 0.1f);
        std::vector<cf32> buf(12500);
        for (int k = 0; k < 60; k++) {
            for (auto& v : buf) v = cf32(nd(g), nd(g));
            rx.feed(buf.data(), buf.size());
        }
        TetrapolTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.cells.empty() || !t.cells.front().hasSysinfo, "a cell out of noise");
        CHECK(t.voiceFrames == 0 && t.tsdus == 0, "frames out of noise: voice %llu tsdu %llu", (unsigned long long)t.voiceFrames, (unsigned long long)t.tsdus);
    }
    return testSummary("tetrapol_rx");
}
