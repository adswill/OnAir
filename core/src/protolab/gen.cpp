// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: generators of test signals with known structure, to check the auto-detection (and for the "Demo signal" button).
// None of these is a real protocol; they have the typical shape of the things people reverse-engineer.
#include "dect2/protolab/protolab.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::protolab {

const char* testProtoName(TestProto p) {
    switch (p) {
        case TestProto::FskCrc16: return "2FSK packets, preamble + sync, CRC-16/CCITT-FALSE";
        case TestProto::ManchesterOok: return "OOK sensor, Manchester, CRC-8 (0x31)";
        case TestProto::HdlcFsk: return "2FSK, NRZI + HDLC framing, CRC-16/X-25";
        case TestProto::WhitenedFsk: return "2FSK, PN9 whitening, CRC-16 (0x8005, init 0xFFFF)";
        case TestProto::Fsk4Crc8: return "4FSK, preamble + sync, CRC-8 (0x31, init 0xFF)";
    }
    return "?";
}

namespace {
std::vector<float> smooth(const std::vector<float>& x, size_t w) {
    if (w <= 1) return x;
    std::vector<float> y(x.size());
    double acc = 0;
    for (size_t i = 0; i < x.size(); i++) {
        acc += x[i];
        if (i >= w) acc -= x[i - w];
        y[i] = (float)(acc / (double)std::min(i + 1, w));
    }
    return y;
}
void crcAppend(std::vector<uint8_t>& v, const CrcModel& m, size_t from, bool be) {
    const uint32_t c = crcCompute(m, v.data() + from, v.size() - from);
    const int nb = m.width / 8;
    for (int i = 0; i < nb; i++) v.push_back((uint8_t)(be ? (c >> (8 * (nb - 1 - i))) : (c >> (8 * i))));
}
}   // namespace

