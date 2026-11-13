// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL v3 transmitter (VoiceTx with a codec bitrate): the air time per second of speech (12 and 16 kbit/s exactly real time, 24 and 32 twice
// as long), the receiver decoding the transmitter's own signal at every bitrate with the profile read from the frame headers, the callsign
// in the sweep, chunk-size independence, and the refused settings.
#include "dect2/kaal/kaal_rx.h"
#include "dect2/kaal/kaal_tx.h"
#include "kaal_rx_gen.h"
#include <algorithm>
#include <cstdio>
using namespace dect2;
using namespace dect2::kaal;
#include "../common/test_util.h"

static std::vector<std::complex<float>> makeIq(int kb, const std::vector<int16_t>& x, size_t chunk, const char* call) {
    VoiceTx tx;
    tx.configure(kb >= 24 ? 96000 : 48000);   // 24 / 32 kbit/s: 16 ksym/s, the same stream at twice the speed; the receiver reads it as 48 kHz
    CHECK(tx.setBitrate(kb), "setBitrate %d", kb);
    if (call) tx.setSweepCallsign(call);
    std::vector<std::complex<float>> all, buf(5000);
    auto drain = [&] { for (size_t n; (n = tx.popIq(buf.data(), buf.size())) > 0;) all.insert(all.end(), buf.begin(), buf.begin() + (std::ptrdiff_t)n); };
    for (size_t i = 0; i < x.size(); i += chunk) { tx.pushAudio(&x[i], std::min(chunk, x.size() - i)); drain(); }
    tx.flush();
    drain();
    return all;
}

int main() {
    const std::vector<int16_t> pcm = kaalrx::testPcm(16000 * 20);   // 20 s of speech-like signal
    for (int kb : {12, 16, 24, 32}) {
        const auto iq = makeIq(kb, pcm, 777, "OH2XYZ/MM");
        const double rateHz = kb >= 24 ? 96000.0 : 48000.0;
        const double ratio = (double)iq.size() / rateHz / 20.0;   // air time in real seconds (a 16 ksym/s stream is 96000 samples a second)
        const double want = kaalAirTimeRatio(kb);
        printf("%d kbit/s: %zu IQ samples = %.2f s for 20 s of speech, air time ratio %.3f (model %.2f)\n", kb, iq.size(), iq.size() / rateHz, ratio, want);
        CHECK(ratio > want - 0.01 && ratio < want + 0.03, "%d: air time ratio %.3f, expected about %.2f", kb, ratio, want);
        CHECK(ratio <= 1.03, "%d kbit/s must be real time (air time <= audio time + the tail): %.3f", kb, ratio);
        // the receiver on the transmitter's own signal
        VoiceRx rx;
        rx.configure(48000.0);
        const auto sig = std::vector<std::complex<float>>(iq);
        std::vector<int16_t> audio, b(4096);
        for (size_t i = 0; i < sig.size(); i += 1000) {
            rx.feed(&sig[i], std::min<size_t>(1000, sig.size() - i));
            for (size_t n; (n = rx.popAudio(b.data(), b.size())) > 0;) audio.insert(audio.end(), b.begin(), b.begin() + (std::ptrdiff_t)n);
        }
        const RxStats st = rx.stats();
        // best-lag SNR against the input
        double bestSnr = -99;
        for (int lag = -700; lag <= 700; lag += 1) {
            double s = 0, e = 0;
            for (size_t i = 16000; i < 16000 + 16000 * 8 && i < pcm.size(); i++) {
                const long j = (long)i + lag;
                if (j < 0 || j >= (long)audio.size()) continue;
                const double r = pcm[i], d = r - audio[(size_t)j];
                s += r * r; e += d * d;
            }
            if (e > 0) bestSnr = std::max(bestSnr, 10 * std::log10(s / e));
        }
        printf("  rx: words good %llu bad %llu, bitrate read %d, audio %zu samples, SNR vs input %.1f dB, sweep ID '%s'\n", (unsigned long long)st.framesGood,
               (unsigned long long)st.framesBad, st.bitrateKbps, audio.size(), bestSnr, st.idCall.c_str());
        CHECK(st.bitrateKbps == kb, "%d: bitrate read %d", kb, st.bitrateKbps);
        CHECK(st.framesBad == 0, "%d: %llu bad words on a clean loopback", kb, (unsigned long long)st.framesBad);
        CHECK(bestSnr > 8.0, "%d: decoded speech SNR %.1f dB", kb, bestSnr);
        CHECK(st.idValid && st.idCall == "OH2XYZ/MM", "%d: sweep ID '%s'", kb, st.idCall.c_str());
    }
    {   // chunking changes nothing
        const std::vector<int16_t> y(pcm.begin(), pcm.begin() + 6400);
        const auto a = makeIq(16, y, 6400, "ABC"), c = makeIq(16, y, 1, "ABC"), d = makeIq(24, y, 333, "ABC"), e = makeIq(24, y, 6400, "ABC");
        CHECK(a == c && !a.empty(), "16 kbit/s: one-sample pushes differ");
        CHECK(d == e && !d.empty(), "24 kbit/s: chunk size changes the output");
    }
    {   // refused and default settings
        VoiceTx tx;
        CHECK(tx.bitrate() == 0, "VoiceTx itself starts in the legacy format");
        CHECK(!tx.setBitrate(20) && !tx.setBitrate(-1) && tx.bitrate() == 0, "bad bitrates are refused");
        CHECK(tx.setBitrate(32) && tx.bitrate() == 32 && tx.setBitrate(0) && tx.bitrate() == 0, "bitrate switching");
        CHECK(tx.setBitrate(16), "16");
        CHECK(!tx.setSweepCallsign("TOOLONGCALL") && !tx.setSweepCallsign(""), "a callsign that does not fit leaves the sweep without ID");
        CHECK(tx.setSweepCallsign("ly2abc/p"), "lower case is raised");
        CHECK(kaalAirTimeRatio(0) > 1.26 && kaalAirTimeRatio(0) < 1.27, "legacy ratio %.4f", kaalAirTimeRatio(0));
    }
    return testSummary("kaal_tx_v3");
}
