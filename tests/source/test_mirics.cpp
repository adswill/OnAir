// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Mirics MSi2500 + MSi001 driver (SDRplay RSP1, issue #503) without a radio: the register writes for every sample rate, frequency, filter
// and gain the driver can ask for, and the unpacking of every sample format (levels from full scale to one step, any split of the stream, a
// lost block, the sample counter wrapping). NOT tested here: that a real RSP1 answers these registers the way libmirisdr describes them.
#include "dect2/source/mirics.h"
#include "dect2/source/usb_fd.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using dect2::cf32;
using dect2::usbFdKindForId;
using namespace dect2::mirics;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---- encoders: what the bridge puts in a block (the inverse of the documented layouts)
static void header(uint8_t* b, uint32_t ctr) { b[0] = ctr & 0xff; b[1] = (ctr >> 8) & 0xff; b[2] = (ctr >> 16) & 0xff; b[3] = ctr >> 24; for (int i = 4; i < 16; i++) b[i] = 0x5a; }

// values: interleaved I, Q as integers of the format's width; range codes (S10 only, one per 10 bytes)
static std::vector<uint8_t> encode(Format f, uint32_t ctr, const std::vector<int>& v, const std::vector<int>& ranges = {}) {
    std::vector<uint8_t> b(kBlockBytes, 0xee);
    header(b.data(), ctr);
    uint8_t* s = b.data() + 16;
    switch (f) {
    case Format::S14:
        for (size_t i = 0; i < v.size(); i++) { const unsigned w = (unsigned)v[i] & 0x3fff; s[2 * i] = w & 0xff; s[2 * i + 1] = (uint8_t)(w >> 8); }
        break;
    case Format::S12:
        for (size_t i = 0; i < v.size() / 2; i++) {
            const unsigned a = (unsigned)v[2 * i] & 0xfff, q = (unsigned)v[2 * i + 1] & 0xfff;
            s[3 * i] = a & 0xff; s[3 * i + 1] = (uint8_t)((a >> 8) | ((q & 0xf) << 4)); s[3 * i + 2] = (uint8_t)(q >> 4);
        }
        break;
    case Format::S10:
        for (int g = 0; g < 6; g++) {
            uint8_t* gp = s + g * 164;
            uint32_t rw = 0;
            for (int k = 0; k < 16; k++) {
                rw |= (uint32_t)(ranges.empty() ? 3 : ranges[g * 16 + k]) << (2 * k);
                for (int h = 0; h < 2; h++) {
                    uint64_t bits = 0;
                    for (int j = 0; j < 4; j++) bits |= (uint64_t)((unsigned)v[(g * 16 + k) * 8 + h * 4 + j] & 0x3ff) << (10 * j);
                    for (int j = 0; j < 5; j++) gp[k * 10 + h * 5 + j] = (uint8_t)(bits >> (8 * j));
                }
            }
            gp[160] = rw & 0xff; gp[161] = (rw >> 8) & 0xff; gp[162] = (rw >> 16) & 0xff; gp[163] = rw >> 24;
        }
        break;
    case Format::S8:
        for (size_t i = 0; i < v.size(); i++) s[i] = (uint8_t)(int8_t)v[i];
        break;
    }
    return b;
}

static int bitsOf(Format f) { return f == Format::S14 ? 14 : f == Format::S12 ? 12 : f == Format::S10 ? 10 : 8; }

static void testUsbSetup() {
    uint16_t v, i;
    writeSetup({9, 0x123456}, v, i);
    CHECK(v == 0x5609 && i == 0x1234);
    writeSetup({8, 0xf380}, v, i);
    CHECK(v == 0x8008 && i == 0x00f3);
    CHECK(usbFdKindForId(0x1df7, 0x2500) == "mirics" && usbFdKindForId(0x2040, 0xd300) == "mirics" && usbFdKindForId(0x0511, 0x0037) == "mirics");
    CHECK(usbFdKindForId(0x1df7, 0x3000).empty() && usbFdKindForId(0x1df7, 0x3010).empty());   // RSP1A / RSP2: other chips, the SDRplay API
    CHECK(usbModel(0x1df7, 0x2500) && usbModel(0x1df7, 0x2500)->flavour == Flavour::Rsp1);
    CHECK(usbModel(0x2040, 0xd300) && usbModel(0x2040, 0xd300)->flavour == Flavour::Generic);
    for (const auto& m : usbModels()) CHECK(usbFdKindForId(m.vid, m.pid) == "mirics");   // the driver's table and the descriptor registry agree
}

