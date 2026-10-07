// DTMB tables (GB 20600-2006). Where each number comes from:
//  - frame geometry, PN generators and the PN phase schedule: GB 20600-2006 clause 4.6 and appendices D / E. The three generators (seed and
//    recurrence) and the phase schedule were taken from the dtmb-sdr project (github.com/ningzichun/dtmb-sdr, MIT licence); the test
//    tests/test_dtmb_defs.cpp checks the chips against the hexadecimal header vectors that project quotes from the standard.
//  - system information: the 32 bit codewords of the 11 profiles, the four leading zero chips and the complement for interleaver mode 2
//    are from the same project (c1.cpp); the positions of the 36 symbols in a C=3780 body and the carrier interleaver are its c3780.cpp
//    (standard appendix F). The codewords form a first order Reed-Muller coset: all 22 words are at distance 16 from each other except
//    complementary pairs; the test checks that.
//  - constellations: 4/16/64QAM reflected Gray labels per axis, the 32QAM cross constellation: dtmb-sdr (core.cpp, test_qam.cpp).
//  - 4QAM-NR: Nordstrom-Robinson table of appendix C (via dtmb-sdr tests/nr_appendix_c.txt); the test checks that it is a (16,256,6) code
//    with the weight distribution 1, 112, 30, 112, 1.
// None of this has been compared with the printed standard.
#include "dect2/dtmb_defs.h"
#include <cmath>
#include <cstring>

