// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat Aero C channel (voice): coding layer, framer and the demodulator with real-world impairments. Framing and coding follow JAERO
// (aero_cchan.h) and were checked on a real 8400 bit/s recording from JAERO's sample set (test_aero_cchan_real); everything here uses synthetic
// signals made from that framing (aero_cgen.h). The voice content is random bytes: no vocoder is involved.
#include "dect2/aero/aero_crx.h"
#include "dect2/aero/aero_cgen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

namespace {
std::string key(const uint8_t* p) { return std::string((const char*)p, 12); }

struct RunResult {
    int frames = 0, exact = 0, withCall = 0, wrongVoice = 0;
    uint64_t falseSync = 0;
    double carrier = 0, ber = 0;
    bool acquired = false;
    double firstFrameSec = -1;
};

// generator -> receiver; every received frame is compared with what was sent
RunResult run(AeroCGenConfig g, double secs, size_t chunk = 4800, double rxOffsetErr = 0, double rxSearch = 2800, double fsRx = 0) {
    AeroCGenerator gen(g);
    AeroCReceiver rx;
    rx.configure(g.fs, g.carrierHz + rxOffsetErr, rxSearch);
    RunResult r;
    std::vector<AeroCFrameEvent> evs;
    rx.setFrameCallback([&](const AeroCFrameEvent& e) { evs.push_back(e); if (r.firstFrameSec < 0) r.firstFrameSec = (double)rx.samplesFed() / g.fs; });
    std::vector<cf32> buf(chunk);
    for (size_t n = 0; n < (size_t)(g.fs * secs); n += chunk) {
        const size_t k = std::min(chunk, (size_t)(g.fs * secs) - n);
        gen.generate(buf.data(), k);
        rx.feed(buf.data(), k);
    }
    std::map<std::string, size_t> idx;
    const auto& sent = gen.sentVoice();
    for (size_t i = 0; i < sent.size(); i++) idx[key(sent[i].data())] = i;
    for (const auto& e : evs) {
        r.frames++;
        r.ber += e.channelBer;
        r.withCall += e.haveCall && e.aesId == g.aesId && e.gesId == g.gesId;
        auto it = idx.find(key(e.voice.data()));
        bool ok = it != idx.end() && it->second % 25 == 0;
        for (int i = 0; ok && i < 25; i++) ok = it->second + (size_t)i < sent.size() && std::memcmp(e.voice.data() + 12 * i, sent[it->second + (size_t)i].data(), 12) == 0;
        r.exact += ok;
        r.wrongVoice += !ok;
    }
    if (r.frames) r.ber /= r.frames;
    r.carrier = rx.carrierHz();
    r.acquired = rx.acquired();
    return r;
}

// the number of frames a run of `secs` seconds should deliver once locked (500 ms each), minus the time to lock
int expectFrames(double secs, double lockSec = 5.0) { return (int)((secs - lockSec) / 0.5); }
}

