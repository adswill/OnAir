// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / packet TNC benchmark: a signal model written here, independent of packet_gen.cpp, run through the real receiver.
// 1200 baud Bell 202 AFSK and 9600 baud G3RUH FSK on an FM carrier with noise, a carrier offset, a transmitter baud clock that is off, 2.5 to 5 kHz
// deviation, transmit pre-emphasis on or off (a radio's microphone input or its flat data port), frame collisions, and the narrow (12.5 kHz) and wide
// (25 kHz) channel filter. Prints the decode percentage against the carrier-to-noise ratio (in 25 kHz) and checks thresholds in the range of a good
// hardware or software TNC (about 90% and more once the FM demodulator is above its threshold, near 100% at 20 dB).
// --sweep prints the whole table (slow); without it a reduced set with the checks runs.
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_rx.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

namespace {

constexpr double kFs = 192000.0;                  // the whole-span bank's carrier rate (setWideInput)
constexpr double kPi = 3.14159265358979323846;
int fails = 0;

struct Case {
    const char* name = "";
    int baud = 1200;
    double cnrDb = 30;           // carrier to noise in 25 kHz
    double offHz = 0;            // carrier offset
    double ppm = 0;              // transmitter baud clock error
    double devHz = 3500;         // peak deviation
    bool preEmph = false;        // transmit pre-emphasis (6 dB per octave on the audio)
    double twistDb = 0;          // mark tone level against space (dB) on top of that: a radio's audio response
    bool txFilter = false;       // the transmitter's speech band-pass, 300 Hz to 2.7 kHz (two second-order sections each)
    bool clip = false;           // the audio driven into the transmitter's limiter (clipped at 70% of the peak)
    double collide = 0;          // share of frames that another, weaker station partly overlaps
    bool narrow = false;
    int frames = 40;
    int fixBits = 0;
    bool vote = true;            // the receiver's multi-demodulator vote (ax25::voteRepair)
    int emphasis = 0;            // the receiver's setting
    bool audio = false;          // sound-card input: the FM-demodulated audio (cnrDb is then the audio SNR in 4 kHz)
    bool noiseOnly = false;      // no transmissions at all (false frames)
    int seed = 1;
};

struct Tx { double start; std::vector<uint8_t> levels; double amp; double off; };

ax25::Frame makeFrame(int k, std::mt19937& rng) {
    ax25::Frame f;
    char call[8]; snprintf(call, sizeof call, "N%dTST", k % 10);
    f.from.call = call; f.from.ssid = k % 16;
    f.to.call = "APRS";
    ax25::Address d; d.call = "WIDE1"; d.ssid = 1; f.path.push_back(d);
    char b[64]; snprintf(b, sizeof b, ">TNC benchmark frame %05d ", k);
    f.info = b;
    const int extra = (int)(rng() % 90);
    for (int i = 0; i < extra; i++) f.info.push_back((char)('A' + (rng() % 26)));
    return f;
}

// AFSK audio (unit amplitude) at kFs: phase-continuous tones, the bit clock 1200 * (1 + ppm); with preEmph a first-order pre-emphasis
// (corner 300 Hz) as a radio's microphone input applies, normalised so the peak deviation is the same
// RBJ second-order section, run in place
void biquad(std::vector<float>& a, double f0, bool high) {
    const double w = 2 * kPi * f0 / kFs, al = std::sin(w) / (2 * 0.7071), cw = std::cos(w);
    double b0 = high ? (1 + cw) / 2 : (1 - cw) / 2, b1 = high ? -(1 + cw) : 1 - cw, b2 = b0;
    const double a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (auto& v : a) {
        const double y = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1; x1 = v; y2 = y1; y1 = y;
        v = (float)y;
    }
}

std::vector<float> afskAudio(const std::vector<uint8_t>& lv, const struct Case& c) {
    const double spb = kFs / (1200.0 * (1 + c.ppm * 1e-6));
    const bool pre = c.preEmph;
    const float markAmp = (float)std::pow(10.0, c.twistDb / 20);
    std::vector<float> a;
    a.reserve((size_t)(lv.size() * spb) + 4);
    double ph = 0, t = 0;
    for (size_t i = 0; i < lv.size(); i++) {
        const double f = lv[i] ? 1200.0 : 2200.0;
        for (t += spb; t >= 1.0; t -= 1.0) { ph += 2 * kPi * f / kFs; a.push_back((float)std::sin(ph) * (lv[i] ? markAmp : 1.f)); }
    }
    if (c.txFilter) { biquad(a, 300, true); biquad(a, 300, true); biquad(a, 2700, false); biquad(a, 2700, false); }
    if (pre) {
        // H(s) = (1 + s / w1) / (1 + s / w2), corners 300 Hz and 8 kHz (bilinear), scaled so 2200 Hz keeps unit amplitude: mark comes out
        // about 5 dB under space, as from a radio's microphone input
        const double k1 = kFs / (kPi * 300.0), k2 = kFs / (kPi * 8000.0);
        const double b0 = 1 + k1, b1 = 1 - k1, a0 = 1 + k2, a1 = 1 - k2;
        const double w = 2 * kPi * 2200.0 / kFs;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const double g = std::abs((a0 + a1 * z1) / (b0 + b1 * z1));
        double x1 = 0, y1 = 0;
        for (auto& v : a) {
            const double y = (b0 * v + b1 * x1 - a1 * y1) / a0;
            x1 = v; y1 = y;
            v = (float)(y * g);
        }
    }
    float pk = 1e-6f;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    if (c.clip) { for (auto& v : a) v = std::max(-0.7f * pk, std::min(0.7f * pk, v)); pk *= 0.7f; }   // the limiter after the pre-emphasis, as in a radio
    for (auto& v : a) v /= pk;   // the peak deviation stays devHz
    return a;
}

// G3RUH 9600: scrambled NRZI levels, raised-cosine shaped (alpha 0.5) at kFs
std::vector<float> fskAudio(const std::vector<uint8_t>& lv, double ppm) {
    const double sps = kFs / (9600.0 * (1 + ppm * 1e-6));
    const size_t n = (size_t)(lv.size() * sps) + 1;
    std::vector<float> a(n, 0.f);
    const int span = (int)(4 * sps);
    for (size_t i = 0; i < lv.size(); i++) {
        const double c = (double)i * sps;
        const float s = lv[i] ? 1.f : -1.f;
        for (int k = -span; k <= span; k++) {
            const int64_t j = (int64_t)std::llround(c) + k;
            if (j < 0 || j >= (int64_t)n) continue;
            const double x = ((double)j - c) / sps;
            double h;
            const double al = 0.5;
            if (std::fabs(x) < 1e-9) h = 1;
            else if (std::fabs(std::fabs(2 * al * x) - 1) < 1e-9) h = kPi / 4 * std::sin(kPi * x) / (kPi * x);
            else h = std::sin(kPi * x) / (kPi * x) * std::cos(kPi * al * x) / (1 - 4 * al * al * x * x);
            a[(size_t)j] += (float)(s * h);
        }
    }
    return a;
}

struct Result { int sent = 0, got = 0, falseFrames = 0, fixed = 0, voted = 0; double twist = 0; };

Result run(const Case& c) {
    std::mt19937 rng((unsigned)c.seed * 7919u + (unsigned)c.baud);
    std::vector<Tx> txs;
    std::set<std::string> want;
    double t = 0.8;                       // the receiver learns its noise floor first
    const double bitSec = 1.0 / c.baud;
    uint32_t scr = 0;
    for (int k = 0; k < c.frames; k++) {
        if (c.noiseOnly) { t += 1.0; continue; }
        const ax25::Frame f = makeFrame(k, rng);
        want.insert(f.info);
        const int txd = c.baud == 1200 ? 25 : 60, tail = 3;   // TXDELAY: 25 flags (167 ms) at 1200, 60 (50 ms) at 9600
        std::vector<uint8_t> bits = ax25::hdlcBits(ax25::buildFrame(f), txd, tail);
        std::vector<uint8_t> lv = ax25::nrziEncode(bits);
        if (c.baud == 9600) lv = ax25::scramble(lv, scr);
        Tx x{t, lv, 1.0, c.offHz};
        txs.push_back(x);
        const double dur = (double)lv.size() * bitSec;
        if (c.collide > 0 && (double)(rng() % 1000) < c.collide * 1000) {
            // another station keys up over the second half of this frame, 10 dB weaker (FM capture keeps the stronger one)
            const ax25::Frame g = makeFrame(10000 + k, rng);
            std::vector<uint8_t> b2 = ax25::nrziEncode(ax25::hdlcBits(ax25::buildFrame(g), txd, tail));
            if (c.baud == 9600) b2 = ax25::scramble(b2, scr);
            txs.push_back(Tx{t + dur * 0.5, b2, std::pow(10.0, -10 / 20.0), c.offHz + 700});
        }
        t += dur + 0.15 + (double)(rng() % 100) * 0.002;
    }
    const size_t total = (size_t)((t + 1.0) * kFs);
    std::vector<cf32> x(total, cf32(0, 0));
    for (const Tx& tx : txs) {
        const std::vector<float> a = c.baud == 1200 ? afskAudio(tx.levels, c) : fskAudio(tx.levels, c.ppm);
        const size_t s0 = (size_t)(tx.start * kFs);
        double th = (double)(rng() % 1000) * 0.006;
        if (c.audio) {   // the radio's audio output: the tones at half of full scale
            for (size_t i = 0; i < a.size() && s0 + i < total; i++) x[s0 + i] += cf32((float)(0.5 * tx.amp * a[i]), 0.f);
            continue;
        }
        for (size_t i = 0; i < a.size() && s0 + i < total; i++) {
            th += 2 * kPi * (tx.off + c.devHz * a[i]) / kFs;
            x[s0 + i] += cf32((float)(tx.amp * std::cos(th)), (float)(tx.amp * std::sin(th)));
        }
    }
    // noise: carrier power 1, noise power in 25 kHz 10^(-cnr/10), spread over kFs
    if (c.noiseOnly) {   // a carrier with a warbling tone (a voice user, say), 3 s overs: noise and clicks for the slicers, no frames
        double th = 0;
        for (size_t i = 0; i < total; i++) {
            const double tt = (double)i / kFs;
            th += 2 * kPi * 3000.0 * std::sin(2 * kPi * (900.0 + 400.0 * std::sin(2 * kPi * 0.7 * tt)) * tt) / kFs;
            if (std::fmod(tt, 4.0) > 1.0) x[i] = cf32((float)std::cos(th), (float)std::sin(th));   // 3 s on, 1 s off
        }
    }
    if (c.audio) {
        // audio: tone power 0.125 against noise in 4 kHz, the noise band-limited to 5 kHz as from a radio
        std::vector<float> nz(total);
        std::normal_distribution<float> na(0.f, (float)std::sqrt(0.125 * std::pow(10.0, -c.cnrDb / 10) * kFs / 2 / 4000.0));
        for (auto& v : nz) v = na(rng);
        biquad(nz, 5000, false); biquad(nz, 5000, false);
        for (size_t i = 0; i < total; i++) x[i] += cf32(nz[i], 0.f);
    } else {
        const double sigma = std::sqrt(std::pow(10.0, -c.cnrDb / 10) * kFs / 25000.0 / 2);
        std::normal_distribution<float> nd(0.f, (float)sigma);
        for (auto& v : x) v += cf32(nd(rng), nd(rng));
    }

    PacketReceiver rx;
    rx.setWideInput(true);
    rx.setNarrowChannel(c.narrow);
    rx.setSilent(true);
    rx.setFixBits(c.fixBits);
    rx.setVote(c.vote);
    rx.setEmphasis(c.emphasis);
    rx.setAudioInput(c.audio);
    rx.configure(kFs);
    std::set<std::string> got;
    Result r;
    r.sent = c.frames;
    PacketTelemetry tel; uint64_t seq = 0;
    double lastSeen = -1;
    const size_t blk = 9600;
    for (size_t i = 0; i < total; i += blk) {
        rx.feed(x.data() + i, std::min(blk, total - i));
        if (rx.telemetry(tel, seq)) {
            seq = tel.seq;
            for (const PacketFrameInfo& f : tel.frames) {
                if (f.timeSec <= lastSeen) continue;
                lastSeen = f.timeSec;
                if (want.count(f.info)) got.insert(f.info);
                else if (f.info.rfind(">TNC benchmark frame 1", 0) != 0) {   // the colliding station's frames count as neither
                    r.falseFrames++;
                    printf("  unexpected frame %s>%s: %s\n", f.from.c_str(), f.to.c_str(), f.summary.c_str());
                }   // the colliding station's frames count as neither
            }
        }
    }
    r.got = (int)got.size();
    r.fixed = (int)tel.fixedFrames;
    r.voted = (int)tel.votedFrames;
    r.twist = tel.twistDb;
    return r;
}

double pct(const Result& r) { return 100.0 * r.got / std::max(1, r.sent); }

void check(Case c, double minPct) {
    const Result r = run(c);
    const double p = pct(r);
    printf("%-58s %5.1f dB  %3d/%3d  %5.1f%%  (need %.0f%%)  twist %+.1f dB, %d fixed%s\n", c.name, c.cnrDb, r.got, r.sent, p, minPct, r.twist, r.fixed,
           r.falseFrames ? "  FALSE FRAMES" : "");
    if (p + 1e-9 < minPct || r.falseFrames) { printf("FAIL: %s at %.1f dB\n", c.name, c.cnrDb); fails++; }
}

// vote: the demodulators' failed copies of one frame voted into a good one (weak signals, 1200 baud); never a wrong frame
void voteBlock(const std::vector<double>& cnrs, int seeds, double minGain = 1.0) {
    int off = 0, on = 0, voted = 0, sent = 0, wrong = 0;
    for (double cnr : cnrs) {
        for (int seed = 1; seed <= seeds; seed++) {
            Case c; c.cnrDb = cnr; c.frames = 80; c.seed = seed;
            c.vote = false; const Result r0 = run(c);
            c.vote = true; const Result r1 = run(c);
            printf("  1200 vote at %.0f dB seed %d: off %d, on %d (%d voted) of %d\n", cnr, seed, r0.got, r1.got, r1.voted, r0.sent);
            off += r0.got; on += r1.got; voted += r1.voted; sent += r0.sent; wrong += r1.falseFrames;
        }
    }
    printf("%-58s off %d, on %d (%d voted) of %d, wrong frames %d\n", "1200 vote, total", off, on, voted, sent, wrong);
    if (on < off * minGain || wrong) { printf("FAIL: vote (need %.0f%% more frames than without)\n", (minGain - 1) * 100); fails++; }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--vote")) { voteBlock({1.0, 2.0, 3.0, 4.0, 5.0}, argc > 2 ? atoi(argv[2]) : 3); return fails ? 1 : 0; }
    const bool sweep = argc > 1 && !strcmp(argv[1], "--sweep");
    if (sweep) {
        printf("decode %% vs CNR (25 kHz)\n");
        const double cnrs[] = {6, 8, 10, 12, 14, 16, 20};
        struct Row { const char* name; Case c; };
        std::vector<Row> rows;
        auto add = [&](const char* n, Case c) { c.name = n; rows.push_back({n, c}); };
        Case b; b.frames = 60;
        add("1200 flat 3.5k", b);
        { Case c = b; c.preEmph = true; add("1200 pre-emphasis 3.5k", c); }
        { Case c = b; c.devHz = 2500; add("1200 flat 2.5k", c); }
        { Case c = b; c.devHz = 5000; add("1200 flat 5k", c); }
        { Case c = b; c.offHz = 1500; add("1200 flat +1.5 kHz", c); }
        { Case c = b; c.offHz = -1500; c.preEmph = true; add("1200 pre-emph -1.5 kHz", c); }
        { Case c = b; c.ppm = 500; add("1200 flat +500 ppm", c); }
        { Case c = b; c.ppm = -500; c.preEmph = true; add("1200 pre-emph -500 ppm", c); }
        { Case c = b; c.narrow = true; add("1200 flat narrow", c); }
        { Case c = b; c.narrow = true; c.offHz = 1500; c.devHz = 5000; add("1200 narrow +1.5k 5k dev", c); }
        { Case c = b; c.narrow = true; c.offHz = -3000; add("1200 narrow -3 kHz", c); }
        { Case c = b; c.narrow = true; c.offHz = 5000; add("1200 narrow +5 kHz", c); }
        { Case c = b; c.offHz = 7000; add("1200 wide +7 kHz", c); }
        { Case c = b; c.twistDb = -9; add("1200 twist -9 dB", c); }
        { Case c = b; c.twistDb = 6; add("1200 twist +6 dB", c); }
        { Case c = b; c.preEmph = true; c.twistDb = -4; c.txFilter = true; add("1200 pre-emph, -4 dB, band-pass", c); }
        { Case c = b; c.preEmph = true; c.clip = true; c.devHz = 4500; add("1200 pre-emph, limiter", c); }
        { Case c = b; c.txFilter = true; c.clip = true; add("1200 band-pass, limiter", c); }
        { Case c = b; c.collide = 0.05; add("1200 5% collisions", c); }
        { Case c = b; c.fixBits = 1; add("1200 flat fix 1 bit", c); }
        { Case c = b; c.baud = 9600; c.devHz = 3000; add("9600 3k", c); }
        { Case c = b; c.baud = 9600; c.devHz = 2500; add("9600 2.5k", c); }
        { Case c = b; c.baud = 9600; c.devHz = 4000; c.narrow = true; add("9600 4k narrow", c); }
        { Case c = b; c.baud = 9600; c.devHz = 3000; c.offHz = 1500; c.ppm = 200; add("9600 3k +1.5k +200ppm", c); }
        printf("%-32s", "case");
        for (double s : cnrs) printf(" %5.0f", s);
        printf("\n");
        for (auto& row : rows) {
            if (argc > 2 && !strstr(row.name, argv[2])) continue;
            printf("%-32s", row.name); fflush(stdout);
            for (double s : cnrs) { Case c = row.c; c.cnrDb = s; printf(" %5.1f", pct(run(c))); fflush(stdout); }
            printf("\n");
        }
        return 0;
    }
    // checks (thresholds a little under the measured figures: docs/PACKET.md has the table)
    Case b;
    { Case c = b; c.name = "1200 flat, 3.5 kHz"; c.cnrDb = 20; check(c, 100); }
    { Case c = b; c.name = "1200 flat, 3.5 kHz"; c.cnrDb = 12; check(c, 90); }
    { Case c = b; c.name = "1200 pre-emphasis, 3.5 kHz"; c.preEmph = true; c.cnrDb = 12; check(c, 90); }
    { Case c = b; c.name = "1200 2.5 kHz deviation"; c.devHz = 2500; c.cnrDb = 14; check(c, 90); }
    { Case c = b; c.name = "1200 5 kHz deviation, pre-emphasis"; c.devHz = 5000; c.preEmph = true; c.cnrDb = 12; check(c, 90); }
    { Case c = b; c.name = "1200 +1.5 kHz off, +500 ppm"; c.offHz = 1500; c.ppm = 500; c.cnrDb = 14; check(c, 90); }
    { Case c = b; c.name = "1200 -1.5 kHz off, -500 ppm, pre-emphasis"; c.offHz = -1500; c.ppm = -500; c.preEmph = true; c.cnrDb = 14; check(c, 90); }
    { Case c = b; c.name = "1200 -3 kHz off (wide channel)"; c.offHz = -3000; c.cnrDb = 16; check(c, 95); }
    { Case c = b; c.name = "1200 12.5 kHz channel, +1.5 kHz off, 5 kHz dev"; c.narrow = true; c.offHz = 1500; c.devHz = 5000; c.cnrDb = 14; check(c, 90); }
    { Case c = b; c.name = "1200 12.5 kHz channel, 2.5 kHz dev, pre-emphasis"; c.narrow = true; c.devHz = 2500; c.preEmph = true; c.cnrDb = 14; check(c, 90); }
    { Case c = b; c.name = "1200 5% collisions"; c.collide = 0.05; c.cnrDb = 20; check(c, 92); }
    {   // fix bits: at the edge of the FM threshold more frames, never a wrong one
        Case c = b; c.cnrDb = 7; c.frames = 60;
        c.name = "1200 weak, fix bits off"; const Result r0 = run(c);
        c.fixBits = 1; c.name = "1200 weak, fix bits 1"; const Result r1 = run(c);
        c.fixBits = 2; c.name = "1200 weak, fix bits 2"; const Result r2 = run(c);
        printf("%-58s %5.1f dB  off %d, 1 bit %d (%d fixed), 2 bits %d (%d fixed) of %d\n", "1200 fix bits", c.cnrDb, r0.got, r1.got, r1.fixed, r2.got, r2.fixed, r0.sent);
        if (r1.got < r0.got || r2.got < r1.got || r1.falseFrames || r2.falseFrames) { printf("FAIL: fix bits\n"); fails++; }
    }
    voteBlock({4.0, 5.0}, 2, 1.05);
    { Case c = b; c.name = "1200 tones 9 dB apart (mark low)"; c.twistDb = -9; c.cnrDb = 10; check(c, 95); }
    { Case c = b; c.name = "1200 tones 6 dB apart (mark high)"; c.twistDb = 6; c.cnrDb = 10; check(c, 95); }
    { Case c = b; c.name = "1200 pre-emphasis, band-pass, limiter"; c.preEmph = true; c.txFilter = true; c.clip = true; c.cnrDb = 12; check(c, 95); }
    { Case c = b; c.name = "1200 pre-emphasis, receiver de-emphasis on"; c.preEmph = true; c.emphasis = 1; c.cnrDb = 12; check(c, 95); }
    { Case c = b; c.name = "1200 sound card, speaker audio (+5 dB mark)"; c.audio = true; c.twistDb = 5; c.cnrDb = 12; check(c, 95); }
    { Case c = b; c.name = "1200 sound card, discriminator, pre-emphasis"; c.audio = true; c.preEmph = true; c.cnrDb = 12; check(c, 95); }
    { Case c = b; c.name = "9600 sound card, discriminator"; c.audio = true; c.baud = 9600; c.cnrDb = 20; check(c, 95); }
    { Case c = b; c.name = "voice carrier at 6 dB, fix bits 2: no frames"; c.noiseOnly = true; c.fixBits = 2; c.frames = 60; c.cnrDb = 6; check(c, 0); }
    { Case c = b; c.name = "9600 G3RUH 3 kHz"; c.baud = 9600; c.devHz = 3000; c.cnrDb = 20; check(c, 100); }
    { Case c = b; c.name = "9600 G3RUH 3 kHz, +1.5 kHz off, +200 ppm"; c.baud = 9600; c.devHz = 3000; c.offHz = 1500; c.ppm = 200; c.cnrDb = 20; check(c, 95); }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
