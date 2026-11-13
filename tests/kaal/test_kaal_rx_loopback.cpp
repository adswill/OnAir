// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL streaming receiver (v3 format), loopback: the transmit signal built from the PHY functions is fed in 1000-sample chunks, for
// every codec bitrate. Clean: the audio equals the codec-only decode bit for bit and the bitrate is read from the frame header. Noise
// (Eb/N0 10 dB), a frequency offset of +-200 Hz and a start in the middle of the stream: the frame error rate stays low.
#include "dect2/kaal/kaal_codec3.h"
#include "dect2/kaal/kaal_rx.h"
#include "kaal_rx_gen.h"
#include <algorithm>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

struct Run {
    std::vector<int16_t> audio;
    kaal::RxStats st;
};

static Run run(const std::vector<std::complex<float>>& sig, size_t from) {
    kaal::VoiceRx rx;
    rx.configure(48000.0);
    Run r;
    std::vector<int16_t> buf(4096);
    for (size_t i = from; i < sig.size(); i += 1000) {
        rx.feed(&sig[i], std::min<size_t>(1000, sig.size() - i));
        for (size_t n; (n = rx.popAudio(buf.data(), buf.size())) > 0;) r.audio.insert(r.audio.end(), buf.begin(), buf.begin() + (std::ptrdiff_t)n);
    }
    r.st = rx.stats();
    return r;
}

static double fer(const kaal::RxStats& s) {
    const double n = (double)(s.framesGood + s.framesBad);
    return n > 0 ? (double)s.framesBad / n : 1.0;
}

int main() {
    const std::vector<int16_t> pcm = kaalrx::testPcm(16000 * 6);
    for (int kb : {12, 16, 24, 32}) {
        printf("--- %d kbit/s\n", kb);
        const std::vector<uint8_t> bits = kaalrx::frameBits(pcm, kb);
        double pw = 0;
        const std::vector<kaalrx::cd> tx = kaalrx::transmit(bits, kb, "", pw);
        const size_t fb = (size_t)kb * 20, nfr = bits.size() / fb;
        printf("signal: %zu samples (%.1f s), %zu frames, air time per audio second %.2f\n", tx.size(), tx.size() / 48000.0, nfr,
               (double)tx.size() / 48000.0 / (nfr * 0.02));
        KaalCodec3 codec(kb);
        const std::vector<int16_t> all = codec.decode(bits.data(), nfr);
        const std::vector<int16_t> expected(all.begin() + KaalCodec3::kDelay, all.end());
        {   // clean: bit exact against the codec alone
            const Run r = run(kaalrx::channelSnr(tx, pw, 60.0, 0.0, 1), 0);
            printf("clean: %zu of %zu samples, words good %llu bad %llu, snr %.1f dB, sweep %.1f dB, bitrate %d\n", r.audio.size(), expected.size(),
                   (unsigned long long)r.st.framesGood, (unsigned long long)r.st.framesBad, r.st.snrDb, r.st.sweepDb, r.st.bitrateKbps);
            CHECK(r.st.framesBad == 0, "%d: bad words %llu", kb, (unsigned long long)r.st.framesBad);
            CHECK(r.st.bitrateKbps == kb, "%d: bitrate read from the header: %d", kb, r.st.bitrateKbps);
            CHECK(r.audio.size() >= expected.size() * 3 / 4, "%d: audio %zu of %zu", kb, r.audio.size(), expected.size());
            size_t diff = 0, lead = 0;
            for (size_t i = 0; i < r.audio.size() && i < expected.size(); i++) diff += r.audio[i] != expected[i];
            (void)lead;
            CHECK(diff == 0, "%d: %zu samples differ from the codec-only output", kb, diff);
        }
        {   // Eb/N0 10 dB
            const Run r = run(kaalrx::channel(tx, pw, 10.0, 0.0, 2, kb), 0);
            printf("10 dB: words good %llu bad %llu (FER %.3f), snr est %.1f dB\n", (unsigned long long)r.st.framesGood, (unsigned long long)r.st.framesBad, fer(r.st), r.st.snrDb);
            CHECK(r.st.framesGood + r.st.framesBad >= 150, "%d: words %llu", kb, (unsigned long long)(r.st.framesGood + r.st.framesBad));
            CHECK(fer(r.st) < 0.05, "%d: FER %.3f", kb, fer(r.st));
        }
        for (double cfo : {-200.0, 200.0}) {   // frequency offset
            const Run r = run(kaalrx::channel(tx, pw, 12.0, cfo, 3, kb), 0);
            printf("cfo %+.0f Hz: words good %llu bad %llu (FER %.3f), cfo est %.1f Hz\n", cfo, (unsigned long long)r.st.framesGood,
                   (unsigned long long)r.st.framesBad, fer(r.st), r.st.cfoHz);
            CHECK(r.st.framesGood + r.st.framesBad >= 150, "%d: words %llu", kb, (unsigned long long)(r.st.framesGood + r.st.framesBad));
            CHECK(fer(r.st) < 0.05, "%d: FER %.3f at %+.0f Hz", kb, fer(r.st), cfo);
            CHECK(std::fabs(r.st.cfoHz - cfo) < 30.0, "%d: cfo estimate %.1f", kb, r.st.cfoHz);
        }
        {   // start in the middle of the stream (not on a frame or sweep boundary)
            const Run r = run(kaalrx::channel(tx, pw, 12.0, 0.0, 4, kb), 100003);
            printf("mid start: words good %llu bad %llu (FER %.3f), audio %zu samples\n", (unsigned long long)r.st.framesGood, (unsigned long long)r.st.framesBad,
                   fer(r.st), r.audio.size());
            CHECK(r.st.framesGood >= 60, "%d: good words %llu", kb, (unsigned long long)r.st.framesGood);
            CHECK(fer(r.st) < 0.05, "%d: FER %.3f", kb, fer(r.st));
            CHECK(r.audio.size() >= 16000, "%d: audio %zu", kb, r.audio.size());
        }
    }
    return testSummary("kaal_rx_loopback");
}
