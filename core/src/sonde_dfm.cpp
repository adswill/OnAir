// Graw DFM-06, DFM-09, DFM-17: frame builder, symbol builder and decoder.
//
// Facts from rs1729/RS demod/mod/dfm09mod.c unless said otherwise:
//  - 2500 symbols/s, Manchester coded (pair 10 = bit 0, pair 01 = bit 1, "manchester2"; the DFM-06 uses the opposite pair, "manchester1"),
//    so 1250 bits/s. A frame is 280 bits = 560 symbols = 0.224 s: header 0x45CF (16 bits), 56 bits of configuration, two data blocks of 104 bits.
//    The raw header symbols are 10011010100110010101101001010101. Frames follow each other without a gap.
//  - The configuration block is 7 Hamming codewords, a data block 13. Hamming(8,4): the 4 data bits first, then 4 parity bits
//    (matrices G and H in the source). The codewords of a block are interleaved: bit j of codeword i is at position L*j+i of the block
//    (L = 7 or 13).
//  - Data block: 48 bits of payload and a 4-bit id (0..8). 9 ids per cycle, two blocks per frame, so a cycle takes 4.5 frames (about a
//    second). Position mode comes from id 0 (bits 16..23): 2 (standard), 3 (two solutions), 4 (with XDATA).
//    Mode 2: id 1 = satellite mask (32) + milliseconds (16), id 2 = latitude (int32, 1e-7 deg) + horizontal speed (int16, 0.01 m/s),
//    id 3 = longitude + direction (uint16, 0.01 deg), id 4 = height (int32, 0.01 m, ellipsoid) + vertical speed (int16), id 5 = geoid
//    difference (int16, 0.01 m). Modes 3 and 4: id 0 = milliseconds + mode + frame number + horizontal speed, id 1 = latitude + direction,
//    id 2 = longitude + vertical speed, id 3 = height (mean sea level). Id 8 = year (12), month (4), day (5), hour (5), minute (6) and
//    the number of satellites (8 bits at offset 32). The same layout, scales and date fields are in einergehtnochrein/ra-firmware
//    src/dfm/dfmgps.c (independent check). Id 0 also has the frame number (bits 24..31, seconds modulo 256).
//  - Configuration block: 4-bit channel number and 24 bits. Channels 0..4 are measurement values as 24-bit floats (4-bit exponent, 20-bit
//    mantissa, value = mantissa / 2^exponent; DFM-06 has 20-bit values with a zero low nibble). Temperature: R = (m0 - m3) / g with
//    g = m4 / Rf (Rf 220 k, 332 k for DFM-17), then a Steinhart-Hart polyfit of the Epcos B57540G0502 thermistor. Channel 5 (DFM-09/17)
//    holds the battery voltage in mV (16 bits), 6 the internal temperature. The serial number is in the last channel (0xA DFM-09,
//    0xB DFM-17, 0xC / 0xD with pressure sensor): a nibble 0xC, 16 bits of the number and a nibble 0 or 1 telling which half it is.
//    DFM-06: channel 5 is 0xA00000 (or 0xB00000) and channel 6 holds the serial number as 6 hex digits, seen twice in a row.
// Not in the sources, so left out: humidity and pressure (the DFM-09P / DFM-17P), burst/kill timers.
// Our generator has no real DFM to copy: which half of the serial number goes in which cycle, the content of the filler channels
// and of the satellite data ids is our choice (documented at the builders below).
#include "sonde_bits_dm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
using namespace sondebits;

// ---- Hamming(8,4) ----
uint8_t sondeHamming84Encode(uint8_t n) {
    const int d0 = (n >> 3) & 1, d1 = (n >> 2) & 1, d2 = (n >> 1) & 1, d3 = n & 1;
    const int p0 = d1 ^ d2 ^ d3, p1 = d0 ^ d2 ^ d3, p2 = d0 ^ d1 ^ d3, p3 = d0 ^ d1 ^ d2;
    return (uint8_t)((d0 << 7) | (d1 << 6) | (d2 << 5) | (d3 << 4) | (p0 << 3) | (p1 << 2) | (p2 << 1) | p3);
}