int main() {
    // ---------- constants ----------
    {
        const auto uw = aeroCUniqueWordBits();
        CHECK(uw.size() == 104, "unique word length %zu", uw.size());
        int ones = 0; for (auto b : uw) ones += b;
        CHECK(ones > 30 && ones < 74, "unique word balance %d", ones);
        AeroCFrameEncoder enc;
        uint8_t v[300] = {}, su[36] = {};
        CHECK(enc.frame(v, su).size() == 4200, "frame length");
    }
    // ---------- coding round trip and error correction ----------
    {
        std::mt19937 rng(5);
        AeroCFrameEncoder enc;
        AeroCFrameDecoder dec;
        for (int t = 0; t < 4; t++) {
            uint8_t v[300], su[36];
            for (auto& b : v) b = (uint8_t)rng();
            std::memset(su, 0, 36);
            for (int j = 0; j < 3; j++) { su[12 * j] = j == 2 ? 0x30 : 0x01; su[12 * j + 4] = 0x90; aeroSuSetCrc(su + 12 * j); }
            const auto bits = enc.frame(v, su);
            std::vector<float> soft(4096);
            for (int i = 0; i < 4096; i++) soft[i] = bits[104 + i] ? 1.f : -1.f;
            AeroCDecoded d;
            dec.decode(soft.data(), d);
            CHECK(std::memcmp(d.voice.data(), v, 300) == 0, "clean voice mismatch");
            CHECK(std::memcmp(d.su.data(), su, 36) == 0 && d.suOk[0] && d.suOk[1] && d.suOk[2], "clean SU mismatch");
            CHECK(d.channelBer == 0.f, "clean ber %f", d.channelBer);
            // 0.5 percent of the channel bits flipped (hard decisions): the code still corrects them
            std::vector<float> bad = soft;
            for (int i = 0; i < 20; i++) { size_t p = rng() % 4096; bad[p] = -bad[p]; }
            dec.decode(bad.data(), d);
            CHECK(std::memcmp(d.voice.data(), v, 300) == 0 && d.suOk[0] && d.suOk[1] && d.suOk[2], "0.5 percent flips not corrected");
        }
    }
    // ---------- decoder sensitivity: ideal soft bits in AWGN against theory ----------
    // Rate: 2714 information bits in 4096 channel bits. Eb/N0 here is per CHANNEL bit; per information bit it is 1.79 dB higher.
    // A rate 2/3, K = 7 soft code needs about 4.4 dB per information bit for 1e-5 (a frame of 2714 bits then fails about 3 times in 100), i.e. about
    // 2.6 dB per channel bit: that is the theory column.
    {
        std::mt19937 rng(11);
        std::normal_distribution<float> nd(0.f, 1.f);
        AeroCFrameEncoder enc;
        AeroCFrameDecoder dec;
        printf("decoder only, ideal soft bits (theory: 100%% from about 2.6 dB per channel bit)\n  Eb/N0  frames-ok\n");
        double okAt[8] = {};
        const double dbs[8] = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0};
        for (int k = 0; k < 8; k++) {
            const double eb = std::pow(10.0, dbs[k] / 10);
            const float sigma = (float)std::sqrt(1.0 / (2.0 * eb));
            int ok = 0; const int N = 60;
            for (int t = 0; t < N; t++) {
                uint8_t v[300], su[36];
                for (auto& b : v) b = (uint8_t)rng();
                std::memset(su, 0, 36);
                for (int j = 0; j < 3; j++) { su[12 * j] = 0x01; su[12 * j + 4] = 0x90; aeroSuSetCrc(su + 12 * j); }
                const auto bits = enc.frame(v, su);
                std::vector<float> soft(4096);
                for (int i = 0; i < 4096; i++) soft[i] = (bits[104 + i] ? 1.f : -1.f) + sigma * nd(rng);
                AeroCDecoded d;
                dec.decode(soft.data(), d);
                ok += std::memcmp(d.voice.data(), v, 300) == 0;
            }
            okAt[k] = (double)ok / N;
            printf("  %4.1f   %5.1f%%\n", dbs[k], 100.0 * okAt[k]);
        }
        CHECK(okAt[4] >= 0.95, "3.5 dB per channel bit: %.0f%% frames", 100 * okAt[4]);
        CHECK(okAt[6] >= 0.999, "5 dB: %.0f%%", 100 * okAt[6]);
    }
    // ---------- framer on a bit stream: polarity, arm swap, start, errors, gaps, false syncs ----------
    {
        std::mt19937 rng(21);
        AeroCFrameEncoder enc;
        std::vector<uint8_t> all;
        std::vector<std::vector<uint8_t>> voices;
        for (int f = 0; f < 12; f++) {
            uint8_t v[300], su[36];
            for (auto& b : v) b = (uint8_t)rng();
            std::memset(su, 0, 36);
            for (int j = 0; j < 3; j++) { su[12 * j] = 0x01; su[12 * j + 4] = 0x90; aeroSuSetCrc(su + 12 * j); }
            voices.emplace_back(v, v + 300);
            const auto b = enc.frame(v, su);
            all.insert(all.end(), b.begin(), b.end());
        }
        auto runFramer = [&](const std::vector<float>& soft, int* frames, int* exact, uint64_t* falseSync) {
            AeroCFramer fr; int n = 0, ex = 0;
            fr.setCallback([&](AeroCFrameEvent& e) { n++; for (auto& v : voices) if (std::memcmp(e.voice.data(), v.data(), 300) == 0) { ex++; break; } });
            for (float s : soft) fr.push(s);
            *frames = n; *exact = ex; if (falseSync) *falseSync = fr.falseSyncs();
        };
        // arm polarity (4 combinations) and a one-bit shift (the arms swapped): all frames after the first word come out right
        for (int inv = 0; inv < 4; inv++) for (int shift = 0; shift < 2; shift++) {
            std::vector<float> soft;
            if (shift) soft.push_back(0.3f);
            for (size_t i = 0; i < all.size(); i++) { float s = all[i] ? 1.f : -1.f; if ((inv >> (i & 1)) & 1) s = -s; soft.push_back(s); }
            int n, ex; runFramer(soft, &n, &ex, nullptr);
            CHECK(n >= 11 && ex == n, "inv %d shift %d: %d frames, %d exact", inv, shift, n, ex);
        }
        // starting in the middle of a frame, with random bits in front
        {
            std::vector<float> soft;
            for (int i = 0; i < 1777; i++) soft.push_back((rng() & 1) ? 0.7f : -0.7f);
            for (size_t i = 2000; i < all.size(); i++) soft.push_back(all[i] ? 1.f : -1.f);
            int n, ex; runFramer(soft, &n, &ex, nullptr);
            CHECK(n >= 10 && ex == n, "mid-frame start: %d frames, %d exact", n, ex);
        }
        // 10 percent of the unique word bits wrong (hard) in every frame: the flywheel still holds
        {
            std::vector<float> soft;
            for (size_t i = 0; i < all.size(); i++) {
                float s = all[i] ? 1.f : -1.f;
                if (i % 4200 < 104 && i >= 4200 && (rng() % 100) < 10) s = -s;
                soft.push_back(s);
            }
            int n, ex; runFramer(soft, &n, &ex, nullptr);
            CHECK(n >= 11 && ex == n, "damaged unique words: %d frames, %d exact", n, ex);
        }
        // a lost / extra bit mid-stream (clock slip): the framer finds the word again
        for (int slip = -1; slip <= 1; slip += 2) {
            std::vector<float> soft;
            for (size_t i = 0; i < all.size(); i++) {
                if ((int)i == 3 * 4200 + 2000) { if (slip < 0) continue; soft.push_back(0.4f); }
                soft.push_back(all[i] ? 1.f : -1.f);
            }
            int n, ex; runFramer(soft, &n, &ex, nullptr);
            CHECK(n >= 10, "slip %d: %d frames", slip, n);
            CHECK(ex >= n - 1, "slip %d: %d exact of %d", slip, ex, n);
        }
        // noise only: no frame, ever
        {
            std::vector<float> soft;
            std::normal_distribution<float> nd(0.f, 1.f);
            for (int i = 0; i < 3000000; i++) soft.push_back(nd(rng));
            int n, ex; uint64_t fs; runFramer(soft, &n, &ex, &fs);
            CHECK(n == 0, "frames from noise: %d (false syncs %llu)", n, (unsigned long long)fs);
        }
    }
    // ---------- the whole chain: generator -> OQPSK demodulator -> framer ----------
    {
        printf("chain at 48 kHz, 40 s, Eb/N0 per channel bit: frames exact / expected, wrong, channel BER\n");
        const double dbs[] = {2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 8.0, 14.0};
        for (double db : dbs) {
            AeroCGenConfig g; g.fs = 48000; g.ebn0Db = db; g.carrierHz = 600;
            const RunResult r = run(g, 40);
            printf("  %5.1f dB  %2d/%2d  wrong %d  ber %.3f  call %d  carrier %.0f\n", db, r.exact, expectFrames(40), r.wrongVoice, r.ber, r.withCall, r.carrier);
            if (db >= 5) CHECK(r.exact >= expectFrames(40) - 1 && r.wrongVoice == 0, "%.1f dB: %d exact, %d wrong", db, r.exact, r.wrongVoice);
            if (db >= 3.5) CHECK(r.exact >= expectFrames(40) * 8 / 10, "%.1f dB: only %d exact", db, r.exact);
            if (db >= 8) CHECK(r.withCall >= 5, "call-progress units seen: %d", r.withCall);
        }
        // carrier offsets across the search range (the receiver is told 0 Hz and searches +-2800)
        for (double off : {-2600.0, -1500.0, -700.0, 0.0, 400.0, 1500.0, 2600.0}) {
            AeroCGenConfig g; g.fs = 48000; g.ebn0Db = 8; g.carrierHz = off;
            const RunResult r = run(g, 30, 4800, -off);
            CHECK(r.exact >= expectFrames(30) && r.wrongVoice == 0, "offset %.0f: %d exact, %d wrong", off, r.exact, r.wrongVoice);
            CHECK(std::fabs(r.carrier - off) < 25, "offset %.0f measured %.1f", off, r.carrier);
        }
        // bit clock error (ground station clocks are good, a drifting SDR clock is not)
        for (double ppm : {-1000.0, -300.0, -50.0, 50.0, 300.0, 1000.0}) {
            AeroCGenConfig g; g.fs = 48000; g.ebn0Db = 8; g.carrierHz = 300; g.clockPpm = ppm;
            const RunResult r = run(g, 30);
            CHECK(r.exact >= expectFrames(30) && r.wrongVoice == 0, "clock %+.0f ppm: %d exact, %d wrong", ppm, r.exact, r.wrongVoice);
        }
        // chunk sizes, including one sample at a time
        for (size_t chunk : {(size_t)1, (size_t)97, (size_t)4096, (size_t)65536}) {
            AeroCGenConfig g; g.fs = 48000; g.ebn0Db = 9; g.carrierHz = -900;
            const RunResult r = run(g, chunk == 1 ? 12 : 25, chunk);
            CHECK(r.exact >= expectFrames(chunk == 1 ? 12 : 25, 4.5) && r.wrongVoice == 0, "chunk %zu: %d exact, %d wrong", chunk, r.exact, r.wrongVoice);
        }
        // sample rates: the receiver takes any input rate (its mixer decimates to about 48 kHz); the carrier far from 0 Hz in a wide stream
        for (double fs : {48000.0, 96000.0, 240000.0, 1200000.0, 2400000.0}) {
            AeroCGenConfig g; g.fs = fs; g.ebn0Db = 10; g.carrierHz = fs > 100000 ? 0.31 * fs : 700;
            const RunResult r = run(g, 30, 65536, 600);
            CHECK(r.exact >= expectFrames(30) && r.wrongVoice == 0, "rate %.0f: %d exact, %d wrong", fs, r.exact, r.wrongVoice);
        }
        // Doppler-like drift (an aircraft-side oscillator and a moving satellite): a carrier ramp of 5 Hz per second is followed
        {
            AeroCGenConfig g; g.fs = 48000; g.ebn0Db = 9; g.carrierHz = 0;
            AeroCGenerator gen(g);
            AeroCReceiver rx; rx.configure(48000, 0, 2800);
            int n = 0; rx.setFrameCallback([&](const AeroCFrameEvent&) { n++; });
            std::vector<cf32> b(4800); double ph = 0; double t = 0;
            for (int blk = 0; blk < 400; blk++) {
                gen.generate(b.data(), b.size());
                for (auto& z : b) { const double f = -1200 + 5.0 * t; ph += 2 * M_PI * f / 48000; t += 1.0 / 48000; z *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
                rx.feed(b.data(), b.size());
            }
            CHECK(n >= expectFrames(40) * 9 / 10, "drift 5 Hz/s: %d frames", n);
        }
        // level steps (fading / AGC): the signal drops by 20 dB for 3 s and comes back; noise stays: the receiver must not lose the call for good
        {
            AeroCGenConfig g; g.fs = 48000; g.carrierHz = 500; g.amplitude = 1.0;
            AeroCGenerator gen(g);
            AeroCReceiver rx; rx.configure(48000, 0, 2800);
            std::mt19937 rng(3); std::normal_distribution<float> nd(0.f, 1.f);
            const float sigma = (float)std::sqrt(48000.0 / (2.0 * 8400 * std::pow(10.0, 1.2)));     // 12 dB at full level
            int n = 0; rx.setFrameCallback([&](const AeroCFrameEvent&) { n++; });
            std::vector<cf32> b(4800);
            for (int blk = 0; blk < 300; blk++) {
                gen.generate(b.data(), b.size());
                const float lev = (blk >= 100 && blk < 130) ? 0.1f : 1.f;
                for (auto& z : b) z = z * lev + cf32(sigma * nd(rng), sigma * nd(rng));
                rx.feed(b.data(), b.size());
            }
            CHECK(n >= 40, "level step: %d frames", n);
        }
        // a call that ends and another that starts on the same carrier later with another phase: the receiver locks again
        {
            AeroCReceiver rx; rx.configure(48000, 0, 2800);
            std::vector<AeroCFrameEvent> evs; rx.setFrameCallback([&](const AeroCFrameEvent& e) { evs.push_back(e); });
            std::mt19937 rng(8); std::normal_distribution<float> nd(0.f, 1.f);
            const float sigma = (float)std::sqrt(48000.0 / (2.0 * 8400 * std::pow(10.0, 1.0)));
            std::vector<cf32> b(4800);
            for (int call = 0; call < 3; call++) {
                AeroCGenConfig g; g.fs = 48000; g.carrierHz = -300 + 400.0 * call; g.seed = 10 + call; g.aesId = 0x100000u + (uint32_t)call;
                AeroCGenerator gen(g);
                for (int blk = 0; blk < 200; blk++) { gen.generate(b.data(), b.size()); for (auto& z : b) z += cf32(sigma * nd(rng), sigma * nd(rng)); rx.feed(b.data(), b.size()); }
                for (int blk = 0; blk < 100; blk++) { for (auto& z : b) z = cf32(sigma * nd(rng), sigma * nd(rng)); rx.feed(b.data(), b.size()); }   // 10 s of noise
            }
            int per[3] = {0, 0, 0};
            for (const auto& e : evs) if (e.haveCall && e.aesId >= 0x100000u && e.aesId < 0x100003u) per[e.aesId - 0x100000u]++;
            CHECK(per[0] > 3 && per[1] > 3 && per[2] > 3, "three calls on one receiver: call units %d %d %d", per[0], per[1], per[2]);
        }
        // the receiver must not report frames from noise or from a different signal (a 10500 bit/s P channel)
        {
            AeroCReceiver rx; rx.configure(48000, 0, 2800);
            int n = 0; rx.setFrameCallback([&](const AeroCFrameEvent&) { n++; });
            std::mt19937 rng(4); std::normal_distribution<float> nd(0.f, 1.f);
            std::vector<cf32> b(4800);
            for (int blk = 0; blk < 600; blk++) { for (auto& z : b) z = cf32(nd(rng), nd(rng)); rx.feed(b.data(), b.size()); }
            CHECK(n == 0, "frames from noise: %d", n);
            AeroFrameEncoder fe(10500);
            AeroModulator mod(10500, 48000);
            std::vector<uint8_t> info(312, 0x55);
            for (int f = 0; f < 40; f++) { auto bits = fe.frame(info.data(), aeroHeader(1, 0, f & 15, 0)); mod.pushBits(bits.data(), bits.size()); }
            while (mod.queuedBits() > 64) { mod.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); }
            CHECK(n == 0, "frames from a 10500 bit/s P channel: %d", n);
        }
    }
    return testSummary("aero_cchan");
}