namespace dect2::dtmb {

namespace {

const HeaderInfo kHeaders[3] = {
    {420, 255, 82, 83, 225, 2.0, "PN420"},
    {595, 595, 0, 0, 216, 1.0, "PN595"},
    {945, 511, 217, 217, 200, 2.0, "PN945"},
};

struct PnGen { int len; const char* seed; int taps[4]; int nTaps; };
const PnGen kPnGen[3] = {
    {8, "10110000", {0, 2, 3, 7}, 4},       // PN420: 8-stage m-sequence, period 255
    {10, "0000000001", {0, 7, 0, 0}, 2},    // PN595: x^10 + x^3 + 1 truncated to 595 chips
    {9, "111110111", {0, 1, 2, 7}, 4},      // PN945: 9-stage m-sequence, period 511
};

// 32 bit codewords of the 11 profiles: NR 0.8; 4QAM 0.4 0.6 0.8; 16QAM 0.4 0.6 0.8; 32QAM 0.8; 64QAM 0.4 0.6 0.8
const char* const kSiWords[11] = {
    "01111000110010000010111011010101", "01110111110001110010000111011010", "00100010100100100111010010001111",
    "01001011111110110001110111100110", "00010001101000010100011110111100", "01111000001101110010111000101010",
    "00101101100111010111101110000000", "01110111001110000010000100100101", "00100010011011010111010001110000",
    "01000100000010110001001000010110", "00010001010111100100011101000011",
};

const uint8_t kNr[256] = {
    0x00, 0xFE, 0x97, 0x69, 0x2F, 0xD1, 0xCC, 0x32, 0x5D, 0xA3, 0xF0, 0x0E, 0x9A, 0x64, 0x43, 0xBD, 0xB9, 0x47, 0x5A,
    0xA4, 0xE2, 0x1C, 0x75, 0x8B, 0x36, 0xC8, 0xEF, 0x11, 0x85, 0x7B, 0x28, 0xD6, 0x73, 0x8D, 0xAA, 0x54, 0xB4, 0x4A,
    0x19, 0xE7, 0xC6, 0x38, 0x25, 0xDB, 0xE9, 0x17, 0x7E, 0x80, 0x6C, 0x92, 0xC1, 0x3F, 0xDF, 0x21, 0x06, 0xF8, 0x0B,
    0xF5, 0x9C, 0x62, 0x50, 0xAE, 0xB3, 0x4D, 0xE5, 0x1B, 0x3C, 0xC2, 0x56, 0xA8, 0xFB, 0x05, 0x6A, 0x94, 0x89, 0x77,
    0x31, 0xCF, 0xA6, 0x58, 0x8E, 0x70, 0x23, 0xDD, 0x49, 0xB7, 0x90, 0x6E, 0xD3, 0x2D, 0x44, 0xBA, 0xFC, 0x02, 0x1F,
    0xE1, 0xD8, 0x26, 0x4F, 0xB1, 0x83, 0x7D, 0x60, 0x9E, 0xBF, 0x41, 0x12, 0xEC, 0x0C, 0xF2, 0xD5, 0x2B, 0x15, 0xEB,
    0xF6, 0x08, 0x3A, 0xC4, 0xAD, 0x53, 0xA0, 0x5E, 0x79, 0x87, 0x67, 0x99, 0xCA, 0x34, 0xCB, 0x35, 0x66, 0x98, 0x78,
    0x86, 0xA1, 0x5F, 0xAC, 0x52, 0x3B, 0xC5, 0xF7, 0x09, 0x14, 0xEA, 0xD4, 0x2A, 0x0D, 0xF3, 0x13, 0xED, 0xBE, 0x40,
    0x61, 0x9F, 0x82, 0x7C, 0x4E, 0xB0, 0xD9, 0x27, 0x1E, 0xE0, 0xFD, 0x03, 0x45, 0xBB, 0xD2, 0x2C, 0x91, 0x6F, 0x48,
    0xB6, 0x22, 0xDC, 0x8F, 0x71, 0xA7, 0x59, 0x30, 0xCE, 0x88, 0x76, 0x6B, 0x95, 0xFA, 0x04, 0x57, 0xA9, 0x3D, 0xC3,
    0xE4, 0x1A, 0xB2, 0x4C, 0x51, 0xAF, 0x9D, 0x63, 0x0A, 0xF4, 0x07, 0xF9, 0xDE, 0x20, 0xC0, 0x3E, 0x6D, 0x93, 0x7F,
    0x81, 0xE8, 0x16, 0x24, 0xDA, 0xC7, 0x39, 0x18, 0xE6, 0xB5, 0x4B, 0xAB, 0x55, 0x72, 0x8C, 0x29, 0xD7, 0x84, 0x7A,
    0xEE, 0x10, 0x37, 0xC9, 0x74, 0x8A, 0xE3, 0x1D, 0x5B, 0xA5, 0xB8, 0x46, 0x42, 0xBC, 0x9B, 0x65, 0xF1, 0x0F, 0x5C,
    0xA2, 0xCD, 0x33, 0x2E, 0xD0, 0x96, 0x68, 0x01, 0xFF,
};

} // namespace

const HeaderInfo& headerInfo(Header h) { return kHeaders[(int)h]; }

const std::vector<int8_t>& pnChips(Header h) {
    static const std::array<std::vector<int8_t>, 3> table = [] {
        std::array<std::vector<int8_t>, 3> t;
        for (int k = 0; k < 3; k++) {
            const PnGen& g = kPnGen[k];
            const int n = kHeaders[k].length;
            std::vector<uint8_t> b((size_t)n);
            for (int i = 0; i < g.len; i++) b[(size_t)i] = (uint8_t)(g.seed[i] - '0');
            for (int i = g.len; i < n; i++) {
                uint8_t v = 0;
                for (int j = 0; j < g.nTaps; j++) v ^= b[(size_t)(i - g.len + g.taps[j])];
                b[(size_t)i] = v;
            }
            t[(size_t)k].resize((size_t)n);
            for (int i = 0; i < n; i++) t[(size_t)k][(size_t)i] = b[(size_t)i] ? -1 : 1;
        }
        return t;
    }();
    return table[(size_t)(int)h];
}

int pnPhase(Header h, int frame) {
    const HeaderInfo& hi = headerInfo(h);
    if (!hi.cyclic()) return 0;
    int i = frame % hi.framesPerSuper;
    const int turn = hi.framesPerSuper / 2;
    if (i > turn) i = 2 * turn - i;
    if (i == 0) return 0;
    return (i % 2) ? (i + 1) / 2 : hi.core - i / 2;
}

void pnHeader(Header h, int phase, int8_t* out) {
    const HeaderInfo& hi = headerInfo(h);
    const std::vector<int8_t>& c = pnChips(h);
    if (!hi.cyclic()) { std::memcpy(out, c.data(), c.size()); return; }
    for (int n = 0; n < hi.length; n++) out[n] = c[(size_t)((n + phase) % hi.core)];
}

bool profileValid(const Profile& p) {
    if ((p.map == Mapping::Qam4Nr || p.map == Mapping::Qam32) && p.rate != Rate::R08) return false;
    return true;
}

namespace {
int profileNumber(const Profile& p) {   // 0 NR, 1..3 4QAM, 4..6 16QAM, 7 32QAM, 8..10 64QAM
    switch (p.map) {
    case Mapping::Qam4Nr: return 0;
    case Mapping::Qam4: return 1 + (int)p.rate;
    case Mapping::Qam16: return 4 + (int)p.rate;
    case Mapping::Qam32: return 7;
    default: return 8 + (int)p.rate;
    }
}
} // namespace

int siIndex(const Profile& p) {
    if (!profileValid(p)) return -1;
    return 3 + 2 * profileNumber(p) + (p.mode2 ? 1 : 0);   // odd: interleaver mode 1, even: mode 2
}

bool profileFromSi(int index, Profile& p) {
    if (index < 3 || index > 24) return false;
    const int n = (index - 3) / 2;
    p = Profile();
    if (n == 0) { p.map = Mapping::Qam4Nr; p.rate = Rate::R08; }
    else if (n <= 3) { p.map = Mapping::Qam4; p.rate = (Rate)(n - 1); }
    else if (n <= 6) { p.map = Mapping::Qam16; p.rate = (Rate)(n - 4); }
    else if (n == 7) { p.map = Mapping::Qam32; p.rate = Rate::R08; }
    else { p.map = Mapping::Qam64; p.rate = (Rate)(n - 8); }
    p.mode2 = ((index - 3) % 2) == 1;
    return true;
}

void siChips(int index, uint8_t* chips) {
    const int n = (index - 3) / 2;
    const bool flip = (index % 2) == 0;   // even index: the codeword is complemented
    for (int s = 0; s < kSiSymbols; s++) {
        uint8_t v = 0;
        if (s >= 4) { v = (uint8_t)(kSiWords[n][s - 4] - '0'); if (flip) v ^= 1; }
        chips[s] = v;
    }
}

const std::array<int16_t, kSiSymbols>& siPositions() {
    static const std::array<int16_t, kSiSymbols> t = [] {
        std::array<int16_t, kSiSymbols> a{};
        for (int g = 0; g < 9; g++) {
            a[(size_t)(4 * g + 0)] = (int16_t)(420 * g);
            a[(size_t)(4 * g + 1)] = (int16_t)(420 * g + 140);
            a[(size_t)(4 * g + 2)] = (int16_t)(420 * g + 279);
            a[(size_t)(4 * g + 3)] = (int16_t)(420 * g + 419);
        }
        return a;
    }();
    return t;
}

const std::array<int16_t, kBody>& carrierMap() {
    static const std::array<int16_t, kBody> t = [] {
        std::array<int16_t, kBody> a{};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++)
            for (int l = 0; l < 2; l++) for (int m = 0; m < 2; m++) for (int n = 0; n < 5; n++) for (int o = 0; o < 7; o++) {
                const int phys = o * 540 + n * 108 + m * 54 + l * 27 + k * 9 + j * 3 + i;
                const int logi = i * 1260 + j * 420 + k * 140 + l * 70 + m * 35 + n * 7 + o;
                a[(size_t)logi] = (int16_t)phys;
            }
        return a;
    }();
    return t;
}