static void testFormats() {
    CHECK(formatFor(2e6) == Format::S14 && formatFor(6.048e6) == Format::S14);
    CHECK(formatFor(7e6) == Format::S12 && formatFor(8.064e6) == Format::S12);
    CHECK(formatFor(9e6) == Format::S10 && formatFor(10e6) == Format::S8 && formatFor(12.096e6) == Format::S8 && formatFor(15e6) == Format::S8);
    CHECK(formatFor(2e6, "8") == Format::S8 && formatFor(2e6, "10") == Format::S10 && formatFor(2e6, "12") == Format::S12);
    CHECK(formatFor(8e6, "14") == Format::S12);   // too fine for the rate: Auto
    CHECK(formatFor(11e6, "10") == Format::S8);
    // the USB 2 budget: 24.576 MB/s of 1024-byte blocks
    for (Format f : {Format::S14, Format::S12, Format::S10, Format::S8}) {
        const FormatInfo& fi = formatInfo(f);
        CHECK(std::fabs(fi.maxRateHz - 24.576e6 / 1024 * fi.pairs) < 1);
    }
}

static void testRates() {
    // a known value: 2 Msps, 14-bit: divider 10, synthesiser 240 MHz = 5 x 48 MHz exactly
    const RateRegs r2 = rateRegs(2e6, Format::S14);
    CHECK(r2.div == 10 && r2.n == 5 && r2.frac21 == 0 && r2.reg4 == 0 && r2.reg3 == 0x11513);
    CHECK(std::fabs(r2.rateHz - 2e6) < 1e-6);
    CHECK((rateRegs(10e6, Format::S8).reg3 >> 12 & 0xf) == 0xd && (rateRegs(8e6, Format::S12).reg3 >> 12 & 0xf) == 5);
    // every rate the driver offers, in 1 kHz steps plus odd ones: the registers make it to within 1 Hz, and decode back to it
    std::vector<double> rates;
    for (double r = kMinRateHz; r <= kMaxRateHz; r += 997) rates.push_back(r);
    for (double r : {1.3e6, 1.536e6, 2.048e6, 2.4e6, 3.2e6, 6.048e6, 7.56e6, 8.0e6, 8.064e6, 9.216e6, 10e6, 12.096e6, 2e6 / 3.0, 20e6})
        rates.push_back(r);
    for (double r : rates) {
        const double want = std::min(std::max(r, kMinRateHz), kMaxRateHz);
        const RateRegs rr = rateRegs(r, formatFor(want));
        const double vco = want * rr.div * 12;
        CHECK(vco >= 202e6 - 1);
        CHECK(rr.div >= 4 && rr.div <= 14 && rr.div % 2 == 0 && rr.n >= 4 && rr.n <= 15);
        CHECK(std::fabs(rr.rateHz - want) < 1.0);
        // decode the registers as the bridge reads them
        const int div = (int)(((rr.reg3 >> 2) & 7) + 1) * 2;
        const uint32_t frac = rr.reg4 | ((rr.reg3 >> 7) & 1) << 20;
        const double back = (((rr.reg3 >> 8) & 0xf) + frac / 2097152.0) * 48e6 / (div * 12.0);
        CHECK(std::fabs(back - rr.rateHz) < 1e-3);
        CHECK((rr.reg3 & 3) == 3 && (rr.reg3 >> 16) == 1 && rr.reg4 <= 0xfffff);
    }
}