int sondeHamming84Decode(uint8_t code, uint8_t& nibble) {
    static const uint8_t H[4][8] = {{0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 0, 0, 1, 0}, {1, 1, 1, 0, 0, 0, 0, 1}};
    static const uint8_t He[8] = {0x7, 0xB, 0xD, 0xE, 0x8, 0x4, 0x2, 0x1};   // the columns of H: syndrome of a single error
    int c[8];
    for (int j = 0; j < 8; j++) c[j] = (code >> (7 - j)) & 1;
    int syn = 0;
    for (int i = 0; i < 4; i++) {
        int s = 0;
        for (int j = 0; j < 8; j++) s ^= H[i][j] & c[j];
        syn = (syn << 1) | s;
    }
    int ret = 0;
    if (syn) {
        ret = -1;
        for (int j = 0; j < 8; j++) if (He[j] == syn) { c[j] ^= 1; ret = 1; break; }
    }
    nibble = (uint8_t)((c[0] << 3) | (c[1] << 2) | (c[2] << 1) | c[3]);
    return ret;
}

namespace {

constexpr uint16_t kHeader = 0x45CF;
constexpr int kFrameBits = 280;
constexpr int kFrameSyms = 560;
constexpr char kRawHeader[33] = "10011010100110010101101001010101";
constexpr int kConfAt = 16, kDat1At = 72, kDat2At = 176;

// ---- bit helpers ----
void putBits(std::vector<uint8_t>& b, int at, int n, uint64_t v) {
    for (int i = 0; i < n; i++) b[(size_t)at + i] = (uint8_t)((v >> (n - 1 - i)) & 1);
}
uint32_t getBits(const uint8_t* b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[i] & 1);
    return v;
}

// ---- measurement floats ----
float fl24(int d) { return (float)(d & 0xFFFFF) / (float)(1 << ((d >> 20) & 0xF)); }

// Thermistor polyfit of dfm09mod.c get_Temp (Epcos B57540G0502)
const double kDfmPoly[4] = {1.09698417e-03, 2.39564629e-04, 2.48821437e-06, 5.84354921e-08};

// ---- the encoder ----
struct Variant {
    int id;              // 6, 9, 17
    int snCh;            // channel that carries the serial number
    int nch;             // channels in a cycle
    int mode;            // position mode
    double rf, rs;
};
Variant variantOf(int v) {
    if (v == 6) return {6, 6, 7, 2, 220e3, 10e3};
    if (v == 17) return {17, 0xB, 12, 3, 332e3, 20e3};
    return {9, 0xA, 11, 2, 220e3, 20e3};
}

uint32_t dfmSerialNumber(const std::string& s, int variant) {
    if (variant == 6) {
        if (!s.empty() && s.size() <= 6) {
            char* e = nullptr;
            const unsigned long v = std::strtoul(s.c_str(), &e, 16);
            if (e && *e == 0 && v != 0) return (uint32_t)v;
        }
    } else {
        if (!s.empty() && s.size() <= 9) {
            char* e = nullptr;
            const unsigned long v = std::strtoul(s.c_str(), &e, 10);
            if (e && *e == 0 && v != 0) return (uint32_t)v;
        }
    }
    uint32_t h = 2166136261u;
    for (char ch : s) { h ^= (uint8_t)ch; h *= 16777619u; }
    if (variant == 6) return 0x100000u + (h & 0x7FFFFF);        // six digits, never zero, hex digits 0..7 at the top
    return 20000000u + (h % 9000000u);
}

void unixToCivil(double u, int& y, int& mo, int& d, int& hh, int& mi, double& ss) {
    int64_t days = (int64_t)std::floor(u / 86400.0);
    double rem = u - (double)days * 86400.0;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (mo <= 2));
    hh = (int)(rem / 3600.0); rem -= hh * 3600.0;
    mi = (int)(rem / 60.0); rem -= mi * 60.0;
    ss = rem;
}

// 24-bit float with exponent 0 (DFM-09/17) or the DFM-06 form (value / 16 in the mantissa)
uint32_t packMeas(double f, bool dfm06) {
    if (f < 0) f = 0;
    if (dfm06) return (uint32_t)std::min(65535.0, std::round(f / 16.0)) << 4;
    return (uint32_t)std::min(1048575.0, std::round(f));
}

