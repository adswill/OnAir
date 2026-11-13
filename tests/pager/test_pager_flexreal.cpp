// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLEX as real transmitters send it (issues #95 "Flex Next" and #261). multimon-ng's FLEX_NEXT is its newer FLEX decoder, not a new protocol: the
// recordings users sent are ordinary FLEX (sync codes 0x870C, 0xB068, 0xDEA0) that OnAir did not decode because
//   1. the FIW and the phase data are sent complemented against the sync code and marker (no real FIW passed its check before),
//   2. for a long address the alphanumeric header is the second vector word and the text starts at the start index,
//   3. the BIW and the first vector word carry a 4-bit check, which now rejects words that are not what they seem.
// This test builds frames the way the recordings show (pager_gen.cpp flexFrameTx) and runs them through the receiver at every FLEX speed, with
// noise, carrier offsets, clock errors, both polarities, short and long addresses; checks the bit-level rules; and, only when a recording is given,
// runs the receiver over it and prints counts (frames, code words, pages per type), never what the pages say or who they are for:
//   ONAIR_FLEX_IQ=<wav>      complex baseband, 16-bit stereo WAV (I, Q), any rate the receiver takes; ONAIR_FLEX_IQ_OFFSETS="0,-400000,125000" (Hz)
//   ONAIR_FLEX_AUDIO=<wav>   FM discriminator audio (16-bit WAV, first channel), as SDR programs record it
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_proto.h"
#include "dect2/pager/pager_rx.h"
#include "../common/impair.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void feedAll(PagerReceiver& rx, const V& x, PagerTelemetry& t, uint64_t& seq) {
    auto take = [&] { PagerTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
    V z(65536);
    for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
}

// ---- loopback at every FLEX speed
static void loopback() {
    const double rate = 1.2e6;
    struct Case { const char* name; double snr; std::function<void(V&)> f; };
    const std::vector<Case> cases = {
        {"clean", 30, [](V&) {}},
        {"12 dB", 12, [](V&) {}},
        {"+7 kHz, 20 dB", 20, [&](V& x) { impair::shift(x, 7000, rate); }},
        {"-9 kHz, 20 dB", 20, [&](V& x) { impair::shift(x, -9000, rate); }},
        {"clock -100 ppm, 20 dB", 20, [](V& x) { x = impair::clock(x, -100); }},
        {"+5 kHz, +60 ppm, echo, 22 dB", 22, [&](V& x) { impair::shift(x, 5000, rate); x = impair::clock(x, 60); impair::echo(x, 37, -6, 2.0); }},
    };
    for (int spd = kFlex1600_2; spd <= kFlex6400_4; spd++) {
        const std::vector<PagerTestMessage> want = pagerTestMessages(0, spd + 1);
        int longs = 0;
        for (const PagerTestMessage& w : want) longs += pager::flexLongCap(w.address);
        CHECK(longs >= 1, "%s: the test pages include a long address", pagerSpeedName(spd));
        for (int inv = 0; inv < 2; inv++) {
            for (const Case& cs : cases) {
                SynthConfig c; c.snrDb = cs.snr; c.modeOpt[1] = inv; c.modeOpt[2] = spd + 1; c.modeOpt[3] = 7 + spd;
                auto syn = makePagerSynth(c, rate);
                V x((size_t)(rate * (pagerCycleSeconds(0, spd + 1) * 1.1 + 0.5)));
                for (size_t i = 0; i < x.size(); i += 65536) syn->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
                cs.f(x);
                PagerReceiver rx;
                rx.configure(rate);
                rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
                PagerTelemetry t; uint64_t seq = 0;
                feedAll(rx, x, t, seq);
                int got = 0;
                for (const PagerTestMessage& w : want) {
                    bool f = false;
                    if (t.messages) for (const PagerMessage& m : *t.messages) f |= m.flex && m.speed == spd && m.address == w.address && m.type == w.type && m.text == w.text;
                    got += f;
                }
                int other = 0;
                if (t.messages) for (const PagerMessage& m : *t.messages) other += m.speed != spd;
                printf("%-11s %s %-30s %d of %zu pages\n", pagerSpeedName(spd), inv ? "inverted" : "normal  ", cs.name, got, want.size());
                CHECK(got == (int)want.size(), "%s %s %s: %d of %zu pages", pagerSpeedName(spd), inv ? "inverted" : "normal", cs.name, got, want.size());
                CHECK(other == 0, "%s %s: %d pages of another speed", pagerSpeedName(spd), cs.name, other);
            }
        }
    }
}

