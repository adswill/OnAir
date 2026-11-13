// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Register maths and sample unpacking of the Mirics MSi2500 + MSi001 (dect2/source/mirics.h). No USB here: the tests run it without a radio.
// The register layout is the one libmirisdr documents (GPL-2.0-or-later, THIRD_PARTY.md); the code is OnAir's own.
#include "dect2/source/mirics.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace mirics {

namespace {
const FormatInfo kFormats[] = {
    {Format::S14, "14", "14-bit", 252, 6.048e6, 0x000094, 0x1},
    {Format::S12, "12", "12-bit", 336, 8.064e6, 0x000085, 0x5},
    {Format::S10, "10", "10-bit (+2-bit range)", 384, 9.216e6, 0x0000a5, 0x9},
    {Format::S8, "8", "8-bit", 504, 12.096e6, 0x000c94, 0xd},
};

// Mode bits of the tuner's register 0 (bits 4..8): AM, VHF, band III, bands IV/V, L band. 6 (VHF + band III) is what the 259/261-330/404 MHz
// stretch uses with the LO divided by 8.
constexpr int kModeAm = 0x01, kModeVhf = 0x02, kModeB3 = 0x04, kModeB45 = 0x08, kModeBl = 0x10, kModeB3Lo8 = 0x06;

// low edge (MHz), band, mode bits, LO divider, AM up-converter, band-switch word (bridge register 8)
const BandPlan kGeneric[] = {
    {0, Band::Am, kModeAm, 16, true, 0xf780},
    {12, Band::Am, kModeAm, 16, true, 0xff80},
    {30, Band::Am, kModeAm, 16, true, 0xf280},
    {50, Band::Vhf, kModeVhf, 32, false, 0xf380},
    {108, Band::B3, kModeB3, 16, false, 0xfa80},
    {250, Band::B3, kModeB3, 16, false, 0xf680},
    {259, Band::B3, kModeB3Lo8, 8, false, 0xf680},
    {330, Band::B45, kModeB45, 4, false, 0xf380},
    {960, Band::L, kModeBl, 2, false, 0xfa80},
};
const BandPlan kRsp1[] = {
    {0, Band::Am, kModeAm, 16, true, 0xf580},
    {12, Band::Am, kModeAm, 16, true, 0xf580},
    {30, Band::Am, kModeAm, 16, true, 0xf580},
    {50, Band::Vhf, kModeVhf, 32, false, 0xf180},
    {112, Band::B3, kModeB3, 16, false, 0xf580},
    {250, Band::B3, kModeB3, 16, false, 0xf480},
    {261, Band::B3, kModeB3Lo8, 8, false, 0xf480},
    {404, Band::B45, kModeB45, 4, false, 0xf580},
    {1000, Band::L, kModeBl, 2, false, 0xf580},
};

constexpr uint64_t kSynthRef = 96000000;   // the tuner's synthesiser steps in 96 MHz (4 x the 24 MHz crystal)
constexpr uint64_t kAmShiftHz = 120000000; // the AM inputs are up-converted by 5 x 24 MHz first
constexpr uint64_t kRateRef = 48000000;    // the bridge's ADC clock synthesiser reference

uint64_t gcd(uint64_t a, uint64_t b) { while (b) { const uint64_t t = a % b; a = b; b = t; } return a; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
} // namespace

const FormatInfo& formatInfo(Format f) { return kFormats[(int)f]; }

Format formatFor(double rateHz, const std::string& wish) {
    for (const auto& fi : kFormats)
        if (wish == fi.key && rateHz <= fi.maxRateHz * (1 + 1e-9)) return fi.f;
    for (const auto& fi : kFormats)
        if (rateHz <= fi.maxRateHz * (1 + 1e-9)) return fi.f;
    return Format::S8;
}

RateRegs rateRegs(double rateHz, Format f) {
    RateRegs r;
    const uint64_t rate = (uint64_t)std::llround(std::clamp(rateHz, kMinRateHz, kMaxRateHz));
    // the ADC clock synthesiser runs at rate x div x 12 and must be at least 202 MHz: the smallest even divider 4..14 that gets there
    int div = 14;
    for (int d = 4; d <= 14; d += 2) if (rate * (uint64_t)d * 12 >= 202000000ull) { div = d; break; }
    const uint64_t vco = rate * (uint64_t)div * 12;
    r.div = div;
    r.n = (uint32_t)(vco / kRateRef);
    r.frac21 = (uint32_t)((0x200000ull * (vco % kRateRef)) / kRateRef);   // 21-bit fraction: the top bit goes into register 3, the rest into 4
    r.reg4 = r.frac21 & 0xfffff;
    r.reg3 = 0x3u | ((uint32_t)(div / 2 - 1) & 7u) << 2 | ((r.frac21 >> 20) & 1u) << 7 | (r.n & 0xfu) << 8 |
             (formatInfo(f).reg3Code & 0xfu) << 12 | 1u << 16;
    r.rateHz = (r.n + r.frac21 / 2097152.0) * (double)kRateRef / (div * 12.0);
    return r;
}

const BandPlan& bandFor(double hz, Flavour fl) {
    const BandPlan* plan = fl == Flavour::Rsp1 ? kRsp1 : kGeneric;
    const size_t n = fl == Flavour::Rsp1 ? sizeof kRsp1 / sizeof *kRsp1 : sizeof kGeneric / sizeof *kGeneric;
    size_t i = 0;
    while (i + 1 < n && hz >= plan[i + 1].lowMhz * 1e6) i++;
    return plan[i];
}

const std::vector<uint32_t>& filterWidths() {
    static const std::vector<uint32_t> w = {200000, 300000, 600000, 1536000, 5000000, 6000000, 7000000, 8000000};
    return w;
}

uint32_t filterFor(double rateHz, uint32_t wishHz) {
    const auto& w = filterWidths();
    if (wishHz && std::find(w.begin(), w.end(), wishHz) != w.end()) return wishHz;
    uint32_t best = w.front();
    for (uint32_t x : w) if (x <= rateHz * (1 + 1e-9)) best = x;
    return best;
}

TuneRegs tuneRegs(double hz, uint32_t filterHz, Flavour fl) {
    TuneRegs t;
    const BandPlan& b = bandFor(hz, fl);
    t.band = b.band;
    t.loDiv = b.loDiv;
    const uint64_t offset = b.upconvert ? kAmShiftHz : 0;
    const uint64_t rf = (uint64_t)std::llround(std::max(hz, 0.0)) + offset;
    const uint64_t vco = rf * (uint64_t)b.loDiv;
    const uint64_t rem = vco % kSynthRef;
    t.n = (uint32_t)(vco / kSynthRef);
    // The synthesiser makes 96 MHz x (n + (frac + afc / 4096) / thresh) with 12-bit thresh, frac and afc. thresh: the step that makes the
    // frequency exact when it can (the ratio reduced), else the largest that fits 12 bits; afc takes up what frac / thresh leaves.
    uint64_t thresh = kSynthRef / (uint64_t)b.loDiv;
    const uint64_t g = gcd(thresh, rem / (uint64_t)b.loDiv);
    if (g) thresh /= g;
    const uint64_t a = (thresh + 4094) / 4095;
    thresh = std::max<uint64_t>(1, (thresh + a / 2) / a);
    const uint64_t units = rem * thresh * 4096 / kSynthRef;   // < thresh x 4096
    t.thresh = (uint32_t)thresh;
    t.frac = (uint32_t)(units >> 12);
    t.afc = (uint32_t)(units & 4095);
    t.tunedHz = (double)kSynthRef * (t.n + (double)units / (thresh * 4096.0)) / b.loDiv - (double)offset;

    const auto& w = filterWidths();
    const uint32_t bwCode = (uint32_t)(std::find(w.begin(), w.end(), filterFor(1e12, filterHz)) - w.begin());
    uint32_t reg0 = 0;
    reg0 |= (uint32_t)b.mode << 4;
    if (b.band == Band::Am) reg0 |= (b.upconvert ? 1u : 0u) << 9 | 1u << 11;   // AM: up-converter, input port 2
    reg0 |= 1u << 10;           // RF synthesiser on
    reg0 |= 3u << 12;           // zero IF
    reg0 |= (bwCode & 7u) << 14;
    reg0 |= 2u << 17;           // 24 MHz crystal
    const uint32_t reg5 = 5u | (t.thresh & 0xfffu) << 4 | 0x28u << 16;
    const uint32_t reg2 = 2u | (t.frac & 0xfffu) << 4 | (t.n & 0x3fu) << 16;
    const uint32_t reg3 = 3u | (t.afc & 0xfffu) << 4;
    t.writes = {{8, b.gpioWord}, {9, 0x0e}, {9, reg3}, {9, reg0}, {9, reg5}, {9, reg2}};
    return t;
}

Gain gainFor(double totalDb, const std::string& lna, const std::string& mixer) {
    const int g = (int)std::lround(std::clamp(totalDb, 0.0, kMaxGainDb));
    Gain r;
    const bool lnaAuto = lna != "on" && lna != "off", mixAuto = mixer != "on" && mixer != "off";
    if (lnaAuto && mixAuto) {                 // IF first, then the mixer from 19 dB, the LNA from 43 dB (the most sensitive split)
        r.lna = g >= 43; r.mixer = g >= 19;
    } else if (lnaAuto) {
        r.mixer = mixer == "on";
        r.lna = g - (r.mixer ? 19 : 0) >= 24;
    } else if (mixAuto) {
        r.lna = lna == "on";
        r.mixer = g - (r.lna ? 24 : 0) >= 19;
    } else {
        r.lna = lna == "on"; r.mixer = mixer == "on";
    }
    r.ifDb = std::clamp(g - (r.lna ? 24 : 0) - (r.mixer ? 19 : 0), 0, 59);
    return r;
}

std::vector<Write> gainRegs(const Gain& g, Band band) {
    uint32_t reg1 = 1u;
    reg1 |= (uint32_t)(59 - std::clamp(g.ifDb, 0, 59)) << 4;   // IF gain reduction, 0..59 dB
    if (band == Band::Am) reg1 |= (g.lna ? 0u : 3u) << 10;      // AM inputs: the mix buffer (24 dB on port 2) stands in for the LNA
    else reg1 |= (g.lna ? 0u : 1u) << 13;                       // LNA gain reduction, 24 dB
    reg1 |= (g.mixer ? 0u : 1u) << 12;                          // mixer gain reduction, 19 dB
    reg1 |= 2u << 14;                                           // DC offset calibration: periodic
    const uint32_t reg6 = 6u | 0x1fu << 4 | 0x800u << 10;       // DC offset tracking times
    return {{9, reg1}, {9, reg6}};
}

std::vector<Write> initRegs() { return {{8, 0x006080}, {5, 0x00000c}, {0, 0x000200}, {2, 0x004801}, {8, 0x00f380}}; }
Write adcStopReg() { return {3, 0x010000}; }

size_t Unpacker::block(Format f, const uint8_t* b, cf32* out) {
    const uint8_t* s = b + 16;
    constexpr float k16 = 1.0f / 32768.0f;
    switch (f) {
    case Format::S14:   // 14-bit values in 16-bit little-endian words
        for (int i = 0; i < 252; i++, s += 4)
            out[i] = cf32((int16_t)(uint16_t)((s[0] << 2) | (s[1] << 10)) * k16, (int16_t)(uint16_t)((s[2] << 2) | (s[3] << 10)) * k16);
        return 252;
    case Format::S12:   // two 12-bit values in 3 bytes
        for (int i = 0; i < 336; i++, s += 3) {
            const int16_t re = (int16_t)(uint16_t)((s[0] << 4) | ((s[1] & 0x0f) << 12));
            const int16_t im = (int16_t)(uint16_t)((s[1] & 0xf0) | (s[2] << 8));
            out[i] = cf32(re * k16, im * k16);
        }
        return 336;
    case Format::S10: { // 6 groups of 16 x (8 10-bit values in 10 bytes) + a 32-bit word with a 2-bit range per 10 bytes; 24 bytes unused at the end
        size_t o = 0;
        for (int grp = 0; grp < 6; grp++) {
            const uint8_t* gp = s + grp * 164;
            const uint32_t range = le32(gp + 160);
            for (int k = 0; k < 16; k++) {
                const uint8_t* p = gp + k * 10;
                int16_t v[8];
                for (int h = 0; h < 2; h++) {
                    const uint8_t* q = p + 5 * h;
                    v[4 * h + 0] = (int16_t)(uint16_t)((q[0] << 6) | ((q[1] & 0x03) << 14));
                    v[4 * h + 1] = (int16_t)(uint16_t)(((q[1] & 0xfc) << 4) | ((q[2] & 0x0f) << 12));
                    v[4 * h + 2] = (int16_t)(uint16_t)(((q[2] & 0xf0) << 2) | ((q[3] & 0x3f) << 10));
                    v[4 * h + 3] = (int16_t)(uint16_t)((q[3] & 0xc0) | (q[4] << 8));
                }
                const unsigned code = (range >> (2 * k)) & 3u;
                const float sc = k16 * (code == 0 ? 0.25f : code == 1 ? 0.5f : 1.0f);
                for (int j = 0; j < 4; j++) out[o++] = cf32(v[2 * j] * sc, v[2 * j + 1] * sc);
            }
        }
        return o;
    }
    case Format::S8:
        for (int i = 0; i < 504; i++) out[i] = cf32((int8_t)s[2 * i] * (1.0f / 128), (int8_t)s[2 * i + 1] * (1.0f / 128));
        return 504;
    }
    return 0;
}

void Unpacker::take(const uint8_t* b, std::vector<cf32>& out) {
    const uint32_t ctr = le32(b);
    const uint32_t pairs = (uint32_t)formatInfo(f_).pairs;
    if (have_ && ctr != expect_) {
        const uint32_t gap = ctr - expect_;   // modulo 2^32: the counter wraps
        if (gap < 0x80000000u) lost_ += gap;   // a counter that went back: the stream restarted, nothing to count
    }
    have_ = true;
    expect_ = ctr + pairs;
    blocks_++;
    const size_t at = out.size();
    out.resize(at + pairs);
    out.resize(at + block(f_, b, out.data() + at));
}

size_t Unpacker::feed(const uint8_t* p, size_t n, std::vector<cf32>& out, bool packet) {
    const size_t before = out.size();
    if (packet) carry_.clear();
    if (!carry_.empty()) {
        const size_t need = kBlockBytes - carry_.size();
        const size_t m = std::min(need, n);
        carry_.insert(carry_.end(), p, p + m);
        p += m; n -= m;
        if (carry_.size() < kBlockBytes) return 0;
        take(carry_.data(), out);
        carry_.clear();
    }
    while (n >= kBlockBytes) { take(p, out); p += kBlockBytes; n -= kBlockBytes; }
    if (n && !packet) carry_.assign(p, p + n);
    return out.size() - before;
}

const std::vector<UsbModel>& usbModels() {
    // libmirisdr's list without the RSP1A / RSP2 ids (1df7:3000, 3010): those radios are not an MSi2500 + MSi001 pair driven this way
    static const std::vector<UsbModel> m = {
        {0x1df7, 0x2500, "SDRplay RSP1 / Mirics MSi2500", Flavour::Rsp1},
        {0x2040, 0xd300, "Hauppauge WinTV 133559 LF", Flavour::Generic},
        {0x07ca, 0x8591, "AverMedia A859 Pure DVBT", Flavour::Generic},
        {0x04bb, 0x0537, "IO-DATA GV-TV100", Flavour::Generic},
        {0x0511, 0x0037, "Logitec LDT-1S310U/J", Flavour::Generic},
    };
    return m;
}

const UsbModel* usbModel(uint16_t vid, uint16_t pid) {
    for (const auto& m : usbModels()) if (m.vid == vid && m.pid == pid) return &m;
    return nullptr;
}

} // namespace mirics
} // namespace dect2