const char* mappingName(Mapping m) {
    switch (m) {
    case Mapping::Qam4Nr: return "4QAM-NR";
    case Mapping::Qam4: return "4QAM";
    case Mapping::Qam16: return "16QAM";
    case Mapping::Qam32: return "32QAM";
    default: return "64QAM";
    }
}
const char* rateName(Rate r) { return r == Rate::R04 ? "0.4" : r == Rate::R06 ? "0.6" : "0.8"; }
int bchBlocks(Rate r) { return r == Rate::R04 ? 4 : r == Rate::R06 ? 6 : 8; }
int framesPerGroup(Mapping m) { return (m == Mapping::Qam4Nr || m == Mapping::Qam32) ? 2 : 1; }
int codewordsPerGroup(Mapping m) {
    switch (m) {
    case Mapping::Qam16: return 2;
    case Mapping::Qam32: return 5;
    case Mapping::Qam64: return 3;
    default: return 1;
    }
}
int packetsPerFrame(const Profile& p) {
    return codewordsPerGroup(p.map) * payloadBits(p.rate) / kTsBits / framesPerGroup(p.map);
}
double netBitrate(Header h, const Profile& p) { return packetsPerFrame(p) * (double)kTsBits * kSymbolRate / frameLength(h); }

int qamCount(Mapping m) { return 1 << bitsPerSymbol(m == Mapping::Qam4Nr ? Mapping::Qam4 : m); }