// the 28 bits of configuration channel ch in this cycle
void confBits(const Variant& v, const SondeTruth& t, int ch, int cycle, std::vector<uint8_t>& b /*28*/) {
    uint32_t val = 0;
    const bool is6 = v.id == 6;
    const double g = 2.0;                                      // the gain of the sensor amplifier: our choice
    double R = steinhartResistance(std::max(-110.0, std::min(60.0, t.tempC)), kDfmPoly);
    if (ch == 0) val = packMeas(g * (R + v.rs), is6);
    else if (ch == 1) val = 0x012340;
    else if (ch == 2) val = 0x023450;
    else if (ch == 3) val = packMeas(g * v.rs, is6);
    else if (ch == 4) val = packMeas(g * v.rf, is6);
    else if (is6) {
        if (ch == 5) val = 0xA00000;                           // "nothing here" channel that tells the DFM-06 apart
        else {                                                  // ch 6: serial number, 6 hex digits
            putBits(b, 0, 4, (uint64_t)ch);
            putBits(b, 4, 24, dfmSerialNumber(t.serial, 6) & 0xFFFFFF);
            return;
        }
    } else if (ch == 5) {
        const double mv = t.batteryV > 0.5 ? t.batteryV * 1000.0 : 3000.0;
        val = (uint32_t)std::min(65535.0, std::round(mv)) << 4;
    } else if (ch == 6) val = (uint32_t)2500 << 4;             // internal temperature 25.00 C
    else if (ch == 7) val = (((uint32_t)std::floor(t.unixTime) & 0xFFFF) | 1u) << 4;  // a seconds counter
    else if (ch == v.snCh) {
        const uint32_t sn = dfmSerialNumber(t.serial, v.id);
        // 0xsCaaaab: channel, 0xC, 16 bits of the number, 0 / 1 for the upper / lower half (we alternate between cycles)
        const int hl = cycle & 1;
        const uint32_t half = hl == 0 ? (sn >> 16) : (sn & 0xFFFF);
        putBits(b, 0, 4, (uint64_t)ch);
        putBits(b, 4, 4, 0xC);
        putBits(b, 8, 16, half);
        putBits(b, 24, 4, (uint64_t)hl);
        return;
    } else val = 0x055555 + (uint32_t)ch * 0x1111;            // channels 8, 9, ...: filler, never zero
    putBits(b, 0, 4, (uint64_t)ch);
    putBits(b, 4, 24, val & 0xFFFFFF);
}

// the 52 bits of data block `id`
void datBits(const Variant& v, const SondeTruth& t, int id, std::vector<uint8_t>& b /*52*/) {
    const double unixT = t.unixTime > 0 ? t.unixTime : 1767225600.0;
    int y, mo, d, hh, mi; double ss;
    unixToCivil(unixT, y, mo, d, hh, mi, ss);
    const uint32_t msek = (uint32_t)std::min(59999.0, std::round(ss * 1000.0));
    const int32_t lat = (int32_t)std::llround(t.lat * 1e7), lon = (int32_t)std::llround(t.lon * 1e7);
    auto s16 = [](double x) { return (uint32_t)(uint16_t)(int16_t)std::max(-32768.0, std::min(32767.0, std::round(x))); };
    const uint32_t hv = s16(t.hSpeed * 100.0), vv = s16(t.vSpeed * 100.0);
    const uint32_t dir = (uint32_t)std::min(65535.0, std::round(std::fmod(std::fmod(t.headingDeg, 360.0) + 360.0, 360.0) * 100.0));
    const int frnr = (int)((int64_t)std::floor(unixT) & 0xFF);
    const double dMSL = -20.0;                                  // geoid difference of the example: a fixed value
    const int mode = v.mode;
    putBits(b, 48, 4, (uint64_t)id);
    if (id == 8) {
        putBits(b, 0, 12, (uint64_t)y); putBits(b, 12, 4, (uint64_t)mo); putBits(b, 16, 5, (uint64_t)d);
        putBits(b, 21, 5, (uint64_t)hh); putBits(b, 26, 6, (uint64_t)mi);
        putBits(b, 32, 8, (uint64_t)std::max(0, std::min(255, t.sats)));
        return;
    }
    if (id == 0) {
        putBits(b, 16, 8, (uint64_t)mode);
        putBits(b, 24, 8, (uint64_t)frnr);
        if (mode >= 3) { putBits(b, 0, 16, msek); putBits(b, 32, 16, hv); }
        return;
    }
    if (mode <= 2) {
        const uint32_t mask = t.sats >= 32 ? 0xFFFFFFFFu : ((1u << std::max(0, t.sats)) - 1u);
        switch (id) {
        case 1: putBits(b, 0, 32, mask); putBits(b, 32, 16, msek); break;
        case 2: putBits(b, 0, 32, (uint32_t)lat); putBits(b, 32, 16, hv); break;
        case 3: putBits(b, 0, 32, (uint32_t)lon); putBits(b, 32, 16, dir); break;
        case 4: putBits(b, 0, 32, (uint32_t)(int32_t)std::llround((t.altM - dMSL) * 100.0)); putBits(b, 32, 16, vv); break;
        case 5: putBits(b, 0, 16, s16(dMSL * 100.0)); break;
        default: break;                                         // 6, 7: satellite data, zeros
        }
    } else {
        switch (id) {
        case 1: putBits(b, 0, 32, (uint32_t)lat); putBits(b, 32, 16, dir); break;
        case 2: putBits(b, 0, 32, (uint32_t)lon); putBits(b, 32, 16, vv); break;
        case 3: putBits(b, 0, 32, (uint32_t)(int32_t)std::llround(t.altM * 100.0)); break;
        case 5: putBits(b, 0, 32, (uint32_t)lat); putBits(b, 32, 16, hv); break;       // second solution: the same
        case 6: putBits(b, 0, 32, (uint32_t)lon); putBits(b, 32, 16, dir); break;
        case 7: putBits(b, 0, 32, (uint32_t)(int32_t)std::llround(t.altM * 100.0)); putBits(b, 32, 16, vv); break;
        default: break;                                         // 4: satellite data, zeros
        }
    }
}