// ---- bit-level rules
static std::vector<uint8_t> phaseFromWords(const uint32_t* f) {
    std::vector<uint8_t> bits(2816);
    for (int blk = 0; blk < 11; blk++)
        for (int w = 0; w < 8; w++) {
            const uint32_t W = pager::encodeWord(pager::rev21(f[blk * 8 + w]));
            for (int j = 0; j < 32; j++) bits[(size_t)(blk * 256 + j * 8 + w)] = (uint8_t)((W >> (31 - j)) & 1);
        }
    return bits;
}

static void bitRules() {
    using namespace pager;
    // the check nibble
    for (uint32_t v : {0x000000u, 0x1FFFF0u, 0x0A5A50u, 0x123450u}) {
        const uint32_t c = flexWithCheck(v);
        int s = 0;
        for (int i = 0; i < 20; i += 4) s += (int)((c >> i) & 0xF);
        s += (int)((c >> 20) & 1);
        CHECK((s & 0xF) == 0xF && (c & 0x1FFFF0) == (v & 0x1FFFF0), "check nibble of %06X", v);
    }
    // long address words: the decoder's formula gives the capcode back
    for (uint32_t cap : {2068481u, 2101234u, 2105344u, 2200000u, 3000000u, 2068480u + 0x8000u}) {
        uint32_t w1 = 0, w2 = 0;
        flexLongWords(cap, w1, w2);
        const uint32_t back = (uint32_t)((uint64_t)w1 + ((uint64_t)(w2 ^ 0x1FFFFF) << 15) + 0x1F9000);
        CHECK(w1 >= 1 && w1 <= 0x8000 && back == cap, "long address %u: words %X %X give %u", cap, w1, w2, back);
    }
    // a phase with a short alpha, a long alpha, a long numeric and a short tone page
    std::vector<FlexPage> pages(4);
    pages[0].cap = 1234567; pages[0].type = kPagerAlpha; pages[0].text = "short address text";
    pages[1].cap = 2101234; pages[1].type = kPagerAlpha; pages[1].text = "long address text, more than one word";
    pages[2].cap = 2200000; pages[2].type = kPagerNumeric; pages[2].text = "0123456789 0123";
    pages[3].cap = 77; pages[3].type = kPagerTone;
    std::vector<uint8_t> bits = flexPhaseBits(pages);
    std::vector<PagerMessage> out;
    FlexPhaseStat st;
    flexDecodePhase(bits.data(), out, st);
    CHECK(out.size() == 4 && st.biwOk, "4 pages, got %zu", out.size());
    for (const PagerMessage& m : out) {
        bool ok = false;
        for (const FlexPage& p : pages) ok |= m.address == p.cap && m.type == p.type && m.text == p.text;
        CHECK(ok, "page for %u type %d '%s'", m.address, m.type, m.text.c_str());
    }
    // the same phase word by word, then one vector word with a wrong check: that page goes, the others stay
    uint32_t f[kFlexPhaseWords] = {};
    f[0] = flexWithCheck((4u << 10));                    // addresses 1-3, vectors 4-6
    f[1] = 0x8000 + 1000; f[2] = 0x8000 + 2000; f[3] = 0x8000 + 3000;
    f[4] = flexWithCheck((2u << 4) | (1u << 7));          // tone
    f[5] = flexWithCheck((2u << 4) | (1u << 7)) ^ 0x1;    // tone, check broken
    f[6] = flexWithCheck((2u << 4) | (1u << 7));
    for (int i = 7; i < kFlexPhaseWords; i++) f[i] = (i & 1) ? 0x1FFFFF : 0;   // idle fill as real frames have it
    out.clear(); st = FlexPhaseStat();
    std::vector<uint8_t> b2 = phaseFromWords(f);
    flexDecodePhase(b2.data(), out, st);
    bool a1 = false, a2 = false, a3 = false;
    for (const PagerMessage& m : out) { a1 |= m.address == 1000; a2 |= m.address == 2000; a3 |= m.address == 3000; }
    CHECK(a1 && !a2 && a3 && out.size() == 2, "a vector word with a wrong check: %zu pages", out.size());
    // a BIW with a wrong check: nothing
    f[0] ^= 0x2;
    out.clear(); st = FlexPhaseStat();
    b2 = phaseFromWords(f);
    flexDecodePhase(b2.data(), out, st);
    CHECK(out.empty() && !st.biwOk, "a BIW with a wrong check: %zu pages", out.size());
    // an idle phase (BIW with no addresses, then alternating idle words) gives nothing and no errors
    uint32_t g[kFlexPhaseWords] = {};
    g[0] = flexWithCheck(1u << 10);
    for (int i = 1; i < kFlexPhaseWords; i++) g[i] = (i & 1) ? 0 : 0x1FFFFF;
    out.clear(); st = FlexPhaseStat();
    b2 = phaseFromWords(g);
    flexDecodePhase(b2.data(), out, st);
    CHECK(out.empty() && st.failed == 0, "idle phase: %zu pages, %d failed", out.size(), st.failed);
    printf("bit-level rules checked\n");
}

