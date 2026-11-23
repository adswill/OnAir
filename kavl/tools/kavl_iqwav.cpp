// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// kavl_iqwav: writes a demonstration KAVL signal as an IQ .wav (2 channels I/Q, 16-bit, 4 samples per symbol), which opens in SDR++ (File source) and the
// OnAir IQ-file player. The payload is random bytes (no video yet).
//   kavl_iqwav [--modcod N] [--seconds S] [--symrate R] [--snr DB] [--freq HZ] [--out FILE]
// The default name carries the centre frequency the way SDR++ reads it (..._433000000Hz.wav).
#include "kavl/kavl_phy.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

}  // namespace

int main(int argc, char** argv) {
    int mc = 3;
    double seconds = 5.0, symRate = kavl::kDefaultSymRate, snr = 100.0, freq = 433000000.0;
    std::string out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--modcod") mc = std::atoi(v);
        else if (k == "--seconds") seconds = std::atof(v);
        else if (k == "--symrate") symRate = std::atof(v);
        else if (k == "--snr") snr = std::atof(v);
        else if (k == "--freq") freq = std::atof(v);
        else if (k == "--out") out = v;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    const kavl::Modcod* m = kavl::modcod(mc);
    if (!m || seconds <= 0 || symRate <= 0) {
        std::fprintf(stderr, "bad --modcod, --seconds or --symrate\n");
        return 2;
    }
    if (out.empty()) out = "kavl_" + std::to_string(static_cast<long long>(std::llround(freq))) + "Hz.wav";
    const int frames = std::max(1, static_cast<int>(std::ceil(seconds / kavl::frameSeconds(symRate))));
    dect2::kris::XorShift64 r{12345};
    std::vector<std::vector<uint8_t>> pay(static_cast<size_t>(frames));
    for (auto& p : pay) {
        p.resize(static_cast<size_t>(kavl::framePayloadBytes(*m)));
        for (auto& b : p) b = static_cast<uint8_t>(r.next() >> 56);
    }
    kavl::Transmitter tx(mc);
    auto iq = tx.modulate(pay, 0, 0, 0, 256, 256);
    if (snr < 99.0) {   // noise at the given Es/N0 (power 1 per sample, 4 samples per symbol)
        const double sd = std::sqrt(kavl::kSps / std::pow(10.0, snr / 10.0) / 2.0);
        dect2::kris::XorShift64 nr{777};
        auto g = [&]() {
            const double u1 = std::max(1e-300, static_cast<double>(nr.next() >> 11) / 9007199254740992.0);
            const double u2 = static_cast<double>(nr.next() >> 11) / 9007199254740992.0;
            return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        };
        for (auto& v : iq) v += kavl::cf(static_cast<float>(sd * g()), static_cast<float>(sd * g()));
    }
    const uint32_t rate = static_cast<uint32_t>(std::llround(kavl::kSps * symRate));
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(iq.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);   // PCM
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
    const float scale = 0.18f * 32767.f;   // rms at 18% of full scale leaves room for the peaks
    std::vector<int16_t> s(iq.size() * 2);
    for (size_t i = 0; i < iq.size(); i++) {
        s[2 * i] = static_cast<int16_t>(std::lround(std::fmax(-32768.f, std::fmin(32767.f, iq[i].real() * scale))));
        s[2 * i + 1] = static_cast<int16_t>(std::lround(std::fmax(-32768.f, std::fmin(32767.f, iq[i].imag() * scale))));
    }
    std::fwrite(s.data(), 2, s.size(), f);   // little-endian hosts
    std::fclose(f);
    std::printf("%s: %d frames of %s, %.0f ksym/s, %.0f kHz wide, %u samples/s, %.1f s, %.3f Mbit/s payload\n", out.c_str(), frames, m->name, symRate / 1e3,
                kavl::occupiedHz(symRate) / 1e3, rate, static_cast<double>(iq.size()) / rate, kavl::netBitRate(*m, symRate) / 1e6);
    return 0;
}