std::vector<cf> generateTestSignal(TestProto proto, const GenOptions& o, std::vector<std::vector<uint8_t>>* payloadsOut) {
    std::mt19937 rng(o.seed);
    dect2::normal_distribution<float> gauss(0.f, 1.f);
    const double pi = 3.14159265358979323846;
    const double sigma = std::sqrt(std::pow(10.0, -o.snrDb / 10.0) / 2.0);
    std::vector<cf> out;
    auto noise = [&](size_t n) { for (size_t i = 0; i < n; i++) { const float nr = (float)(sigma * gauss(rng)); const float ni = (float)(sigma * gauss(rng)); out.emplace_back(nr, ni); } };   // draws in a fixed order: call arguments are not evaluated in one
    noise((size_t)(o.fs * 0.008));
    if (payloadsOut) payloadsOut->clear();

    auto fskBurst = [&](const std::vector<float>& sym, double baud, double dev) {   // symbol levels in [-1, 1]
        baud *= o.baudScale;
        const double spb = o.fs / baud;
        std::vector<float> f;
        { double acc = 0; for (float s : sym) { acc += spb; while ((double)f.size() < acc) f.push_back(s); } }   // fractional symbol length
        f = smooth(f, (size_t)std::max(1.0, spb / 3));
        double ph = 0;
        const size_t base = out.size();
        for (size_t i = 0; i < f.size(); i++) {
            ph += 2 * pi * ((double)f[i] * dev + o.cfoHz) / o.fs;
            { const float nr = (float)(sigma * gauss(rng)); const float ni = (float)(sigma * gauss(rng)); out.emplace_back((float)std::cos(ph) + nr, (float)std::sin(ph) + ni); }
        }
        (void)base;
    };
    auto ookBurst = [&](const std::vector<float>& chips, double rate) {
        rate *= o.baudScale;
        const double spb = o.fs / rate;
        std::vector<float> a;
        { double acc = 0; for (float s : chips) { acc += spb; while ((double)a.size() < acc) a.push_back(s); } }
        a = smooth(a, (size_t)std::max(1.0, spb / 8));
        double ph = 0;
        for (size_t i = 0; i < a.size(); i++) {
            ph += 2 * pi * o.cfoHz / o.fs;
            { const float nr = (float)(sigma * gauss(rng)); const float ni = (float)(sigma * gauss(rng)); out.emplace_back(a[i] * (float)std::cos(ph) + nr, a[i] * (float)std::sin(ph) + ni); }
        }
    };
    auto bitsFrom = [](const std::vector<uint8_t>& bytes) { return bytesToBits(bytes, false); };
    auto asLevels = [](const std::vector<uint8_t>& bits) { std::vector<float> l; for (uint8_t b : bits) l.push_back(b ? 1.f : -1.f); return l; };

    uint8_t ctr = (uint8_t)(rng() & 0xFF);
    int temp = 215;
    for (int pk = 0; pk < o.packets; pk++) {
        temp += (int)(rng() % 7) - 3;
        const uint8_t hum = (uint8_t)(40 + rng() % 30);
        ctr = (uint8_t)(ctr + 1);
        std::vector<uint8_t> body;   // the bytes after the sync word
        switch (proto) {
            case TestProto::FskCrc16: {
                body = {9, 0xA3, 0x1C, ctr, (uint8_t)(temp >> 8), (uint8_t)temp, hum, (uint8_t)(180 + rng() % 40), 0x01};
                crcAppend(body, {"", 16, 0x1021, 0xFFFF, false, false, 0}, 0, true);
                std::vector<uint8_t> all = {0xAA, 0xAA, 0xAA, 0xAA, 0x2D, 0xD4};
                all.insert(all.end(), body.begin(), body.end());
                fskBurst(asLevels(bitsFrom(all)), 4800, 2400);
                break;
            }
            case TestProto::ManchesterOok: {
                body = {0x5B, ctr, (uint8_t)(temp >> 8), (uint8_t)temp, hum};
                crcAppend(body, {"", 8, 0x31, 0x00, false, false, 0}, 0, true);
                std::vector<uint8_t> all = {0xFF, 0xFF, 0x5D, 0x2B};
                all.insert(all.end(), body.begin(), body.end());
                std::vector<float> chips;
                std::vector<uint8_t> dbits = bitsFrom(all);
                dbits.push_back(1);   // a last bit so that the final chip of the CRC is not lost when the carrier drops
                for (uint8_t b : dbits) { chips.push_back(b ? 0.f : 1.f); chips.push_back(b ? 1.f : 0.f); }   // 1 = low, high (IEEE 802.3)
                ookBurst(chips, 2000);
                break;
            }
            case TestProto::HdlcFsk: {
                std::vector<uint8_t> frame = {0x84, 0xA2, 0x03, ctr, (uint8_t)(temp >> 8), (uint8_t)temp, hum, 0x11, 0x22, 0x33};
                crcAppend(frame, {"", 16, 0x1021, 0xFFFF, true, true, 0xFFFF}, 0, false);
                body = frame;
                std::vector<uint8_t> bits;   // LSB first, stuffed
                int ones = 0;
                for (uint8_t v : frame) for (int k = 0; k < 8; k++) {
                    const uint8_t b = (v >> k) & 1;
                    bits.push_back(b);
                    ones = b ? ones + 1 : 0;
                    if (ones == 5) { bits.push_back(0); ones = 0; }
                }
                std::vector<uint8_t> all;
                const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
                for (int r = 0; r < 4; r++) all.insert(all.end(), flag, flag + 8);
                all.insert(all.end(), bits.begin(), bits.end());
                for (int r = 0; r < 2; r++) all.insert(all.end(), flag, flag + 8);
                std::vector<uint8_t> nrzi;
                uint8_t lvl = 0;
                for (uint8_t b : all) { if (!b) lvl ^= 1; nrzi.push_back(lvl); }
                fskBurst(asLevels(nrzi), 9600, 3000);
                break;
            }
            case TestProto::WhitenedFsk: {
                body = {11, 0xC4, 0x07, ctr, (uint8_t)(temp >> 8), (uint8_t)temp, hum, 0x00, 0x7F, (uint8_t)(rng() & 0xFF), 0x22};
                crcAppend(body, {"", 16, 0x8005, 0xFFFF, false, false, 0}, 0, true);
                if (payloadsOut) payloadsOut->push_back(body);
                std::vector<uint8_t> w = body;
                whiten(w, whiteningCandidates()[0]);
                std::vector<uint8_t> all = {0xAA, 0xAA, 0xAA, 0xAA, 0xD3, 0x91};
                all.insert(all.end(), w.begin(), w.end());
                fskBurst(asLevels(bitsFrom(all)), 9600, 4000);
                body.clear();
                break;
            }
            case TestProto::Fsk4Crc8: {
                body = {0x52, 0x10, ctr, (uint8_t)(temp >> 8), (uint8_t)temp, hum, (uint8_t)(rng() & 0x3F)};
                crcAppend(body, {"", 8, 0x31, 0xFF, false, false, 0}, 0, true);
                std::vector<uint8_t> all = {0xAA, 0xAA, 0xAA, 0xAA, 0x6E, 0x91};
                all.insert(all.end(), body.begin(), body.end());
                std::vector<float> sym;
                static const float lev[4] = {-1.f, -1.f / 3, 1.f, 1.f / 3};   // Gray, indexed by the two bits: 00 -> -3, 01 -> -1, 11 -> +1, 10 -> +3 (as the slicer reads them)
                const std::vector<uint8_t> bits = bitsFrom(all);
                for (size_t i = 0; i + 1 < bits.size(); i += 2) sym.push_back(lev[bits[i] * 2 + bits[i + 1]]);
                fskBurst(sym, 4800, 3000);
                break;
            }
        }
        if (payloadsOut && !body.empty()) payloadsOut->push_back(body);
        noise((size_t)(o.fs * (0.012 + 0.008 * ((rng() & 7) / 7.0))));
    }
    return out;
}

}   // namespace dect2::protolab
