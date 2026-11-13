// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator: the DVB transmission modes and which of them a link can carry (dvb_modes.h). The tables are those of the
// sheet Std_Tables of DVB_transmission_modes.xlsx, each from the ETSI table named beside it; the formulas are those of its mode sheets.
#include "dect2/tv/dvb_modes.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

namespace dect2::vgen {

namespace {

// "19/256" -> 0.0742...; the part after a space or a dash is not part of the fraction ("5/9-L", "3/5 (S2 code)")
double fraction(std::string_view s) {
    const size_t slash = s.find('/');
    if (slash == std::string_view::npos) return 0;
    int num = 0, den = 0;
    std::from_chars(s.data(), s.data() + slash, num);
    std::from_chars(s.data() + slash + 1, s.data() + s.size(), den);
    return den > 0 ? (double)num / den : 0;
}

double db(double x) { return 10 * std::log10(x); }

// ---- DVB-T: EN 300 744 V1.6.2 ----

// elementary period T (µs) per channel: cl. 4.4 Table 4 (8 MHz), Annex E Tables E.4 (7 MHz) and E.1 (6 MHz), Annex G Table G.1 (5 MHz)
struct TBw { const char* name; double t; };
const TBw kTBw[] = {{"5 MHz", 7.0 / 40}, {"6 MHz", 7.0 / 48}, {"7 MHz", 1.0 / 8}, {"8 MHz", 7.0 / 64}};
// Table 4 (FFT points, K carriers) and cl. 4.5.1 (data carriers: continual / scattered pilots and TPS excluded)
struct TFft { const char* name; int points, k, data; };
const TFft kTFft[] = {{"2K", 2048, 1705, 1512}, {"8K", 8192, 6817, 6048}};
const char* const kTGi[] = {"1/4", "1/8", "1/16", "1/32"};
const char* const kTMod[] = {"QPSK", "16-QAM", "64-QAM"};
const char* const kTCr[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
// Annex A Table A.1, Gaussian channel (8K, 8 MHz; applied to 2K and the other channels as the workbook does): [modulation][code rate]
const double kTCn[3][5] = {{3.5, 5.3, 6.3, 7.3, 7.9}, {9.3, 11.4, 12.6, 13.8, 14.4}, {13.8, 16.7, 18.2, 19.4, 20.2}};

// ---- DVB-T2: EN 302 755 V1.4.1, TS 102 831 V1.2.1 ----

// Table 65: T (µs); 1,7 MHz is 71/131 µs (its own system, DvbSystem::T2Narrow, as the workbook's own sheet), 10 MHz professional use only
const TBw kT2Bw[] = {{"1.7 MHz", 71.0 / 131}, {"5 MHz", 7.0 / 40}, {"6 MHz", 7.0 / 48}, {"7 MHz", 1.0 / 8}, {"8 MHz", 7.0 / 64}, {"10 MHz", 7.0 / 80}};
const char* const kT2Fft[] = {"1K", "2K", "4K", "8K", "16K", "32K"};
const int kT2Points[] = {1024, 2048, 4096, 8192, 16384, 32768};
// Table 66: Ktotal [FFT][normal, extended]; 0 = no extended carrier mode
const int kT2K[6][2] = {{853, 0}, {1705, 0}, {3409, 0}, {6817, 6913}, {13633, 13921}, {27265, 27841}};
const char* const kT2Carriers[] = {"Normal", "Extended"};
const char* const kT2Gi[] = {"1/128", "1/32", "1/16", "19/256", "1/8", "19/128", "1/4"};
const char* const kT2Pp[] = {"PP1", "PP2", "PP3", "PP4", "PP5", "PP6", "PP7", "PP8"};
const char* const kT2Mod[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
const char* const kT2Cr[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6"};
const char* const kT2Frame[] = {"64 800", "16 200"};
const int kT2Nldpc[] = {64800, 16200};
// Table 6(a) / 6(b): Kbch [frame][code rate]
const int kT2Kbch[2][6] = {{32208, 38688, 43040, 48408, 51648, 53840}, {7032, 9552, 10632, 11712, 12432, 13152}};
// TS 102 831 Tables 44 (64 800) and 45 (16 200): raw (C/N)0 on the Gaussian channel [frame][modulation][code rate]
const double kT2Cn[2][4][6] = {
    {{1.0, 2.3, 3.1, 4.1, 4.7, 5.2}, {6.0, 7.6, 8.9, 10.0, 10.8, 11.4}, {9.9, 12.0, 13.5, 15.1, 16.1, 16.8}, {13.2, 16.1, 17.8, 20.0, 21.3, 22.0}},
    {{0.7, 2.5, 3.4, 4.3, 4.9, 5.5}, {5.5, 7.9, 9.1, 10.3, 11.1, 11.7}, {9.2, 12.3, 13.8, 15.5, 16.4, 17.1}, {12.6, 16.9, 18.1, 20.3, 21.6, 22.4}},
};
// Per FFT, carrier mode and pilot pattern: Cdata (EN 302 755 Table 48, no tone reservation) and the pilot-boost correction ΔBP
// (TS 102 831 Table 46)
struct T2Pp { int fft, ext, pp; int cdata; double dbp; };
const T2Pp kT2PpTab[] = {
    {0, 0, 0, 764, 0.34}, {0, 0, 1, 768, 0.32}, {0, 0, 2, 798, 0.44}, {0, 0, 3, 804, 0.42}, {0, 0, 4, 818, 0.48},
    {1, 0, 0, 1522, 0.35}, {1, 0, 1, 1532, 0.33}, {1, 0, 2, 1596, 0.43}, {1, 0, 3, 1602, 0.42}, {1, 0, 4, 1632, 0.47}, {1, 0, 6, 1646, 0.29},
    {2, 0, 0, 3084, 0.39}, {2, 0, 1, 3092, 0.37}, {2, 0, 2, 3228, 0.47}, {2, 0, 3, 3234, 0.45}, {2, 0, 4, 3298, 0.51}, {2, 0, 6, 3328, 0.34},
    {3, 0, 0, 6208, 0.41}, {3, 0, 1, 6214, 0.39}, {3, 0, 2, 6494, 0.49}, {3, 0, 3, 6498, 0.48}, {3, 0, 4, 6634, 0.53}, {3, 0, 6, 6698, 0.37},
    {3, 0, 7, 6698, 0.37},
    {3, 1, 0, 6296, 0.41}, {3, 1, 1, 6298, 0.41}, {3, 1, 2, 6584, 0.50}, {3, 1, 3, 6588, 0.48}, {3, 1, 4, 6728, 0.52}, {3, 1, 6, 6788, 0.39},
    {3, 1, 7, 6788, 0.38},
    {4, 0, 0, 12418, 0.41}, {4, 0, 1, 12436, 0.38}, {4, 0, 2, 12988, 0.49}, {4, 0, 3, 13002, 0.47}, {4, 0, 4, 13272, 0.52},
    {4, 0, 5, 13288, 0.49}, {4, 0, 6, 13416, 0.33}, {4, 0, 7, 13406, 0.35},
    {4, 1, 0, 12678, 0.42}, {4, 1, 1, 12698, 0.38}, {4, 1, 2, 13262, 0.49}, {4, 1, 3, 13276, 0.47}, {4, 1, 4, 13552, 0.52},
    {4, 1, 5, 13568, 0.49}, {4, 1, 6, 13698, 0.34}, {4, 1, 7, 13688, 0.35},
    {5, 0, 1, 24886, 0.37}, {5, 0, 3, 26022, 0.45}, {5, 0, 5, 26592, 0.48}, {5, 0, 6, 26836, 0.33}, {5, 0, 7, 26812, 0.35},
    {5, 1, 1, 25412, 0.37}, {5, 1, 3, 26572, 0.45}, {5, 1, 5, 27152, 0.48}, {5, 1, 6, 27404, 0.33}, {5, 1, 7, 27376, 0.35},
};
// Table 58 (SISO): the pilot patterns allowed per FFT and guard interval, as bit masks of PP1..PP8 (bit 0 = PP1); 0 = combination not
// allowed. Extended carrier mode allows the same patterns as normal mode. [FFT][guard interval in kT2Gi order]
constexpr int pp(std::initializer_list<int> l) { int m = 0; for (int p : l) m |= 1 << (p - 1); return m; }
const int kT2Allowed[6][7] = {
    /* 1K  */ {0, 0, pp({4, 5}), 0, pp({2, 3}), 0, pp({1})},
    /* 2K  */ {0, pp({4, 7}), pp({4, 5}), 0, pp({2, 3}), 0, pp({1})},
    /* 4K  */ {0, pp({4, 7}), pp({4, 5}), 0, pp({2, 3}), 0, pp({1})},
    /* 8K  */ {pp({7}), pp({4, 7}), pp({4, 5, 8}), pp({4, 5, 8}), pp({2, 3, 8}), pp({2, 3, 8}), pp({1, 8})},
    /* 16K */ {pp({7}), pp({4, 6, 7}), pp({2, 4, 5, 8}), pp({2, 4, 5, 8}), pp({2, 3, 8}), pp({2, 3, 8}), pp({1, 8})},
    /* 32K */ {pp({7}), pp({4, 6}), pp({2, 4, 8}), pp({2, 4, 8}), pp({2, 8}), pp({2, 8}), 0},
};

// ---- DVB-S: EN 300 421 V1.1.2 ----

const char* const kSCr[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
const double kSEbN0[] = {4.5, 5.0, 5.5, 6.0, 6.4};   // Table 3: IF-loop Eb/N0 for QEF, per useful bit (incl. 0,8 dB modem margin)
constexpr double kSRollOff = 0.35;                   // cl. 4.4

// ---- DVB-S2: EN 302 307-1 V1.4.1; DVB-S2X: EN 302 307-2 V1.1.1 ----

const char* const kS2Mod[] = {"QPSK", "8PSK", "16APSK", "32APSK"};
const char* const kS2Cr[] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
// Table 5a (64 800) and 5b (16 200; 9/10 not defined): Kbch [frame][code rate]
const int kS2Kbch[2][11] = {{16008, 21408, 25728, 32208, 38688, 43040, 48408, 51648, 53840, 57472, 58192},
                            {3072, 5232, 6312, 7032, 9552, 10632, 11712, 12432, 13152, 14232, 0}};
// Table 13: ideal Es/N0 for QEF on AWGN, normal FECFRAME; NAN = no such MODCOD [modulation][code rate]
const double kS2EsN0[4][11] = {
    {-2.35, -1.24, -0.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18, 6.20, 6.42},
    {NAN, NAN, NAN, NAN, 5.50, 6.62, 7.91, NAN, 9.35, 10.69, 10.98},
    {NAN, NAN, NAN, NAN, NAN, 8.97, 10.21, 11.03, 11.61, 12.89, 13.13},
    {NAN, NAN, NAN, NAN, NAN, NAN, 12.73, 13.64, 14.28, 15.69, 16.05},
};
const char* const kS2Frame[] = {"Normal (64 800)", "Short (16 200)"};
const char* const kS2Pilots[] = {"Off", "On"};
const char* const kS2RollOff[] = {"0.35", "0.25", "0.20"};
const char* const kS2XRollOff[] = {"0.35", "0.25", "0.20", "0.15", "0.10", "0.05"};
const double kRollOff[] = {0.35, 0.25, 0.20, 0.15, 0.10, 0.05};   // the values of kS2XRollOff (kS2RollOff is its start)
const char* const kS2XFrame[] = {"Normal (64 800)", "Short (16 200)", "VL-SNR set 1", "VL-SNR set 2"};
const char* const kS2XPilots[] = {"Off", "On", "In the VL-SNR frame"};

// The S2X MODCODs (Tables 4, 6, 17a, 18a, 19a-19d) with the ideal Es/N0 of Tables 20a (normal), 20b (VL-SNR) and 20c (short).
// frame: index into kS2XFrame; plframe: the fixed VL-SNR frame length incl. header and pilots (0 = from the slots)
struct S2xModcod { const char* name; int frame; int bits; int kbch; int nldpc; int plframe; double esn0; };
const S2xModcod kS2X[] = {
    {"QPSK 13/45", 0, 2, 18528, 64800, 0, -2.03}, {"QPSK 9/20", 0, 2, 28968, 64800, 0, 0.22}, {"QPSK 11/20", 0, 2, 35448, 64800, 0, 1.45},
    {"8APSK 5/9-L", 0, 3, 35808, 64800, 0, 4.73}, {"8APSK 26/45-L", 0, 3, 37248, 64800, 0, 5.13},
    {"8PSK 23/36", 0, 3, 41208, 64800, 0, 6.12}, {"8PSK 25/36", 0, 3, 44808, 64800, 0, 7.02}, {"8PSK 13/18", 0, 3, 46608, 64800, 0, 7.49},
    {"16APSK 1/2-L", 0, 4, 32208, 64800, 0, 5.97}, {"16APSK 8/15-L", 0, 4, 34368, 64800, 0, 6.55},
    {"16APSK 5/9-L", 0, 4, 35808, 64800, 0, 6.84}, {"16APSK 26/45", 0, 4, 37248, 64800, 0, 7.51},
    {"16APSK 3/5", 0, 4, 38688, 64800, 0, 7.80}, {"16APSK 3/5-L", 0, 4, 38688, 64800, 0, 7.41},
    {"16APSK 28/45", 0, 4, 40128, 64800, 0, 8.10}, {"16APSK 23/36", 0, 4, 41208, 64800, 0, 8.38},
    {"16APSK 2/3-L", 0, 4, 43008, 64800, 0, 8.43}, {"16APSK 25/36", 0, 4, 44808, 64800, 0, 9.27},
    {"16APSK 13/18", 0, 4, 46608, 64800, 0, 9.71}, {"16APSK 7/9", 0, 4, 50208, 64800, 0, 10.65},
    {"16APSK 77/90", 0, 4, 55248, 64800, 0, 11.99},
    {"32APSK 2/3-L", 0, 5, 43040, 64800, 0, 11.10}, {"32APSK 32/45", 0, 5, 45888, 64800, 0, 11.75},
    {"32APSK 11/15", 0, 5, 47328, 64800, 0, 12.17}, {"32APSK 7/9", 0, 5, 50208, 64800, 0, 13.05},
    {"64APSK 32/45-L", 0, 6, 45888, 64800, 0, 13.98}, {"64APSK 11/15", 0, 6, 47328, 64800, 0, 14.81},
    {"64APSK 7/9", 0, 6, 50208, 64800, 0, 15.47}, {"64APSK 4/5", 0, 6, 51648, 64800, 0, 15.87}, {"64APSK 5/6", 0, 6, 53840, 64800, 0, 16.55},
    {"128APSK 3/4", 0, 7, 48408, 64800, 0, 17.73}, {"128APSK 7/9", 0, 7, 50208, 64800, 0, 18.53},
    {"256APSK 29/45-L", 0, 8, 41568, 64800, 0, 16.98}, {"256APSK 2/3-L", 0, 8, 43008, 64800, 0, 17.24},
    {"256APSK 31/45-L", 0, 8, 44448, 64800, 0, 18.10}, {"256APSK 32/45", 0, 8, 45888, 64800, 0, 18.59},
    {"256APSK 11/15-L", 0, 8, 47328, 64800, 0, 18.84}, {"256APSK 3/4", 0, 8, 48408, 64800, 0, 19.57},
    {"QPSK 11/45", 1, 2, 3792, 16200, 0, -2.50}, {"QPSK 4/15", 1, 2, 4152, 16200, 0, -2.24}, {"QPSK 14/45", 1, 2, 4872, 16200, 0, -1.46},
    {"QPSK 7/15", 1, 2, 7392, 16200, 0, 0.60}, {"QPSK 8/15", 1, 2, 8472, 16200, 0, 1.45}, {"QPSK 32/45", 1, 2, 11352, 16200, 0, 3.66},
    {"8PSK 7/15", 1, 3, 7392, 16200, 0, 3.83}, {"8PSK 8/15", 1, 3, 8472, 16200, 0, 4.71}, {"8PSK 26/45", 1, 3, 9192, 16200, 0, 5.52},
    {"8PSK 32/45", 1, 3, 11352, 16200, 0, 7.54},
    {"16APSK 7/15", 1, 4, 7392, 16200, 0, 5.99}, {"16APSK 8/15", 1, 4, 8472, 16200, 0, 6.93}, {"16APSK 26/45", 1, 4, 9192, 16200, 0, 7.66},
    {"16APSK 3/5", 1, 4, 9552, 16200, 0, 8.10}, {"16APSK 32/45", 1, 4, 11352, 16200, 0, 9.81},
    {"32APSK 2/3", 1, 5, 10632, 16200, 0, 11.41}, {"32APSK 32/45", 1, 5, 11352, 16200, 0, 12.18},
    {"QPSK 2/9", 2, 2, 14208, 0, 33282, -2.85}, {"BPSK 1/5", 2, 1, 5660, 0, 33282, -6.85}, {"BPSK 11/45", 2, 1, 7740, 0, 33282, -5.50},
    {"BPSK 1/3", 2, 1, 10620, 0, 33282, -4.00}, {"BPSK-S 1/5", 2, 1, 2512, 0, 33282, -9.90}, {"BPSK-S 11/45", 2, 1, 3792, 0, 33282, -8.30},
    {"BPSK 1/5", 3, 1, 3072, 0, 16686, -6.10}, {"BPSK 4/15", 3, 1, 4152, 0, 16686, -4.90}, {"BPSK 1/3", 3, 1, 5232, 0, 16686, -3.72},
};

// PLFRAME length in symbols: 90 per slot plus the 90-symbol header, and with pilots a 36-symbol block after every 16 slots
// (EN 302 307-1 cl. 5.5). Slots = ceil(Nldpc / (90 * bits)) gives Table 11 and S2X Table 16 (128APSK: 103 slots incl. padding).
int plframe(int nldpc, int bits, bool pilots) {
    const int slots = (nldpc + 90 * bits - 1) / (90 * bits);
    return 90 * (slots + 1) + (pilots ? 36 * ((slots - 1) / 16) : 0);
}

std::vector<std::string> strings(std::initializer_list<const char*> l) { return {l.begin(), l.end()}; }
template <size_t N> std::vector<std::string> strings(const char* const (&a)[N]) { return {a, a + N}; }

std::string_view modcodPrefix(std::string_view m) { return m.substr(0, m.find(' ')); }
std::string_view modcodRate(std::string_view m) { return m.substr(m.find(' ') + 1); }

// the S2X code rates of kS2X, by value, the "-L" variant after the plain one
std::vector<std::string> s2xRates() {
    std::set<std::tuple<double, bool, std::string>> r;
    for (const S2xModcod& m : kS2X) {
        const std::string_view rate = modcodRate(m.name);
        r.emplace(fraction(rate), rate.ends_with("-L"), std::string(rate));
    }
    std::vector<std::string> out;
    for (const auto& t : r) out.push_back(std::get<2>(t));
    return out;
}

const char* const kS2XConst[] = {"BPSK", "BPSK-S", "QPSK", "8PSK", "8APSK", "16APSK", "32APSK", "64APSK", "128APSK", "256APSK"};

int indexOf(const std::vector<std::string>& v, std::string_view s) {
    for (size_t i = 0; i < v.size(); i++) if (v[i] == s) return (int)i;
    return -1;
}

// the channels of kT2Bw a DVB-T2 system has: the 1,7 MHz one for T2Narrow, the others for T2
int t2FirstBw(DvbSystem s) { return s == DvbSystem::T2Narrow ? 0 : 1; }
int t2NumBw(DvbSystem s) { return s == DvbSystem::T2Narrow ? 1 : 5; }

std::vector<ModeParam> buildParams(DvbSystem s) {
    std::vector<std::string> tbw, t2bw;
    for (const TBw& b : kTBw) tbw.emplace_back(b.name);
    for (int b = t2FirstBw(s); b < t2FirstBw(s) + t2NumBw(s); b++) t2bw.emplace_back(kT2Bw[b].name);
    std::vector<std::string> tfft;
    for (const TFft& f : kTFft) tfft.emplace_back(f.name);
    switch (s) {
    case DvbSystem::T:
        return {{"Channel", tbw}, {"FFT", tfft}, {"Guard interval", strings(kTGi)}, {"Modulation", strings(kTMod)}, {"Code rate", strings(kTCr)}};
    case DvbSystem::T2:
    case DvbSystem::T2Narrow:
        return {{"Channel", t2bw}, {"FFT", strings(kT2Fft)}, {"Carriers", strings(kT2Carriers)}, {"Guard interval", strings(kT2Gi)},
                {"Pilot pattern", strings(kT2Pp)}, {"Modulation", strings(kT2Mod)}, {"Code rate", strings(kT2Cr)}, {"FEC frame", strings(kT2Frame)}};
    case DvbSystem::S:
        return {{"Code rate", strings(kSCr)}};
    case DvbSystem::S2:
        return {{"Modulation", strings(kS2Mod)}, {"Code rate", strings(kS2Cr)}, {"FEC frame", strings(kS2Frame)}, {"Pilots", strings(kS2Pilots)},
                {"Roll-off", strings(kS2RollOff)}};
    case DvbSystem::S2X:
        return {{"Constellation", strings(kS2XConst)}, {"Code rate", s2xRates()}, {"FEC frame", strings(kS2XFrame)}, {"Pilots", strings(kS2XPilots)},
                {"Roll-off", strings(kS2XRollOff)}};
    default: return {};
    }
}

DvbMode mode(DvbSystem s, std::initializer_list<int> values) {
    DvbMode m;
    m.system = s;
    int i = 0;
    for (int v : values) m.value[(size_t)i++] = (int8_t)v;
    return m;
}

void buildT(std::vector<DvbMode>& out) {
    for (int b = 0; b < 4; b++)
        for (int f = 0; f < 2; f++)
            for (int g = 0; g < 4; g++)
                for (int mo = 0; mo < 3; mo++)
                    for (int c = 0; c < 5; c++) {
                        DvbMode m = mode(DvbSystem::T, {b, f, g, mo, c});
                        const double tu = kTFft[f].points * kTBw[b].t, ts = tu * (1 + fraction(kTGi[g]));
                        m.bwMHz = kTFft[f].k / tu;
                        m.rateMbps = kTFft[f].data * 2.0 * (mo + 1) * fraction(kTCr[c]) * 188 / 204 / ts;
                        m.cnDb = kTCn[mo][c];
                        out.push_back(m);
                    }
}

void buildT2(std::vector<DvbMode>& out, DvbSystem sys) {
    std::map<std::tuple<int, int, int>, const T2Pp*> ppTab;
    for (const T2Pp& p : kT2PpTab) ppTab[{p.fft, p.ext, p.pp}] = &p;
    for (int b = 0; b < t2NumBw(sys); b++)
        for (int f = 0; f < 6; f++)
            for (int e = 0; e < 2; e++) {
                if (kT2K[f][e] == 0) continue;
                for (int g = 0; g < 7; g++)
                    for (int p = 0; p < 8; p++) {
                        if (!(kT2Allowed[f][g] & (1 << p))) continue;
                        const auto it = ppTab.find({f, e, p});
                        if (it == ppTab.end()) continue;
                        const double tu = kT2Points[f] * kT2Bw[t2FirstBw(sys) + b].t, ts = tu * (1 + fraction(kT2Gi[g]));
                        for (int mo = 0; mo < 4; mo++)
                            for (int c = 0; c < 6; c++)
                                for (int fr = 0; fr < 2; fr++) {
                                    DvbMode m = mode(sys, {b, f, e, g, p, mo, c, fr});
                                    m.bwMHz = kT2K[f][e] / tu;
                                    m.rateMbps = it->second->cdata * 2.0 * (mo + 1) * (kT2Kbch[fr][c] - 80) / kT2Nldpc[fr] / ts;
                                    m.cnDb = kT2Cn[fr][mo][c] + it->second->dbp;
                                    out.push_back(m);
                                }
                    }
            }
}

void buildS(std::vector<DvbMode>& out) {
    for (int c = 0; c < 5; c++) {
        DvbMode m = mode(DvbSystem::S, {c});
        m.bwMHz = 1 + kSRollOff;
        m.rateMbps = 2 * fraction(kSCr[c]) * 188 / 204;
        m.cnDb = kSEbN0[c] + db(m.rateMbps);   // C/N = Eb/N0 + 10 log10(Rb / Rs)
        out.push_back(m);
    }
}

void buildS2(std::vector<DvbMode>& out) {
    for (int fr = 0; fr < 2; fr++)
        for (int mo = 0; mo < 4; mo++)
            for (int c = 0; c < 11; c++) {
                if (std::isnan(kS2EsN0[mo][c]) || kS2Kbch[fr][c] == 0) continue;
                for (int p = 0; p < 2; p++)
                    for (int r = 0; r < 3; r++) {
                        DvbMode m = mode(DvbSystem::S2, {mo, c, fr, p, r});
                        m.bwMHz = 1 + kRollOff[r];
                        m.rateMbps = (kS2Kbch[fr][c] - 80) / (double)plframe(fr == 0 ? 64800 : 16200, mo + 2, p == 1);
                        m.cnDb = kS2EsN0[mo][c];
                        m.hasCn = fr == 0;   // Table 13 is for normal frames only; short frames need 0,2-0,3 dB more (cl. 6, not tabulated)
                        out.push_back(m);
                    }
            }
}

void buildS2X(std::vector<DvbMode>& out, const std::vector<ModeParam>& params) {
    for (const S2xModcod& mc : kS2X) {
        const int con = indexOf(params[0].values, modcodPrefix(mc.name)), rate = indexOf(params[1].values, modcodRate(mc.name));
        const bool vl = mc.plframe > 0;
        for (int p = vl ? 2 : 0; p < (vl ? 3 : 2); p++)
            for (int r = 0; r < 6; r++) {
                DvbMode m = mode(DvbSystem::S2X, {con, rate, mc.frame, p, r});
                m.bwMHz = 1 + kRollOff[r];
                m.rateMbps = (mc.kbch - 80) / (double)(vl ? mc.plframe : plframe(mc.nldpc, mc.bits, p == 1));
                m.cnDb = mc.esn0;
                out.push_back(m);
            }
    }
}

struct Tables {
    std::vector<ModeParam> params[kNumDvbSystems];
    std::vector<DvbMode> modes[kNumDvbSystems];
    Tables() {
        for (int i = 0; i < kNumDvbSystems; i++) params[i] = buildParams((DvbSystem)i);
        buildT(modes[(int)DvbSystem::T]);
        buildT2(modes[(int)DvbSystem::T2], DvbSystem::T2);
        buildT2(modes[(int)DvbSystem::T2Narrow], DvbSystem::T2Narrow);
        buildS(modes[(int)DvbSystem::S]);
        buildS2(modes[(int)DvbSystem::S2]);
        buildS2X(modes[(int)DvbSystem::S2X], params[(int)DvbSystem::S2X]);
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

} // namespace

const char* dvbSystemName(DvbSystem s) {
    switch (s) {
    case DvbSystem::T: return "DVB-T";
    case DvbSystem::T2: return "DVB-T2";
    case DvbSystem::T2Narrow: return "DVB-T2 1.7 MHz";
    case DvbSystem::S: return "DVB-S";
    case DvbSystem::S2: return "DVB-S2";
    case DvbSystem::S2X: return "DVB-S2X";
    default: return "";
    }
}

bool isSatellite(DvbSystem s) { return s == DvbSystem::S || s == DvbSystem::S2 || s == DvbSystem::S2X; }

const std::vector<ModeParam>& modeParams(DvbSystem s) { return tables().params[std::clamp((int)s, 0, kNumDvbSystems - 1)]; }
const std::vector<DvbMode>& dvbModes(DvbSystem s) { return tables().modes[std::clamp((int)s, 0, kNumDvbSystems - 1)]; }

std::string modeName(const DvbMode& m) {
    const std::vector<ModeParam>& p = modeParams(m.system);
    auto v = [&](int i) -> const std::string& { return p[(size_t)i].values[(size_t)m.value[(size_t)i]]; };
    auto mhz = [&](int i) { std::string b = v(i); b.erase(std::remove(b.begin(), b.end(), ' '), b.end()); return b; };
    switch (m.system) {
    case DvbSystem::T: return "T-" + mhz(0) + "-" + v(1) + "-GI" + v(2) + "-" + v(3) + "-" + v(4);
    case DvbSystem::T2:
    case DvbSystem::T2Narrow:
        return "T2-" + mhz(0) + "-" + v(1) + (m.value[2] ? "E" : "") + "-GI" + v(3) + "-" + v(4) + "-" + v(5) + "-" + v(6) + (m.value[7] ? "-S" : "-N");
    case DvbSystem::S: return "S-QPSK-" + v(0);
    case DvbSystem::S2: return "S2-" + v(0) + "-" + v(1) + (m.value[2] ? "-S" : "-N") + (m.value[3] ? "-POn" : "-POff") + "-RO" + v(4);
    case DvbSystem::S2X: {
        static const char* const frame[] = {"-N", "-S", "-VL1", "-VL2"};
        const char* pilots = m.value[3] == 0 ? "-POff" : m.value[3] == 1 ? "-POn" : "";
        return "S2X-" + v(0) + " " + v(1) + frame[m.value[2] & 3] + pilots + "-RO" + v(4);
    }
    default: return "";
    }
}

ModeFit fitMode(const DvbMode& m, const LinkInput& in) {
    ModeFit f;
    const double need = in.kbps / 1000.0;
    if (isSatellite(m.system)) {
        f.symbolRate = need / m.rateMbps;
        f.bwMHz = f.symbolRate * m.bwMHz;
        f.capacityMbps = in.bwMHz / m.bwMHz * m.rateMbps;
    } else {
        f.bwMHz = m.bwMHz;
        f.capacityMbps = m.rateMbps;
    }
    f.marginDb = in.cnDb - m.cnDb;
    f.ok = m.hasCn && f.bwMHz <= in.bwMHz * (1 + 1e-9) && f.capacityMbps * (1 + 1e-9) >= need && f.marginDb >= 0;
    return f;
}

ModeFilter openFilter() {
    ModeFilter f;
    f.fill(-1);
    return f;
}

bool matches(const DvbMode& m, const ModeFilter& f) {
    for (size_t i = 0; i < f.size(); i++)
        if (f[i] >= 0 && m.value[i] != f[i]) return false;
    return true;
}

bool betterChoice(const ModeChoice& a, const ModeChoice& b) {
    return std::make_tuple(a.fit.bwMHz, -a.fit.marginDb, -a.fit.capacityMbps) < std::make_tuple(b.fit.bwMHz, -b.fit.marginDb, -b.fit.capacityMbps);
}

std::vector<ModeChoice> availableModes(DvbSystem s, const ModeFilter& f, const LinkInput& in) {
    std::vector<ModeChoice> out;
    for (const DvbMode& m : dvbModes(s)) {
        if (!matches(m, f)) continue;
        const ModeFit fit = fitMode(m, in);
        if (fit.ok) out.push_back({&m, fit});
    }
    std::stable_sort(out.begin(), out.end(), betterChoice);
    return out;
}

} // namespace dect2::vgen