// ---- recordings (optional; counts only)
struct Wav { int rate = 0, ch = 0; long dataOff = 0; long dataLen = 0; };
static bool readWav(FILE* fp, Wav& w) {
    unsigned char h[12];
    if (fread(h, 1, 12, fp) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, fp) != 8) return false;
        const uint32_t sz = (uint32_t)c[4] | ((uint32_t)c[5] << 8) | ((uint32_t)c[6] << 16) | ((uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char f[16];
            if (sz < 16 || fread(f, 1, 16, fp) != 16) return false;
            w.ch = f[2] | (f[3] << 8);
            w.rate = (int)((uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24));
            const int bitsPer = f[14] | (f[15] << 8);
            if (bitsPer != 16) return false;
            fseek(fp, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            w.dataOff = ftell(fp); w.dataLen = (long)sz;
            return w.rate > 0 && w.ch > 0;
        } else fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR);
    }
}

static int report(const char* what, const PagerTelemetry& t) {
    int frames = 0, pages = 0, byType[5] = {};
    for (int s = kFlex1600_2; s <= kFlex6400_4; s++) {
        const PagerSpeedStat& p = t.speeds[s];
        frames += (int)p.transmissions;
        if (p.transmissions) printf("  %s %-11s: %llu frames, code words %llu good, %llu repaired, %llu lost\n", what, pagerSpeedName(s), (unsigned long long)p.transmissions,
                                    (unsigned long long)p.ok, (unsigned long long)p.fixed, (unsigned long long)p.failed);
    }
    if (t.messages) for (const PagerMessage& m : *t.messages) if (m.flex) { pages++; if (m.type >= 0 && m.type <= 4) byType[m.type]++; }
    printf("  %s: %d FLEX frames, %d pages (tone %d, numeric %d, alpha %d, secure %d, binary %d)\n", what, frames, pages, byType[0], byType[1], byType[2], byType[3], byType[4]);
    return frames;
}