// block of L Hamming codewords from n = 4L bits, interleaved
void hammingBlock(const std::vector<uint8_t>& bits, int L, uint8_t* out /*8L*/) {
    for (int i = 0; i < L; i++) {
        const uint8_t cw = sondeHamming84Encode((uint8_t)getBits(&bits[(size_t)i * 4], 4));
        for (int j = 0; j < 8; j++) out[L * j + i] = (cw >> (7 - j)) & 1;
    }
}

// ---- the decoder ----
struct DfmState {
    // configuration
    bool cfgchk24[9] = {};
    float meas24[9] = {};
    int nulCh = 0, maxCh = 0, snCh = 0, chXbit = 0;
    uint32_t chX[2] = {0, 0}, snX = 0, sn6 = 0, sn = 0;
    int sondeTyp = 0;                  // SNbit | channel
    int ptuOut = 0;
    bool cfgchk = false;
    char sensor = 'T';
    double rf = 220e3;
    double status0 = 0;                // battery, V
    std::string serial;
    // data
    int posmode = -1, frnr = 0, nsv = -1;
    double sek = 0, lat = 0, lon = 0, alt = 0, horiV = 0, dir = 0, vertV = 0, dmsl = 0;
    bool haveDmsl = false;
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0;
    // which blocks arrived, and when (in frames)
    double ts[9] = {};
    bool have[9] = {};
};

class DfmDecoder : public SondeBitDecoder {
public:
    const char* type() const override { return "DFM"; }
    double symbolRate() const override { return 2500.0; }
    void push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) override;
    void reset() override { buf_.clear(); pend_.clear(); base_ = total_ = reg_ = goodEnd_ = 0; st_ = DfmState(); corrected_ = 0; firstT_ = -1; }
private:
    struct Cand { uint64_t start; int err; bool inv; };
    std::vector<uint8_t> buf_;
    uint64_t base_ = 0, total_ = 0, reg_ = 0, goodEnd_ = 0;
    std::vector<Cand> pend_;
    DfmState st_;
    int corrected_ = 0;
    double firstT_ = -1;
    bool inv_ = false;

    void service(double timeSec, std::vector<SondeFix>& out);
    bool frame(const Cand& c, double timeSec, std::vector<SondeFix>& out);
    void confOut(const uint8_t* b, int ec);
    int datOut(const uint8_t* b);
    bool makeFix(double timeSec, SondeFix& fx);
    std::string subtype() const;
};