static void testFilters() {
    CHECK(filterFor(2.048e6) == 1536000 && filterFor(10e6) == 8000000 && filterFor(1.3e6) == 600000 && filterFor(6e6) == 6000000);
    CHECK(filterFor(5.5e6) == 5000000 && filterFor(0.1e6) == 200000);
    CHECK(filterFor(2e6, 5000000) == 5000000 && filterFor(2e6, 4000000) == 1536000);   // a listed width as asked, an unlisted one: Auto
}

// the frequency the tuner registers give (as the MSi001 reads them)
static double decodeTune(const TuneRegs& t, double& rfOffset) {
    uint32_t reg0 = 0, reg2 = 0, reg3 = 0, reg5 = 0;
    for (const auto& w : t.writes) if (w.reg == 9) {
        switch (w.value & 0xf) { case 0: reg0 = w.value; break; case 2: reg2 = w.value; break; case 3: reg3 = w.value; break; case 5: reg5 = w.value; break; }
    }
    const int mode = (reg0 >> 4) & 0x1f;
    const bool am = mode == 1;
    rfOffset = am && ((reg0 >> 9) & 1) ? 120e6 : 0;
    const uint32_t n = (reg2 >> 16) & 0x3f, frac = (reg2 >> 4) & 0xfff, thresh = (reg5 >> 4) & 0xfff, afc = (reg3 >> 4) & 0xfff;
    CHECK(((reg5 >> 16) & 0x3f) == 0x28);
    return 96e6 * (n + (frac + afc / 4096.0) / thresh) / t.loDiv - rfOffset;
}

static void testTuning() {
    // a known value: 100 MHz on an RSP1, 8 MHz filter: VHF, LO / 32, band word 0xf180
    {
        const TuneRegs t = tuneRegs(100e6, 8000000, Flavour::Rsp1);
        CHECK(t.writes.size() == 6);
        CHECK(t.writes[0].reg == 8 && t.writes[0].value == 0xf180);
        CHECK(t.writes[1].reg == 9 && t.writes[1].value == 0x0e);
        CHECK(t.writes[3].value == 0x5f420);   // VHF, synthesiser on, zero IF, 8 MHz, 24 MHz crystal
        CHECK(t.band == Band::Vhf && t.loDiv == 32 && t.n == 33 && t.thresh == 3 && t.frac == 1 && t.afc == 0);
        CHECK(std::fabs(t.tunedHz - 100e6) < 1e-6);
    }
    CHECK(tuneRegs(100e6, 8000000, Flavour::Generic).writes[0].value == 0xf380);
    CHECK(tuneRegs(110e6, 8000000, Flavour::Generic).band == Band::B3 && tuneRegs(110e6, 8000000, Flavour::Rsp1).band == Band::Vhf);
    CHECK(tuneRegs(7e6, 200000, Flavour::Rsp1).band == Band::Am && tuneRegs(7e6, 200000, Flavour::Rsp1).writes[0].value == 0xf580);
    CHECK(tuneRegs(1.5e9, 8000000, Flavour::Rsp1).band == Band::L && tuneRegs(2.3e9, 8000000, Flavour::Generic).band == Band::L);
    {   // the AM inputs: up-converter and port 2 bits
        const uint32_t reg0 = tuneRegs(1e6, 600000, Flavour::Rsp1).writes[3].value;
        CHECK(((reg0 >> 4) & 0x1f) == 1 && ((reg0 >> 9) & 1) == 1 && ((reg0 >> 11) & 1) == 1 && ((reg0 >> 14) & 7) == 2);
    }
    // every band, both front ends, every filter: the registers stay in their fields and give the frequency to within one synthesiser step
    std::mt19937 rng(503);
    std::vector<double> fs = {10e3, 100e3, 1e6, 7.074e6, 11.999999e6, 12e6, 29.999e6, 30e6, 49.999999e6, 50e6, 93.9e6, 107.999e6, 108e6, 111.9e6,
                              112e6, 249.9e6, 250e6, 258.99e6, 259e6, 260.99e6, 261e6, 329.9e6, 330e6, 403.9e6, 404e6, 522e6, 522.000001e6,
                              959.9e6, 960e6, 999.9e6, 1000e6, 1090e6, 1575.42e6, 1999.999999e6, 2000e6, 2400e6};
    std::uniform_real_distribution<double> u(10e3, 2e9);
    for (int i = 0; i < 4000; i++) fs.push_back(std::floor(u(rng)));
    for (Flavour fl : {Flavour::Rsp1, Flavour::Generic})
        for (uint32_t bw : filterWidths())
            for (double f : fs) {
                const TuneRegs t = tuneRegs(f, bw, fl);
                CHECK(t.n < 64 && t.thresh >= 1 && t.thresh <= 4095 && t.frac < t.thresh && t.afc < 4096);
                const double step = 96e6 / ((double)t.loDiv * t.thresh * 4096);
                CHECK(t.tunedHz <= std::round(f) + 1e-4 && std::round(f) - t.tunedHz < step + 1e-4);
                double off = 0;
                const double dec = decodeTune(t, off);
                if (std::fabs(dec - t.tunedHz) > 1e-3) { printf("  tune %.0f Hz: registers give %.3f, driver says %.3f\n", f, dec, t.tunedHz); failures++; }
                const uint32_t reg0 = t.writes[3].value;
                CHECK(filterWidths()[(reg0 >> 14) & 7] == bw && ((reg0 >> 12) & 3) == 3 && ((reg0 >> 17) & 7) == 2 && ((reg0 >> 10) & 1) == 1);
                CHECK(t.writes[0].value == bandFor(f, fl).gpioWord);
            }
}