static void recordingIq(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("ONAIR_FLEX_IQ: cannot open the file, skipped\n"); return; }
    Wav w;
    if (!readWav(fp, w) || w.ch != 2) { printf("ONAIR_FLEX_IQ: not a 16-bit stereo WAV, skipped\n"); fclose(fp); return; }
    std::vector<double> offs;
    const char* o = getenv("ONAIR_FLEX_IQ_OFFSETS");
    std::string os = o ? o : "0";
    for (size_t i = 0; i < os.size();) { size_t j = os.find(',', i); if (j == std::string::npos) j = os.size(); offs.push_back(atof(os.substr(i, j - i).c_str())); i = j + 1; }
    std::vector<std::unique_ptr<PagerReceiver>> rx;
    std::vector<PagerTelemetry> tel(offs.size());
    std::vector<uint64_t> seq(offs.size(), 0);
    for (double off : offs) { rx.push_back(std::make_unique<PagerReceiver>()); rx.back()->configure(w.rate); rx.back()->setSignalOffset(off); }
    const size_t chunk = 1 << 18;
    std::vector<int16_t> raw(chunk * 2);
    V x(chunk);
    long left = w.dataLen / 4;
    while (left > 0) {
        const size_t n = fread(raw.data(), 4, std::min<size_t>(chunk, (size_t)left), fp);
        if (n == 0) break;
        left -= (long)n;
        for (size_t i = 0; i < n; i++) x[i] = cf32(raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f);
        for (size_t k = 0; k < rx.size(); k++) {
            rx[k]->feed(x.data(), n);
            PagerTelemetry t; if (rx[k]->telemetry(t, seq[k])) { seq[k] = t.seq; tel[k] = t; }
        }
    }
    fclose(fp);
    int frames = 0;
    for (size_t k = 0; k < rx.size(); k++) {
        V z(65536);
        for (int r = 0; r < 8; r++) { rx[k]->feed(z.data(), z.size()); PagerTelemetry t; if (rx[k]->telemetry(t, seq[k])) { seq[k] = t.seq; tel[k] = t; } }
        char nm[64]; snprintf(nm, sizeof nm, "IQ channel %+.0f Hz", offs[k]);
        frames += report(nm, tel[k]);
    }
    CHECK(frames > 0, "the IQ recording gave no FLEX frame");
}

static void recordingAudio(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("ONAIR_FLEX_AUDIO: cannot open the file, skipped\n"); return; }
    Wav w;
    if (!readWav(fp, w)) { printf("ONAIR_FLEX_AUDIO: not a 16-bit WAV, skipped\n"); fclose(fp); return; }
    std::vector<int16_t> raw((size_t)(w.dataLen / 2));
    raw.resize(fread(raw.data(), 2, raw.size(), fp));
    fclose(fp);
    std::vector<float> a;
    for (size_t i = 0; i + (size_t)w.ch <= raw.size(); i += (size_t)w.ch) a.push_back((float)raw[i]);
    // discriminator audio back to FM: 90 % of the samples are within the full deviation (4.8 kHz), 4 times the audio rate by linear interpolation
    std::vector<float> s(a);
    for (auto& v : s) v = std::fabs(v);
    std::nth_element(s.begin(), s.begin() + (ptrdiff_t)(s.size() * 9 / 10), s.end());
    const double scale = 4800.0 / std::max(1.0f, s[s.size() * 9 / 10]);
    const int up = 4;
    const double rate = (double)w.rate * up;
    int frames = 0;
    for (int pol = 0; pol < 2; pol++) {
        V x;
        x.reserve(a.size() * up);
        double ph = 0;
        for (size_t i = 0; i + 1 < a.size(); i++)
            for (int k = 0; k < up; k++) {
                const double f = (a[i] + (a[i + 1] - a[i]) * k / up) * scale * (pol ? -1 : 1);
                ph += 2 * 3.14159265358979323846 * f / rate;
                x.push_back(cf32((float)std::cos(ph) * 0.5f, (float)std::sin(ph) * 0.5f));
            }
        PagerReceiver rx;
        rx.configure(rate);
        PagerTelemetry t; uint64_t seq = 0;
        feedAll(rx, x, t, seq);
        frames += report(pol ? "audio (inverted)" : "audio", t);
    }
    CHECK(frames > 0, "the audio recording gave no FLEX frame");
}

int main() {
    bitRules();
    loopback();
    const char* iq = getenv("ONAIR_FLEX_IQ");
    const char* au = getenv("ONAIR_FLEX_AUDIO");
    if (iq && *iq) recordingIq(iq); else printf("no ONAIR_FLEX_IQ recording given: that part is skipped\n");
    if (au && *au) recordingAudio(au); else printf("no ONAIR_FLEX_AUDIO recording given: that part is skipped\n");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
