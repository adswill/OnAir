// Shared M10/M20 pieces: checksum, line code, sync search, GPS time. See sonde_bits_dm.h for the sources.
#include "sonde_bits_dm.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace sondebits {

// ---- checksum ----
// A 16-bit linear checksum: the state is shifted a byte at a time with the feedback taps of rs1729's update_checkM10.
uint16_t m10Update(uint16_t c, uint8_t b) {
    const int ci = c;
    const int c1 = ci & 0xFF;
    int bb = b;
    bb = ((bb >> 1) | ((bb & 1) << 7)) & 0xFF;      // rotate right by one
    bb ^= (bb >> 2) & 0xFF;
    const int t6 = (ci & 1) ^ ((ci >> 2) & 1) ^ ((ci >> 4) & 1);
    const int t7 = ((ci >> 1) & 1) ^ ((ci >> 3) & 1) ^ ((ci >> 5) & 1);
    const int t = (ci & 0x3F) | (t6 << 6) | (t7 << 7);
    int s = (ci >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;
    const int c0 = bb ^ t ^ s;
    return (uint16_t)(((c1 << 8) | c0) & 0xFFFF);
}

uint16_t m10Check(const uint8_t* msg, size_t len) {
    uint16_t cs = 0;
    for (size_t i = 0; i < len; i++) cs = m10Update(cs, msg[i]);
    return cs;
}

uint16_t m10BlockCheck(int len, const uint8_t* msg) {
    uint16_t cs = m10Update(0, (uint8_t)(len & 0xFF));
    for (int i = 0; i < len - 2; i++) cs = m10Update(cs, msg[i]);
    return cs;
}

// ---- time ----
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double civilToUnix(int y, int m, int d, int hh, int mm, double ss) {
    return (double)daysFromCivil(y, m, d) * 86400.0 + hh * 3600.0 + mm * 60.0 + ss;
}

void unixToGps(double unixUtc, int leapS, int& week, double& towS) {
    const double g = unixUtc - kGpsEpochUnix + leapS;
    week = (int)std::floor(g / 604800.0);
    towS = g - (double)week * 604800.0;
}

double gpsToUnix(int week, double towS, int leapS) {
    return kGpsEpochUnix + (double)week * 604800.0 + towS - leapS;
}

int fixTrimbleWeek(int week) {
    // rs1729: a week above 4000 is garbage; a week below 1304 (2005) is a 10-bit roll-over of the Copernicus II. One roll-over
    // is added there; we add another while the date would still be before 2018 (no M10 flew then with such a number).
    if (week > 4000 || week < 0) return -1;
    if (week < 1304) week += 1024;
    while (week < 2000) week += 1024;
    return week;
}

// ---- thermistor ----
double steinhartTempC(double r, const double p[4]) {
    if (!(r > 0)) return -300.0;
    const double l = std::log(r);
    return 1.0 / (p[0] + l * (p[1] + l * (p[2] + l * p[3]))) - 273.15;
}

double steinhartResistance(double tempC, const double p[4]) {
    const double invT = 1.0 / (tempC + 273.15);
    double l = std::log(10e3);
    for (int i = 0; i < 60; i++) {
        const double f = p[0] + l * (p[1] + l * (p[2] + l * p[3])) - invT;
        const double df = p[1] + l * (2 * p[2] + l * 3 * p[3]);
        const double st = f / df;
        l -= st;
        if (std::fabs(st) < 1e-12) break;
    }
    return std::exp(l);
}

// rs1729 m10m20mod.c get_Temp: Shibaura PB5-41E thermistor, polyfit of the maker's table
const double kM10Poly[4] = {1.07303516e-03, 2.41296733e-04, 2.26744154e-06, 6.52855181e-08};
const double kM10Rs[3] = {12.1e3, 36.5e3, 475.0e3};     // series resistor per range
const double kM10Rp[3] = {1e20, 330.0e3, 2000.0e3};     // parallel resistor per range

bool m10NtcTemp(int scale, int adc, double& tempC) {
    if (scale < 0 || scale > 2 || adc <= 0 || adc >= 4095) return false;
    const double x = (4095.0 - adc) / adc;
    const double den = x - kM10Rs[scale] / kM10Rp[scale];
    if (!(den > 0)) return false;
    const double r = kM10Rs[scale] / den;
    const double t = steinhartTempC(r, kM10Poly);
    if (t < -120.0 || t > 60.0) return false;       // rs1729 treats these as invalid
    tempC = t;
    return true;
}

bool m10NtcAdc(double tempC, int& scale, int& adc) {
    const double r = steinhartResistance(tempC, kM10Poly);
    for (int sc = 0; sc < 3; sc++) {
        const double x = kM10Rs[sc] / r + kM10Rs[sc] / kM10Rp[sc];
        const double a = 4095.0 / (1.0 + x);
        if (a > 300.0 && a < 3800.0) { scale = sc; adc = (int)std::lround(a); return true; }
    }
    return false;
}

// ---- line code ----
const char kDmSync[33] = "11001100110011001010011001001100";

void DmWriter::bit(int b) {
    level_ ^= 1;
    out_.push_back(level_);
    if (b) level_ ^= 1;
    out_.push_back(level_);
}

void DmWriter::sync() {
    if (level_ != 0) bit(0);                       // an even number of zero bits brings the level back to 0
    for (int i = 0; i < 32; i++) out_.push_back((uint8_t)(kDmSync[i] == '1'));
    level_ = 0;                                    // the last symbol of the sync is 0
}

namespace {
uint32_t syncWord() {
    uint32_t w = 0;
    for (int i = 0; i < 32; i++) w = (w << 1) | (kDmSync[i] == '1');
    return w;
}
const uint32_t kSyncWord = syncWord();
inline int popc(uint32_t v) { return __builtin_popcount(v); }
constexpr uint64_t kMergeSpan = 8;                 // candidates this close are the same sync seen with a few errors
} // namespace

void DmFrameDecoder::reset() {
    buf_.clear();
    pend_.clear();
    base_ = total_ = reg_ = goodEnd_ = 0;
}

bool DmFrameDecoder::readBytes(const Cand& c, size_t nBytes, std::vector<uint8_t>& dst) const {
    const uint64_t first = c.start + 32;
    const uint64_t last = first + nBytes * 16;
    if (first < base_ || last > base_ + buf_.size()) return false;
    dst.assign(nBytes, 0);
    for (size_t i = 0; i < nBytes; i++) {
        uint8_t v = 0;
        for (int j = 0; j < 8; j++) {
            const size_t a = (size_t)(first - base_) + i * 16 + (size_t)j * 2;
            v = (uint8_t)((v << 1) | (buf_[a] ^ buf_[a + 1]));
        }
        dst[i] = v;
    }
    return true;
}

void DmFrameDecoder::push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) {
    for (size_t k = 0; k < n; k++) {
        const uint8_t s = sym[k] & 1;
        buf_.push_back(s);
        reg_ = (reg_ << 1) | s;
        total_++;
        if (total_ < 32) continue;
        const int errN = popc((uint32_t)reg_ ^ kSyncWord);
        const int errI = 32 - errN;
        const int err = std::min(errN, errI);
        if (err > maxSyncErrors_) continue;
        const uint64_t start = total_ - 32;
        if (start < goodEnd_) continue;
        bool merged = false;
        for (auto& c : pend_) {
            if (c.checked) continue;
            const uint64_t d = c.start > start ? c.start - start : start - c.start;
            if (d <= kMergeSpan) {
                if (err < c.err) { c.start = start; c.err = err; c.inv = errI < errN; }
                merged = true;
                break;
            }
        }
        if (!merged) pend_.push_back(Cand{start, err, errI < errN, 0, false});
    }
    service(timeSec, out);
    // drop symbols nobody needs any more
    uint64_t keep = total_ > 96 ? total_ - 96 : 0;
    for (const auto& c : pend_) keep = std::min(keep, c.start);
    if (keep > base_ + 8192) {
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)(keep - base_));
        base_ = keep;
    }
}

void DmFrameDecoder::service(double timeSec, std::vector<SondeFix>& out) {
    if (pend_.empty()) return;
    std::sort(pend_.begin(), pend_.end(), [](const Cand& a, const Cand& b) { return a.start < b.start; });
    std::vector<Cand> keepList;
    for (auto c : pend_) {
        if (c.start < goodEnd_) continue;
        if (!c.checked) {
            if (total_ < c.start + 32 + 32 + kMergeSpan) { keepList.push_back(c); continue; }
            std::vector<uint8_t> hd;
            if (!readBytes(c, 2, hd)) continue;
            c.need = frameLength(hd[0], hd[1]);
            if (c.need < 2) continue;                       // not a frame of this type
            c.checked = true;
        }
        if (total_ < c.start + 32 + c.need * 16) { keepList.push_back(c); continue; }
        std::vector<uint8_t> fr;
        if (!readBytes(c, c.need, fr)) continue;
        SondeFix fx;
        if (handleFrame(fr.data(), fr.size(), timeSec, fx)) {
            out.push_back(fx);
            if (fx.crcOk) goodEnd_ = c.start + 32 + c.need * 16;
        }
    }
    pend_.swap(keepList);
}

} // namespace sondebits
} // namespace dect2
