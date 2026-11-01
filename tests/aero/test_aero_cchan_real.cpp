// Aero C channel on a REAL recording: 14 s of the 8400 bit/s voice-channel sample from JAERO's sample set (github.com/jontio/JAERO, samples/
// 8400bps_ambe_sample.ogg, GPL-3, Jonti Olds; see tests/data/aero/README.txt). The audio is a receiver's 48 kHz output with the carrier at about
// 2.5 kHz; here it is an 8-bit file. Checks that framing, descrambling and decoding give valid signal units (CRC-16) and the call-progress
// unit of the real call, and that the voice bytes of that frame are the ones JAERO's framing gives (known answer).
#include "dect2/aero/aero_crx.h"
#include "dect2/dsp/fftutil.h"
#include <cstdio>
#include <cstring>
#include <fstream>
using namespace dect2;
#include "../common/test_util.h"

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

int main() {
    std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/tests/data/aero/jaero_c8400_48k.s8", std::ios::binary);
    CHECK(f.good(), "sample file not found");
    if (!f.good()) return testSummary("aero_cchan_real");
    std::vector<int8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(raw.size() == 672000, "sample size %zu", raw.size());
    // real audio -> analytic signal (FFT, drop the negative frequencies)
    const int N = 1 << 20;
    std::vector<cf32> a(N, cf32(0, 0));
    for (size_t i = 0; i < raw.size(); i++) a[i] = cf32(raw[i] / 127.f, 0);
    Fft fft(N);
    fft.forward(a.data());
    for (int i = 1; i < N / 2; i++) a[i] *= 2.f;
    for (int i = N / 2 + 1; i < N; i++) a[i] = cf32(0, 0);
    fft.inverse(a.data());
    for (auto& z : a) z /= (float)N;

    AeroCReceiver rx;
    rx.configure(48000, 0, 3000);
    int frames = 0, suOk = 0, calls = 0, exactKnown = 0;
    double berSum = 0;
    static const uint8_t known[12] = {0xa9, 0x95, 0x6e, 0x19, 0xfd, 0xd8, 0xbe, 0x86, 0x91, 0x08, 0x03, 0x5b};   // first voice frame of the call-progress frame
    rx.setFrameCallback([&](const AeroCFrameEvent& e) {
        frames++;
        for (int j = 0; j < 3; j++) suOk += e.suOk[j];
        berSum += e.channelBer;
        if (e.haveCall) {
            calls++;
            CHECK(e.aesId == 0x7812E7 && e.gesId == 0x90, "call %06x ges %02x", e.aesId, e.gesId);
            exactKnown += std::memcmp(e.voice.data(), known, 12) == 0;
        }
    });
    for (size_t i = 0; i < raw.size(); i += 4096) rx.feed(&a[i], std::min<size_t>(4096, raw.size() - i));
    printf("real recording: %d frames, %d of %d signal units pass CRC-16, %d call units, channel BER %.3f, carrier %.1f Hz\n", frames, suOk, 3 * frames, calls, frames ? berSum / frames : 0, rx.carrierHz());
    CHECK(frames >= 18, "frames: %d", frames);
    CHECK(suOk >= 3 * frames * 9 / 10, "signal units with a good CRC: %d of %d", suOk, 3 * frames);
    CHECK(calls >= 1, "no call-progress unit");
    CHECK(exactKnown >= 1, "known voice frame not found");
    CHECK(frames && berSum / frames < 0.08, "channel BER %.3f", berSum / frames);
    CHECK(rx.carrierHz() > 2300 && rx.carrierHz() < 2800, "carrier %.1f", rx.carrierHz());
    return testSummary("aero_cchan_real");
}