static void testGain() {
    auto same = [](const Gain& g, bool lna, bool mixer, int ifDb) { return g.lna == lna && g.mixer == mixer && g.ifDb == ifDb; };
    CHECK(same(gainFor(0), false, false, 0));
    CHECK(same(gainFor(18), false, false, 18) && same(gainFor(19), false, true, 0) && same(gainFor(42), false, true, 23));
    CHECK(same(gainFor(43), true, true, 0) && same(gainFor(102), true, true, 59) && same(gainFor(150), true, true, 59) && same(gainFor(-5), false, false, 0));
    for (int g = 0; g <= 102; g++) CHECK(std::fabs(gainFor(g).totalDb() - g) < 1e-9);   // Auto reaches every whole dB
    CHECK(same(gainFor(102, "off"), false, true, 59));
    CHECK(same(gainFor(30, "on"), true, false, 6) && same(gainFor(50, "on"), true, true, 7));
    CHECK(same(gainFor(30, "auto", "off"), true, false, 6) && same(gainFor(10, "auto", "on"), false, true, 0));
    CHECK(same(gainFor(60, "off", "off"), false, false, 59));
    {
        const auto w = gainRegs(gainFor(102), Band::Vhf);
        CHECK(w.size() == 2 && w[0].reg == 9 && w[0].value == 0x8001 && w[1].value == (6u | 0x1fu << 4 | 0x800u << 10));
    }
    CHECK(gainRegs(gainFor(0), Band::Vhf)[0].value == 0xb3b1);
    {   // AM inputs: no LNA bit; the mix buffer reduces by 24 dB when the "LNA" is off
        const uint32_t off = gainRegs(gainFor(0), Band::Am)[0].value, on = gainRegs(gainFor(102), Band::Am)[0].value;
        CHECK(((off >> 10) & 3) == 3 && ((off >> 13) & 1) == 0 && ((on >> 10) & 3) == 0);
    }
}

// values of the format's width: full scale both ways, one step, zero, and noise at several levels
static std::vector<int> pattern(Format f, int seed, double level) {
    const int bits = bitsOf(f), hi = (1 << (bits - 1)) - 1, lo = -(1 << (bits - 1));
    const int n = formatInfo(f).pairs * 2;
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, level * hi);
    std::vector<int> v(n);
    for (int i = 0; i < n; i++) v[i] = std::max(lo, std::min(hi, (int)std::lround(g(rng))));
    v[0] = hi; v[1] = lo; v[2] = 1; v[3] = -1; v[4] = 0; v[5] = lo;
    return v;
}