uint32_t rawHeaderWord() {
    uint32_t w = 0;
    for (int i = 0; i < 32; i++) w = (w << 1) | (kRawHeader[i] == '1');
    return w;
}
const uint32_t kRawWord = rawHeaderWord();

void DfmDecoder::push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) {
    for (size_t k = 0; k < n; k++) {
        const uint8_t s = sym[k] & 1;
        buf_.push_back(s);
        reg_ = (reg_ << 1) | s;
        total_++;
        if (total_ < 32) continue;
        const int errN = __builtin_popcount((uint32_t)reg_ ^ kRawWord);
        const int errI = 32 - errN;
        const int err = std::min(errN, errI);
        if (err > 4) continue;
        const uint64_t start = total_ - 32;
        if (start < goodEnd_) continue;
        bool merged = false;
        for (auto& c : pend_) {
            const uint64_t d = c.start > start ? c.start - start : start - c.start;
            if (d <= 8) { if (err < c.err) { c.start = start; c.err = err; c.inv = errI < errN; } merged = true; break; }
        }
        if (!merged) pend_.push_back(Cand{start, err, errI < errN});
    }
    service(timeSec, out);
    uint64_t keep = total_ > 96 ? total_ - 96 : 0;
    for (const auto& c : pend_) keep = std::min(keep, c.start);
    if (keep > base_ + 8192) {
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)(keep - base_));
        base_ = keep;
    }
}

void DfmDecoder::service(double timeSec, std::vector<SondeFix>& out) {
    if (pend_.empty()) return;
    std::sort(pend_.begin(), pend_.end(), [](const Cand& a, const Cand& b) { return a.start < b.start; });
    std::vector<Cand> keepList;
    for (const auto& c : pend_) {
        if (c.start < goodEnd_) continue;
        if (total_ < c.start + (uint64_t)kFrameSyms) { keepList.push_back(c); continue; }
        if (frame(c, timeSec, out)) goodEnd_ = c.start + kFrameSyms;
    }
    pend_.swap(keepList);
}

std::string DfmDecoder::subtype() const {
    const bool dfm17_0xA = st_.sn >= 23000000u && inv_;
    switch (st_.sondeTyp & 0xF) {
    case 0x6: return "DFM-06";
    case 0x7: case 0x8: return st_.sn6 ? "DFM-06P" : "PS-15";
    case 0xA: return dfm17_0xA ? "DFM-17" : "DFM-09";
    case 0xB: return "DFM-17";
    case 0xC: return st_.sensor == 'P' ? "DFM-09P" : "DFM-17";
    case 0xD: return "DFM-17P";
    default: return "DFM";
    }
}

// One candidate: Manchester decoding, header check, de-interleaving, Hamming. true when the frame was usable.
bool DfmDecoder::frame(const Cand& c, double timeSec, std::vector<SondeFix>& out) {
    const uint64_t first = c.start;
    if (first < base_ || first + kFrameSyms > base_ + buf_.size()) return false;
    const size_t o = (size_t)(first - base_);
    uint8_t bits[kFrameBits];
    int violations = 0;
    for (int k = 0; k < kFrameBits; k++) {
        const uint8_t a = buf_[o + 2 * (size_t)k] ^ (c.inv ? 1 : 0), b = buf_[o + 2 * (size_t)k + 1] ^ (c.inv ? 1 : 0);
        violations += a == b;
        bits[k] = b;                                         // the second symbol of the pair is the bit
    }
    const uint32_t hd = getBits(bits, 16);
    const int hdErr = __builtin_popcount(hd ^ kHeader);
    if (hdErr > 3 || violations > 40) return false;
    inv_ = c.inv;

    const double ts = (double)first / (double)kFrameSyms;
    int ecn[3] = {0, 0, 0};
    bool good[3] = {true, true, true};
    uint8_t nib[3][52];
    const int at[3] = {kConfAt, kDat1At, kDat2At}, L[3] = {7, 13, 13};
    for (int p = 0; p < 3; p++) {
        // de-interleave: codeword i, bit j = frame bit [L*j + i]
        for (int i = 0; i < L[p]; i++) {
            uint8_t cw = 0;
            for (int j = 0; j < 8; j++) cw = (uint8_t)((cw << 1) | bits[at[p] + L[p] * j + i]);
            uint8_t nb;
            const int r = sondeHamming84Decode(cw, nb);
            if (r < 0) good[p] = false;
            if (r > 0) ecn[p]++;
            for (int j = 0; j < 4; j++) nib[p][i * 4 + j] = (nb >> (3 - j)) & 1;
        }
        // like rs1729 for its JSON output: more than 4 corrected codewords in a block is not trusted
        if (ecn[p] > 4) good[p] = false;
    }
    if (!good[0] || !good[1] || !good[2]) {
        SondeFix bad;                                       // one bad-frame report per frame that lost a block
        bad.type = "DFM"; bad.crcOk = false;
        out.push_back(bad);
    }
    if (!good[0] && !good[1] && !good[2]) return false;
    if (firstT_ < 0) firstT_ = timeSec;
    for (int p = 0; p < 3; p++) if (good[p]) corrected_ += ecn[p];
    if (good[0]) confOut(nib[0], ecn[0]);
    for (int p = 1; p < 3; p++) {
        if (!good[p]) continue;
        const int id = (int)getBits(nib[p] + 48, 4);
        if (id <= 8) { st_.ts[id] = ts; st_.have[id] = true; }
        const int fr = datOut(nib[p]);
        if (fr == 8) {
            SondeFix fx;
            if (makeFix(timeSec, fx)) out.push_back(fx);
        }
    }
    return true;
}

