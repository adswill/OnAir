// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum explorer maths, shared by the app and the tests: frequency text ("1.2G", "433.92M", "145500k"), LNB / converter offsets,
// a noise floor that may be tilted, measurements of a region (occupied bandwidth at -3 and -26 dB, channel power, peak, SNR, symbol
// rate of a flat-topped carrier), signal detection, and the panorama a sweep stitches together.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace specx {

// "1.2G", "433.92M", "433.92 MHz", "145500k", "100" (no unit: MHz), "2,4 GHz", "915000000 Hz". false when it is not a frequency.
bool parseFreq(const std::string& text, double& hz);
std::string freqText(double hz);   // "433.920 000 MHz" style: GHz / MHz / kHz with enough digits

// A converter in front of the radio. kind 0: down-converter, oscillator below (radio = RF - LO: Ku LNBs, QO-100);
// 1: up-converter (radio = RF + LO: HF upconverters); 2: down-converter, oscillator above, spectrum inverted (radio = LO - RF: C band).
struct Lnb {
    bool on = false;
    double loHz = 9750e6;
    int kind = 0;
    double radioHz(double rfHz) const { return !on ? rfHz : kind == 1 ? rfHz + loHz : kind == 2 ? loHz - rfHz : rfHz - loHz; }
    double rfHz(double radioHz) const { return !on ? radioHz : kind == 1 ? radioHz - loHz : kind == 2 ? loHz - radioHz : radioHz + loHz; }
    bool inverted() const { return on && kind == 2; }
};
struct LnbPreset { const char* name; double loMhz; int kind; };
const LnbPreset* lnbPresets(int& count);   // Universal low / high, QO-100, C band, upconverter +125 MHz

// One spectrum: n bins of dBFS, bin k at f0Hz + k * binHz. enbw: the window's noise bandwidth in bins (channel power divides by it).
struct SpecView {
    const float* db = nullptr;
    int n = 0;
    double f0Hz = 0, binHz = 1, enbw = 1;
    double freq(double bin) const { return f0Hz + bin * binHz; }
    double bin(double hz) const { return (hz - f0Hz) / binHz; }
};

// The noise floor as a straight line in dB (a + b * bin): a fit that leaves out what stands above it, so carriers and a tilt are fine.
struct Floor { double a = -200, b = 0; double at(double bin) const { return a + b * bin; } };
Floor fitFloor(const SpecView& v);

struct Measure {
    bool valid = false;
    double loHz = 0, hiHz = 0;           // the region
    double centreHz = 0;                 // middle of the -3 dB band
    double bw3Hz = 0, bw26Hz = 0;        // occupied bandwidth at -3 and -26 dB below the top
    bool bw26AtFloor = false;            // -26 dB lies under the floor: bw26Hz is where the signal meets the floor (+3 dB)
    double bw20Hz = 0;                   // the same at -20 dB
    bool bw20AtFloor = false;
    double obwHz = 0, obwCentreHz = 0;   // 99 % of the power over the floor lies in this width, centred here (balanced for two-humped FSK)
    double powerDb = -200;               // channel power of the region, dBFS
    double peakDb = -200, peakHz = 0;    // the strongest bin
    double topDb = -200;                 // mean level across the -3 dB band
    double floorDb = -200;               // noise floor under the centre (per bin)
    double snrDb = 0;                    // topDb - floorDb
    bool flatTop = false;                // a flat-topped carrier (digital, root-raised-cosine shaped)
    double rollOff = 0;                  // the roll-off nearest bw26 / bw3 - 1 (0.20, 0.25 or 0.35), flat tops only
    double symRate[3] = {0, 0, 0};       // bw26 / (1 + rolloff) for rolloff 0.20, 0.25, 0.35
    double symRateBest = 0;              // the symbol rate to use: the -3 dB width (the half-power width of a root-raised-cosine is Rs)
    bool looksDvbs = false;              // flat-topped, 100 kHz .. 72 MHz wide, 4 dB over the floor
};
extern const double kRollOffs[3];        // 0.20, 0.25, 0.35
Measure measure(const SpecView& v, double loHz, double hiHz, const Floor* floor = nullptr);

// Signals standing thrDb over the floor, at least minBins wide, each measured over its run and a margin.
std::vector<Measure> detect(const SpecView& v, double thrDb = 6, int minBins = 3);

// A sweep: the radio steps across [loHz, hiHz]; each step keeps the middle keepFrac of its spectrum (the edges roll off) and the
// panorama, with bins of binHz, takes those values (the newest wins). Bins not yet seen are -200.
std::vector<double> sweepCentres(double loHz, double hiHz, double spanHz, double keepFrac = 0.8);
struct Panorama {
    double loHz = 0, hiHz = 0, binHz = 1;
    std::vector<float> db;
    std::vector<unsigned char> seen;
    void init(double lo, double hi, int bins);
    void add(const SpecView& v, double keepFrac = 0.8);
    bool complete() const;
    SpecView view(double enbw = 1) const { SpecView s; s.db = db.data(); s.n = (int)db.size(); s.f0Hz = loHz + binHz / 2; s.binHz = binHz; s.enbw = enbw; return s; }
};

} // namespace specx
} // namespace dect2