const QamPoint* qamPoints(Mapping m) {
    static const std::array<std::vector<QamPoint>, 4> tab = [] {   // 4, 16, 32, 64 points
        std::array<std::vector<QamPoint>, 4> t;
        auto square = [](std::vector<QamPoint>& v, int axisBits, const double* levels, double norm) {
            const int L = 1 << axisBits;
            v.assign((size_t)L * (size_t)L, QamPoint{0, 0});
            for (int i = 0; i < L; i++) for (int q = 0; q < L; q++) {
                const int li = i ^ (i >> 1), lq = q ^ (q >> 1);   // reflected Gray label of the level number
                v[(size_t)(li | (lq << axisBits))] = QamPoint{(float)(levels[i] / norm), (float)(levels[q] / norm)};
            }
        };
        const double l4[2] = {-4.5, 4.5}, l16[4] = {-6, -2, 2, 6}, l64[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        square(t[0], 1, l4, std::sqrt(40.5));
        square(t[1], 2, l16, std::sqrt(40.0));
        square(t[3], 3, l64, std::sqrt(42.0));
        static const double p32[32][2] = {
            {-1.5, -1.5}, {-7.5, -4.5}, {1.5, -1.5}, {7.5, -4.5}, {-4.5, -1.5}, {-7.5, -1.5}, {4.5, -1.5}, {7.5, -1.5},
            {-1.5, 1.5}, {-7.5, 4.5}, {1.5, 1.5}, {7.5, 4.5}, {-4.5, 1.5}, {-7.5, 1.5}, {4.5, 1.5}, {7.5, 1.5},
            {-1.5, -4.5}, {-1.5, -7.5}, {1.5, -4.5}, {1.5, -7.5}, {-4.5, -4.5}, {-4.5, -7.5}, {4.5, -4.5}, {4.5, -7.5},
            {-1.5, 4.5}, {-1.5, 7.5}, {1.5, 4.5}, {1.5, 7.5}, {-4.5, 4.5}, {-4.5, 7.5}, {4.5, 4.5}, {4.5, 7.5}};
        t[2].resize(32);
        for (int i = 0; i < 32; i++) t[2][(size_t)i] = QamPoint{(float)(p32[i][0] / std::sqrt(45.0)), (float)(p32[i][1] / std::sqrt(45.0))};
        return t;
    }();
    switch (m) {
    case Mapping::Qam4Nr: case Mapping::Qam4: return tab[0].data();
    case Mapping::Qam16: return tab[1].data();
    case Mapping::Qam32: return tab[2].data();
    default: return tab[3].data();
    }
}

uint8_t nrParity(uint8_t x) { return kNr[x]; }

double srrcPulse(double t) {
    const double a = kRollOff, pi = 3.14159265358979323846;
    if (std::fabs(t) < 1e-9) return 1.0 - a + 4.0 * a / pi;
    if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-9)
        return a / std::sqrt(2.0) * ((1.0 + 2.0 / pi) * std::sin(pi / (4.0 * a)) + (1.0 - 2.0 / pi) * std::cos(pi / (4.0 * a)));
    return (std::sin(pi * t * (1.0 - a)) + 4.0 * a * t * std::cos(pi * t * (1.0 + a))) / (pi * t * (1.0 - (4.0 * a * t) * (4.0 * a * t)));
}

} // namespace dect2::dtmb