static void testUnpack() {
    for (Format f : {Format::S14, Format::S12, Format::S10, Format::S8}) {
        const int bits = bitsOf(f);
        const double full = (double)(1 << (bits - 1));
        for (double level : {1.0, 0.3, 0.01}) {
            const auto v = pattern(f, (int)f * 10 + (int)(level * 100), level);
            std::vector<int> ranges;
            if (f == Format::S10) { std::mt19937 r(7); for (int i = 0; i < 96; i++) ranges.push_back((int)(r() % 4)); }
            const auto blk = encode(f, 1234, v, ranges);
            std::vector<cf32> out(1024);
            const size_t n = Unpacker::block(f, blk.data(), out.data());
            CHECK((int)n == formatInfo(f).pairs);
            double worst = 0;
            for (size_t i = 0; i < n; i++) {
                double sc = 1;
                if (f == Format::S10) { const int c = ranges[i / 4]; sc = c == 0 ? 0.25 : c == 1 ? 0.5 : 1; }
                worst = std::max(worst, std::fabs(out[i].real() - v[2 * i] / full * sc));
                worst = std::max(worst, std::fabs(out[i].imag() - v[2 * i + 1] / full * sc));
            }
            if (worst > 1e-6) { printf("  format %d level %g: worst error %g\n", bits, level, worst); failures++; }
        }
    }
    // a stream of blocks in any split gives the same samples; a part block at the end of an isochronous packet is dropped
    for (Format f : {Format::S14, Format::S12, Format::S10, Format::S8}) {
        const int pairs = formatInfo(f).pairs;
        std::vector<uint8_t> stream;
        const uint32_t start = 0xffffffffu - (uint32_t)pairs * 2;   // the counter wraps in the middle
        for (int b = 0; b < 8; b++) { const auto blk = encode(f, start + (uint32_t)(b * pairs), pattern(f, 100 + b, 0.5)); stream.insert(stream.end(), blk.begin(), blk.end()); }
        std::vector<cf32> whole, split, pkt;
        Unpacker a, b, c;
        a.reset(f); b.reset(f); c.reset(f);
        a.feed(stream.data(), stream.size(), whole);
        std::mt19937 rng(9);
        for (size_t p = 0; p < stream.size();) { const size_t m = std::min(stream.size() - p, (size_t)(1 + rng() % 1500)); b.feed(stream.data() + p, m, split); p += m; }
        CHECK(whole.size() == (size_t)pairs * 8 && split == whole);
        CHECK(a.lostSamples() == 0 && b.lostSamples() == 0 && a.blocks() == 8);   // the wrap is not a loss
        c.feed(stream.data(), 3 * 1024, pkt, true);
        c.feed(stream.data() + 3 * 1024, 2 * 1024 + 500, pkt, true);   // a short packet: its part block goes
        c.feed(stream.data() + 6 * 1024, 2 * 1024, pkt, true);        // block 5 is missing: counted as lost
        CHECK(pkt.size() == (size_t)pairs * 7 && c.lostSamples() == (uint64_t)pairs);
        CHECK(std::equal(pkt.begin(), pkt.begin() + pairs * 5, whole.begin()) && std::equal(pkt.begin() + pairs * 5, pkt.end(), whole.begin() + pairs * 6));
        // a counter that goes back (the stream restarted) is not a loss
        Unpacker d;
        d.reset(f);
        std::vector<cf32> o;
        d.feed(stream.data() + 4 * 1024, 1024, o, true);
        d.feed(stream.data(), 1024, o, true);
        CHECK(d.lostSamples() == 0);
    }
}

int main() {
    testUsbSetup();
    testFormats();
    testRates();
    testFilters();
    testTuning();
    testGain();
    testUnpack();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("mirics: all passed\n");
    return 0;
}