void DfmDecoder::confOut(const uint8_t* cb, int ec) {
    DfmState& s = st_;
    const int confId = (int)getBits(cb, 4);
    if (confId > 4 && getBits(cb + 8, 20) == 0) s.nulCh = (int)getBits(cb, 8);
    const bool dfm6typ = ((s.nulCh & 0xF0) == 0x50) && (s.nulCh & 0x0F);
    auto resetCfg = [&]() {
        for (int j = 0; j < 9; j++) s.cfgchk24[j] = false;
        s.cfgchk = false; s.ptuOut = 0; s.serial.clear();
    };
    if (dfm6typ) s.ptuOut = 6;
    if (dfm6typ && (s.sondeTyp & 0xF) > 6) { s.sondeTyp = 0; s.maxCh = confId; resetCfg(); }
    if (confId > 5 && confId > s.maxCh && ec == 0) {
        if (getBits(cb + 4, 4) == 0xC) s.maxCh = confId;
    }
    if (confId > 5 && (confId == (s.nulCh >> 4) + 1 || confId == s.maxCh)) {
        const int sn2 = (int)getBits(cb, 8);
        const int snCh = (sn2 >> 4) & 0xF;
        if ((s.nulCh & 0x58) == 0x58) {                                   // DFM-06: serial number as 6 hex digits
            const uint32_t sn6 = getBits(cb + 4, 24);
            if (sn6 == s.sn6 && sn6 != 0) {
                s.sondeTyp = 0x100 | snCh;
                s.ptuOut = 6;
                char b[16]; std::snprintf(b, sizeof b, "%06X", s.sn6);
                s.serial = b;
            } else { s.sondeTyp = 0; resetCfg(); }
            s.sn6 = sn6;
        } else if ((sn2 & 0xF) == 0xC || (sn2 & 0xF) == 0x0) {           // DFM-09 / 17: two halves in two cycles
            const uint32_t val = getBits(cb + 8, 20);
            const int hl = (int)(val & 0xF);
            if (hl < 2) {
                if (s.snCh != snCh) { s.chXbit = 0; s.chX[0] = s.chX[1] = 0; resetCfg(); }
                s.snCh = snCh;
                s.chX[hl] = (val >> 4) & 0xFFFF;
                s.chXbit |= 1 << hl;
                if (s.chXbit == 3) {
                    const uint32_t sn = (s.chX[0] << 16) | s.chX[1];
                    if (sn == s.snX || s.snX == 0) {
                        s.sondeTyp = 0x100 | snCh;
                        s.sn = sn;
                        s.ptuOut = 0;
                        if (snCh == 0xA || snCh == 0xB || snCh == 0xC || snCh == 0xD) s.ptuOut = snCh;
                        if (s.sn6 == 0 || (s.sondeTyp & 0xF) >= 0xA) {
                            char b[16]; std::snprintf(b, sizeof b, "%u", s.sn);
                            s.serial = b;
                        }
                    } else { s.sondeTyp = 0; resetCfg(); }
                    s.snX = sn;
                    s.chXbit = 0;
                }
            }
        }
    }
    const bool dfm17_0xA = s.sn >= 23000000u && inv_;
    if (confId <= 8 && ec == 0) {
        s.cfgchk24[confId] = true;
        s.meas24[confId] = fl24((int)getBits(cb + 4, 24));
        s.cfgchk = false;
        if (s.ptuOut >= 0x5) {
            s.cfgchk = s.cfgchk24[0] && s.cfgchk24[1] && s.cfgchk24[2] && s.cfgchk24[3] && s.cfgchk24[4] && s.cfgchk24[5];
        }
        if (s.ptuOut >= 0x7) s.cfgchk = s.cfgchk && s.cfgchk24[6] && s.cfgchk24[7];
        if (s.ptuOut >= 0x8) s.cfgchk = s.cfgchk && s.cfgchk24[8];
    }
    s.sensor = 'T';
    s.rf = 220e3;
    if (s.cfgchk) {
        if (s.ptuOut >= 0xD || (s.ptuOut >= 0xC && s.meas24[6] < 220e3)) s.sensor = 'P';
        if (((s.ptuOut == 0xB || s.ptuOut == 0xC) && s.sensor == 'T') || s.ptuOut >= 0xD) s.rf = 332e3;
        if (s.ptuOut == 0xA && s.sensor == 'T' && dfm17_0xA) s.rf = 332e3;
        if (s.ptuOut == 6 && (s.sondeTyp & 0xF) == 8) s.sensor = 'P';
        if (s.ptuOut >= 0xA) {
            const int ofs = s.sensor == 'P' ? 2 : 0;
            if (confId == 0x5 + ofs) s.status0 = getBits(cb + 8, 16) / 1000.0;      // battery voltage
        } else s.status0 = 0;
    }
}

