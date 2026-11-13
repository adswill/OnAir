// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL codec v3 against the Python kaal_codec_v3.py: the bits of the first frames at 12, 16, 24 and 32 kbit/s are identical, the decode of
// those bits follows the Python to within one LSB (also with a wrong-profile and a wrong-shape frame), the profile is read from the header
// by an any-profile stream decoder, and a round trip keeps a tone.
#include "dect2/kaal/kaal_codec3.h"
#include "kaal_golden_codec3.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<int16_t> signal() {   // same as gen_golden_codec3.py
    std::vector<int16_t> x(16000);
    int64_t st = 12345;
    for (int i = 0; i < 16000; i++) {
        const double t = i / 16000.0;
        const double env = 0.6 + 0.4 * std::sin(2.0 * 3.141592653589793 * 3.0 * t);
        st = (st * 1103515245LL + 12345) & 0x7FFFFFFFLL;
        const int nz = (int)((st >> 16) % 1201) - 600;
        const double v = env * (4000.0 * std::sin(2.0 * 3.141592653589793 * 200.0 * t) + 2500.0 * std::sin(2.0 * 3.141592653589793 * 600.0 * t) +
                                1500.0 * std::sin(2.0 * 3.141592653589793 * 2400.0 * t)) + nz;
        x[i] = (int16_t)v;
    }
    return x;
}
static std::vector<uint8_t> hexBits(const char* h, size_t nbits) {
    std::vector<uint8_t> b(nbits);
    for (size_t i = 0; i < nbits / 4; i++) {
        const char c = h[i];
        const int v = c <= '9' ? c - '0' : c - 'a' + 10;
        for (int j = 0; j < 4; j++) b[i * 4 + j] = (uint8_t)((v >> (3 - j)) & 1);
    }
    return b;
}
static const char* const* goldBits(int kb) { return kb == 12 ? kG3Bits12 : kb == 16 ? kG3Bits16 : kb == 24 ? kG3Bits24 : kG3Bits32; }
static const int16_t* goldPcm(int kb) { return kb == 12 ? kG3Pcm12 : kb == 16 ? kG3Pcm16 : kb == 24 ? kG3Pcm24 : kG3Pcm32; }

int main() {
    const std::vector<int16_t> x = signal();
    for (int kb : {12, 16, 24, 32}) {
        const KaalCodec3 c(kb);
        const size_t fb = c.frameBits();
        CHECK(fb == (size_t)kb * 20, "frame bits %zu", fb);
        const std::vector<uint8_t> bits = c.encode(x);
        CHECK(bits.size() == 51 * fb, "%d kbit/s: %zu bits", kb, bits.size());
        size_t badFrames = 0;
        for (int f = 0; f < kG3Frames; f++) {
            const std::vector<uint8_t> g = hexBits(goldBits(kb)[f], fb);
            size_t d = 0;
            for (size_t i = 0; i < fb; i++) d += g[i] != bits[(size_t)f * fb + i];
            if (d) { badFrames++; printf("  %d kbit/s frame %d: %zu bits differ from the Python\n", kb, f, d); }
        }
        CHECK(badFrames == 0, "%d kbit/s: %zu of %d frames differ from the Python bit stream", kb, badFrames, kG3Frames);
        // header names the profile
        CHECK(KaalCodec3::bitrateOfHeader(bits.data()) == kb, "%d kbit/s: header reads %d", kb, KaalCodec3::bitrateOfHeader(bits.data()));
        // decode of the golden bits
        std::vector<uint8_t> gb;
        for (int f = 0; f < kG3Frames; f++) { const auto g = hexBits(goldBits(kb)[f], fb); gb.insert(gb.end(), g.begin(), g.end()); }
        const std::vector<int16_t> y = c.decode(gb.data(), kG3Frames);
        int worst = 0;
        for (size_t i = 0; i < y.size(); i++) worst = std::max(worst, std::abs((int)y[i] - (int)goldPcm(kb)[i]));
        CHECK(worst <= 1, "%d kbit/s: decode differs from the Python by %d", kb, worst);
        printf("%d kbit/s: golden bits ok=%d, decode worst difference %d LSB\n", kb, badFrames == 0, worst);
        // round trip
        const std::vector<int16_t> all = c.decode(bits.data(), bits.size() / fb);
        double s = 0, e = 0;
        for (size_t i = 2000; i < 14000; i++) { const double r = x[i], d = r - all[KaalCodec3::kDelay + i]; s += r * r; e += d * d; }
        const double snr = 10 * std::log10(s / e);
        printf("  round trip SNR %.1f dB\n", snr);
        CHECK(snr > 10.0, "%d kbit/s: round trip SNR %.1f dB", kb, snr);
        // stream decoder = decode
        KaalCodec3::StreamDecoder sd(kb);
        std::vector<int16_t> ys(51 * 320);
        int wrongProfile = 0;
        for (size_t t = 0; t < 51; t++) wrongProfile += sd.push(&bits[t * fb], fb, &ys[t * 320]) != kb;
        CHECK(ys == all && wrongProfile == 0, "%d kbit/s: stream decoder differs from decode()", kb);
    }
    // damaged: a 24 kbit/s frame inside a 16 kbit/s stream and a 7-bit frame are concealed exactly like the Python
    {
        const KaalCodec3 c16(16), c24(24);
        const std::vector<uint8_t> b16 = c16.encode(x), b24 = c24.encode(x);
        std::vector<std::vector<uint8_t>> fr;
        for (int f = 0; f < kG3Frames; f++) fr.push_back(hexBits(kG3Bits16[f], 320));
        fr[2] = std::vector<uint8_t>(b24.begin() + 2 * 480, b24.begin() + 3 * 480);
        fr[4] = std::vector<uint8_t>(7, 0);
        const std::vector<int16_t> y = c16.decode(fr);
        int worst = 0;
        for (size_t i = 0; i < y.size(); i++) worst = std::max(worst, std::abs((int)y[i] - (int)kG3PcmDamaged16[i]));
        CHECK(worst <= 1, "damaged decode differs from the Python by %d", worst);
        printf("damaged 16 kbit/s decode: worst difference %d LSB\n", worst);
    }
    // an any-profile stream decoder follows a profile change from the headers
    {
        KaalCodec3::StreamDecoder sd(0);
        const int order[6] = {12, 16, 24, 32, 16, 12};
        int bad = 0;
        for (int i = 0; i < 6; i++) {
            const KaalCodec3 c(order[i]);
            const std::vector<uint8_t> b = c.encode(x);
            int16_t out[320];
            bad += sd.push(&b[(size_t)(10 + i) * c.frameBits()], c.frameBits(), out) != order[i];
        }
        CHECK(bad == 0, "the any-profile decoder missed %d profile changes", bad);
        int16_t out[320];
        uint8_t junk[320];
        for (int i = 0; i < 320; i++) junk[i] = (uint8_t)(i * 7 % 2);
        CHECK(sd.push(junk, 321, out) == 0 && sd.push(nullptr, 0, out) == 0, "a wrong length or a lost frame must be concealed");
    }
    // non-binary bytes are a lost frame, not a crash
    {
        const KaalCodec3 c(16);
        std::vector<uint8_t> b = c.encode(x);
        b[320 * 3 + 50] = 7;
        const auto y = c.decode(b.data(), b.size() / 320);
        CHECK(y.size() == 51 * 320, "decode length");
    }
    return testSummary("kaal_codec3");
}