int DfmDecoder::datOut(const uint8_t* b) {
    DfmState& s = st_;
    const int id = (int)getBits(b + 48, 4);
    if (id == 0) {
        const int mode = (int)getBits(b + 16, 8);
        s.posmode = (mode > 1 && mode < 5) ? mode : -1;
        s.frnr = (int)getBits(b + 24, 8);
    }
    auto i16 = [&](int at) { return (double)(int16_t)(getBits(b + at, 16) & 0xFFFF); };
    auto u16 = [&](int at) { return (double)(getBits(b + at, 16) & 0xFFFF); };
    auto i32 = [&](int at) { return (double)(int32_t)getBits(b + at, 32); };
    if (s.posmode <= 2) {
        if (id == 1) { s.sek = getBits(b + 32, 16) / 1000.0; }
        if (id == 2) { s.lat = i32(0) / 1e7; s.horiV = i16(32) / 100.0; }
        if (id == 3) { s.lon = i32(0) / 1e7; s.dir = u16(32) / 100.0; }
        if (id == 4) { s.alt = i32(0) / 100.0; s.vertV = i16(32) / 100.0; }
        if (id == 5) { s.dmsl = (double)(int16_t)(getBits(b, 16) & 0xFFFF) / 100.0; s.haveDmsl = true; }
    } else {                                      // modes 3 and 4 share the position layout
        if (id == 0) { s.sek = getBits(b, 16) / 1000.0; s.horiV = i16(32) / 100.0; }
        if (id == 1) { s.lat = i32(0) / 1e7; s.dir = u16(32) / 100.0; }
        if (id == 2) { s.lon = i32(0) / 1e7; s.vertV = i16(32) / 100.0; }
        if (id == 3) { s.alt = i32(0) / 100.0; }
    }
    if (id == 8) {
        s.y = (int)getBits(b, 12); s.mo = (int)getBits(b + 12, 4); s.d = (int)getBits(b + 16, 5);
        s.hh = (int)getBits(b + 21, 5); s.mi = (int)getBits(b + 26, 6);
        s.nsv = (int)getBits(b + 32, 8);
    }
    return id;
}

bool DfmDecoder::makeFix(double timeSec, SondeFix& fx) {
    DfmState& s = st_;
    // like rs1729: ids 0, 1, 2, 3, 4 and 8 within the last 6 frames
    const int need[6] = {0, 1, 2, 3, 4, 8};
    for (int id : need) if (!s.have[id] || s.ts[8] - s.ts[id] >= 6.0 || s.ts[8] - s.ts[id] < 0) return false;
    if (s.serial.empty() && firstT_ >= 0 && timeSec - firstT_ < 15.0) return false;      // the serial number takes a few seconds
    fx = SondeFix();
    fx.type = "DFM";
    fx.subtype = subtype();
    fx.serial = s.serial;
    fx.frame = s.frnr;
    fx.crcOk = true;
    fx.corrected = corrected_;
    corrected_ = 0;
    if (std::fabs(s.lat) <= 90 && std::fabs(s.lon) <= 180 && !(s.lat == 0 && s.lon == 0) && s.alt > -1000 && s.alt < 100000) {
        fx.hasPos = true; fx.lat = s.lat; fx.lon = s.lon;
        fx.altM = s.alt + ((s.posmode <= 2 && s.haveDmsl) ? s.dmsl : 0.0);       // mode 2 sends the ellipsoid height and the geoid difference
        fx.hasVel = true; fx.hSpeed = s.horiV; fx.headingDeg = s.dir; fx.vSpeed = s.vertV;
        fx.sats = s.nsv;
    }
    if (s.y >= 2000 && s.y < 2100 && s.mo >= 1 && s.mo <= 12 && s.d >= 1 && s.d <= 31 && s.hh < 24 && s.mi < 60 && s.sek < 61.0) {
        fx.hasTime = true; fx.unixTime = civilToUnix(s.y, s.mo, s.d, s.hh, s.mi, s.sek);
    }
    if (s.cfgchk && s.ptuOut) {
        const bool p = s.sensor == 'P';
        const double f = s.meas24[p ? 1 : 0], f1 = s.meas24[p ? 5 : 3], f2 = s.meas24[p ? 6 : 4];
        if (f * f1 * f2 > 0) {
            const double g = f2 / s.rf, R = (f - f1) / g;
            const double t = steinhartTempC(R, kDfmPoly);
            if (t > -120.0 && t < 80.0) { fx.hasTemp = true; fx.tempC = t; }
        }
    }
    if (s.ptuOut >= 0xA && s.status0 > 0.5 && s.status0 < 20.0) fx.batteryV = s.status0;
    return true;
}

} // namespace

std::unique_ptr<SondeBitDecoder> makeDfmDecoder() { return std::unique_ptr<SondeBitDecoder>(new DfmDecoder()); }

std::vector<uint8_t> dfmFrameBits(const SondeTruth& t, int variant) {
    const Variant v = variantOf(variant);
    const int f = std::max(0, t.frame);
    std::vector<uint8_t> bits(kFrameBits, 0);
    putBits(bits, 0, 16, kHeader);
    // configuration: channel f mod nch, cycle number f / nch
    std::vector<uint8_t> conf(28, 0);
    confBits(v, t, f % v.nch, f / v.nch, conf);
    // data blocks 2f and 2f+1 of the stream of ids 0..8
    std::vector<uint8_t> d1(52, 0), d2(52, 0);
    datBits(v, t, (2 * f) % 9, d1);
    datBits(v, t, (2 * f + 1) % 9, d2);
    uint8_t blk[104];
    hammingBlock(conf, 7, blk);
    for (int i = 0; i < 56; i++) bits[kConfAt + i] = blk[i];
    hammingBlock(d1, 13, blk);
    for (int i = 0; i < 104; i++) bits[kDat1At + i] = blk[i];
    hammingBlock(d2, 13, blk);
    for (int i = 0; i < 104; i++) bits[kDat2At + i] = blk[i];
    return bits;
}

std::vector<uint8_t> dfmSymbols(const SondeTruth& t, int variant) {
    const std::vector<uint8_t> bits = dfmFrameBits(t, variant);
    std::vector<uint8_t> sym;
    sym.reserve(kFrameSyms);
    const bool manch1 = variant == 6;                   // DFM-06: 1 -> 10, 0 -> 01
    for (uint8_t b : bits) {
        const uint8_t x = manch1 ? (uint8_t)(b ^ 1) : b;       // x = the second symbol
        sym.push_back((uint8_t)(x ^ 1)); sym.push_back(x);
    }
    return sym;
}

} // namespace dect2
