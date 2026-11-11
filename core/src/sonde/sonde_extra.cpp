// Radiosonde decoders for iMet-1 / iMet-4 (Bell 202 AFSK, UART, 1 packet set per second), iMet-54 (4800 baud UART, Hamming(8,4) with a block interleaver),
// Meisei iMS-100 (biphase-S, BCH(46,34)) and MRZ (MP3-H1, Manchester). Protocol knowledge from zilog80's RS demodulators (github.com/rs1729/RS, GPL-3.0).
// These were validated on the simulated signals of sonde_gen only: no recording of these types was available.
#include "dect2/sonde/sonde_bits.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace dect2 {

namespace {

// --------------------------------------------------------------------------------- shared
uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void wr16le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32le(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void wr32be(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
float f32(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
uint32_t u32f(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

void pushUart(std::vector<uint8_t>& s, uint8_t b) {
    s.push_back(0);
    for (int i = 0; i < 8; i++) s.push_back((b >> i) & 1);
    s.push_back(1);
}

// seconds of the UTC day (hh:mm:ss.ss) on the current day (the sonde sends no date); near midnight the nearest day wins
double utcToday(int h, int m, double sec, int64_t nowUnix) {
    const int64_t day = nowUnix - nowUnix % 86400;
    double t = (double)day + h * 3600.0 + m * 60.0 + sec;
    if (t - nowUnix > 43200) t -= 86400; else if (nowUnix - t > 43200) t += 86400;
    return t;
}

// --------------------------------------------------------------------------------- iMet-1 / iMet-4
uint16_t imetCrc(const uint8_t* d, size_t n) {
    uint16_t c = 0x1D0F;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

const int kImetPtuLen = 14, kImetEptuLen = 20, kImetGpsLen = 18, kImetEgpsLen = 30;

class ImetDecoder : public SondeBitDecoder {
public:
    const char* type() const override { return "iMet"; }
    double symbolRate() const override { return 1200; }
    bool isAfsk() const override { return true; }
    void reset() override { state_ = Hunt; ones_ = 0; have_ = false; pos_ = 0; pktN_ = 0; }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override {
        for (size_t i = 0; i < n; i++) step(sym[i], out);
    }
private:
    enum { Hunt, Start, Data, Stop } state_ = Hunt;
    int ones_ = 0, bit_ = 0;
    uint8_t byte_ = 0, buf_[64]; int pos_ = 0, pktN_ = 0;
    bool haveFix_ = false, have_ = false;
    SondeFix fix_;
    void step(int s, std::vector<SondeFix>& out) {
        if (state_ == Hunt || state_ == Start) {
            if (s) { ones_++; return; }
            // a start bit after idle ones
            if (state_ == Hunt && ones_ < 4) { ones_ = 0; return; }
            ones_ = 0; state_ = Data; bit_ = 0; byte_ = 0;
            return;
        }
        if (state_ == Data) { byte_ |= (uint8_t)(s << bit_); if (++bit_ == 8) state_ = Stop; return; }
        state_ = Start;
        ones_ = 0;
        if (!s) { abort(out); return; }           // framing error
        onByte(byte_, out);
    }
    void abort(std::vector<SondeFix>& out) { flushFix(out); state_ = Hunt; pos_ = 0; }
    void flushFix(std::vector<SondeFix>& out) {
        if (!haveFix_) return;
        haveFix_ = false;
        fix_.crcOk = true;
        fix_.type = "iMet"; fix_.subtype = "iMet-1/4";
        out.push_back(fix_);
        fix_ = SondeFix();
    }
    void onByte(uint8_t b, std::vector<SondeFix>& out) {
        if (pos_ == 0 && b != 0x01) { abort(out); return; }
        buf_[pos_++] = b;
        if (pos_ < 2) return;
        int need = 0;
        switch (buf_[1]) {
            case 0x01: need = kImetPtuLen; break;
            case 0x02: need = kImetGpsLen; break;
            case 0x04: need = kImetEptuLen; break;
            case 0x05: need = kImetEgpsLen; break;
            case 0x03: if (pos_ >= 3) { need = buf_[2] + 5; if (need > (int)sizeof buf_) { abort(out); return; } } else return; break;
            default: abort(out); return;
        }
        if (pos_ < need) return;
        const uint16_t c = imetCrc(buf_, (size_t)need - 2);
        const uint16_t got = (uint16_t)((buf_[need - 2] << 8) | buf_[need - 1]);
        pos_ = 0;
        if (c != got) { abort(out); return; }
        if (buf_[1] == 0x01 || buf_[1] == 0x04) ptu(); else if (buf_[1] == 0x02 || buf_[1] == 0x05) gps(buf_[1] == 0x05, out);
    }
    void ptu() {
        fix_.frame = rd16le(buf_ + 2);
        const uint32_t p = buf_[4] | (buf_[5] << 8) | (buf_[6] << 16);
        fix_.pressureHpa = p / 100.0; fix_.hasPressure = true;
        fix_.tempC = (int16_t)rd16le(buf_ + 7) / 100.0; fix_.hasTemp = true;
        fix_.humidity = rd16le(buf_ + 9) / 100.0; fix_.hasHumidity = true;
        fix_.batteryV = buf_[11] / 10.0;
    }
    void gps(bool ext, std::vector<SondeFix>& out) {
        fix_.lat = f32(rd32le(buf_ + 2)); fix_.lon = f32(rd32le(buf_ + 6));
        fix_.altM = (double)rd16le(buf_ + 10) - 5000.0;
        fix_.sats = buf_[12];
        fix_.hasPos = fix_.sats > 0 && fix_.lat >= -90 && fix_.lat <= 90 && fix_.lon >= -180 && fix_.lon <= 180;
        int tp = 13;
        if (ext) {
            const double ve = f32(rd32le(buf_ + 13)), vn = f32(rd32le(buf_ + 17)), vu = f32(rd32le(buf_ + 21));
            fix_.hasVel = true;
            fix_.vSpeed = vu; fix_.hSpeed = std::hypot(ve, vn);
            double hd = std::atan2(ve, vn) * 180.0 / M_PI; if (hd < 0) hd += 360; fix_.headingDeg = hd;
            tp = 25;
        }
        fix_.hasTime = true;
        fix_.unixTime = utcToday(buf_[tp], buf_[tp + 1], buf_[tp + 2], (int64_t)time(nullptr));
        haveFix_ = true;
        flushFix(out);
    }
};

// --------------------------------------------------------------------------------- MRZ (MP3-H1)
uint16_t mrzCrc(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

// WGS-84 ECEF to geodetic (iterated)
void ecefToLla(double x, double y, double z, double& lat, double& lon, double& alt) {
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f);
    lon = std::atan2(y, x);
    const double p = std::hypot(x, y);
    double phi = std::atan2(z, p * (1 - e2)), N = a, h = 0;
    for (int i = 0; i < 8; i++) {
        const double s = std::sin(phi);
        N = a / std::sqrt(1 - e2 * s * s);
        h = p / std::cos(phi) - N;
        phi = std::atan2(z, p * (1 - e2 * N / (N + h)));
    }
    lat = phi * 180.0 / M_PI; lon *= 180.0 / M_PI; alt = h;
}

void lla2ecef(double latDeg, double lonDeg, double alt, double& x, double& y, double& z) {
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f);
    const double la = latDeg * M_PI / 180.0, lo = lonDeg * M_PI / 180.0, s = std::sin(la), c = std::cos(la);
    const double N = a / std::sqrt(1 - e2 * s * s);
    x = (N + alt) * c * std::cos(lo); y = (N + alt) * c * std::sin(lo); z = (N * (1 - e2) + alt) * s;
}

class MrzDecoder : public SondeBitDecoder {
public:
    const char* type() const override { return "MRZ"; }
    double symbolRate() const override { return 2400; }
    bool midpointSlicer() const override { return true; }
    void reset() override { sync_ = 0; bits_.clear(); run_ = false; cfgC_ = cfgD_ = -1; last_ = -1; }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override {
        for (size_t i = 0; i < n; i++) step(sym[i], out);
    }
private:
    uint64_t sync_ = 0; std::vector<uint8_t> bits_; bool run_ = false; int inv_ = 0, phase_ = 0, cfgC_ = -1, cfgD_ = -1, last_ = -1;
    // on air: the preamble 1001... and then AA BF 35 Manchester coded, MSB first (Manchester: 1 -> 10, 0 -> 01 or the inverse)
    void step(int s, std::vector<SondeFix>& out) {
        if (!run_) {
            sync_ = (sync_ << 1) | (uint64_t)(s & 1);
            const uint64_t pat = 0x99999999999AAULL & ((1ULL << 44) - 1);     // 9 x 1001 and 10101010
            const uint64_t m = (1ULL << 44) - 1;
            const uint64_t v = sync_ & m;
            int match = -1;
            // 4.0: up to 3 symbol errors in the 44 (the preamble repeats: a copy of the pattern shifted by 2 or 4 symbols differs from it by 4 in the last
            // 8 symbols, so a looser match there could lock early; an exact match needed 44 right symbols in a row, 41 % of the frames at a 2 % symbol error rate)
            // (the last 8 symbols, 10101010, may have one error at most: a copy of the pattern shifted by 2 or 4 symbols differs there by 4)
            if (__builtin_popcountll(v ^ pat) <= 3 && __builtin_popcountll((v ^ pat) & 0xFF) <= 1) match = 0;
            else if (__builtin_popcountll(v ^ (pat ^ m)) <= 3 && __builtin_popcountll((v ^ pat ^ m) & 0xFF) <= 1) match = 1;
            if (match >= 0) { inv_ = match; run_ = true; phase_ = 0; bits_.clear(); }
            return;
        }
        // two symbols per bit: the header has just ended on a symbol boundary
        symPair_[phase_] = (uint8_t)(inv_ ? !s : s);
        if (++phase_ == 2) {
            phase_ = 0;
            // the header ends with ...1010: symbols 10 are one bit, keep the header's bit sense: "10" = 1
            bits_.push_back(symPair_[0]);
            if (bits_.size() == 394) frame(out);   // the header ends 6 bits into the BF: 2 bits of it, then the 49 bytes from 0x35 on
        }
    }
    uint8_t symPair_[2] = {0, 0};
    void frame(std::vector<SondeFix>& out) {
        run_ = false;
        uint8_t f[51];
        std::memset(f, 0, sizeof f);
        f[0] = 0xAA; f[1] = 0xBC;
        for (size_t i = 0; i < bits_.size(); i++) { const size_t b = i + 14; f[b / 8] |= (uint8_t)(bits_[i] << (7 - b % 8)); }
        SondeFix o;
        if (!mrzParseFrame(f, sizeof f, o)) return;
        if ((f[3] & 15) == 0xC) cfgC_ = (int)rd32le(f + (rd16le(f + 30) == 0xFFFF ? 41 : 44));
        if ((f[3] & 15) == 0xD) cfgD_ = (int)rd32le(f + (rd16le(f + 30) == 0xFFFF ? 41 : 44));
        o.serial = "MRZ-" + (cfgC_ >= 0 ? std::to_string(cfgC_) : std::string("?")) + "-" + (cfgD_ >= 0 ? std::to_string(cfgD_) : std::string("?"));
        if (cfgC_ < 0 || cfgD_ < 0) o.serial.clear();
        // a frame goes out 6 times: report one
        if (o.frame == last_) return;
        last_ = o.frame;
        out.push_back(o);
    }
};

// --------------------------------------------------------------------------------- iMet-54
uint8_t hamEncNib(uint8_t n) {
    const int m0 = n & 1, m1 = (n >> 1) & 1, m2 = (n >> 2) & 1, m3 = (n >> 3) & 1;
    const int c0 = m0 ^ m1 ^ m3, c1 = m0 ^ m2 ^ m3, c2 = m0, c3 = m1 ^ m2 ^ m3, c4 = m1, c5 = m2, c6 = m3, c7 = m0 ^ m1 ^ m2;
    return (uint8_t)(c0 | (c1 << 1) | (c2 << 2) | (c3 << 3) | (c4 << 4) | (c5 << 5) | (c6 << 6) | (c7 << 7));
}
// 0 clean, 1 corrected, -1 not correctable
int hamDec(uint8_t c, uint8_t& n) {
    const int b[8] = {c & 1, (c >> 1) & 1, (c >> 2) & 1, (c >> 3) & 1, (c >> 4) & 1, (c >> 5) & 1, (c >> 6) & 1, (c >> 7) & 1};
    const int s0 = b[0] ^ b[2] ^ b[4] ^ b[6], s1 = b[1] ^ b[2] ^ b[5] ^ b[6], s2 = b[3] ^ b[4] ^ b[5] ^ b[6], s3 = (b[0] ^ b[1] ^ b[2] ^ b[3] ^ b[4] ^ b[5] ^ b[6] ^ b[7]);
    const int s = s0 | (s1 << 1) | (s2 << 2) | (s3 << 3);
    int rc = 0;
    if (s) {
        static const int he[8] = {9, 10, 11, 12, 13, 14, 15, 8};
        int j = -1;
        for (int i = 0; i < 8; i++) if (he[i] == s) j = i;
        if (j < 0) return -1;
        c = (uint8_t)(c ^ (1 << j));
        rc = 1;
    }
    n = (uint8_t)(((c >> 2) & 1) | (((c >> 4) & 1) << 1) | (((c >> 5) & 1) << 2) | (((c >> 6) & 1) << 3));
    return rc;
}

void imet54StdCrc(const uint8_t* f, uint32_t& a0, uint32_t& a1) {
    uint32_t c0 = 0x48EB, c1 = 0x1ACA;
    a0 = a1 = 0;
    int n = 104;
    for (int k = 0; k < 108; k++) {
        for (int b = 0; b < 8; b++) {
            if ((n < 100 || (n > 101 && n < 106)) && ((f[n] >> b) & 1)) { a0 ^= c0; a1 ^= c1; }
            uint32_t nx0 = c0, nx1 = c1;
            if (c1 & 0x8000) { nx0 ^= 0x0EDB; nx1 ^= 0x8260; }
            nx0 = (nx0 << 1) | ((c1 & 0x8000) ? 1u : 0u);
            nx1 = (nx1 << 1) | (((c1 ^ c0) & 0x8000) ? 1u : 0u);
            nx0 &= 0xFFFF;
            c0 = nx0; c1 = nx1;
        }
        if (n % 4 == 3) n -= 7; else n++;
    }
}

class Imet54Decoder : public SondeBitDecoder {
public:
    const char* type() const override { return "iMet-54"; }
    double symbolRate() const override { return 4800; }
    bool narrowChannel() const override { return true; }
    bool midpointSlicer() const override { return true; }
    void reset() override { hdr_ = 0; run_ = false; bits_.clear(); }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override { for (size_t i = 0; i < n; i++) step(sym[i], out); }
private:
    uint64_t hdr_ = 0; bool run_ = false; int inv_ = 0; std::vector<uint8_t> bits_;
    void step(int s, std::vector<SondeFix>& out) {
        if (!run_) {
            hdr_ = (hdr_ << 1) | (uint64_t)(s & 1);
            const uint64_t pat = imet54Header(), m = (1ULL << 40) - 1, v = hdr_ & m;
            if (__builtin_popcountll(v ^ pat) <= 3) inv_ = 0;
            else if (__builtin_popcountll(v ^ (pat ^ m)) <= 3) inv_ = 1;
            else return;
            run_ = true; bits_.clear();
            return;
        }
        bits_.push_back(inv_ ? !s : s);
        if (bits_.size() == (size_t)(3 + 216) * 10) frame(out);
    }
    static uint64_t imet54Header() {
        static const uint8_t b[4] = {0x00, 0xAA, 0x24, 0x24};
        uint64_t v = 0;
        for (uint8_t x : b) { v = (v << 1); for (int i = 0; i < 8; i++) v = (v << 1) | ((x >> i) & 1); v = (v << 1) | 1; }
        return v;
    }
    void frame(std::vector<SondeFix>& out) {
        run_ = false;
        std::vector<uint8_t> d;
        for (size_t i = 0; i + 10 <= bits_.size(); i += 10) for (size_t b = 1; b <= 8; b++) d.push_back(bits_[i + b]);
        if (d.size() < 24 + 1728) return;
        std::vector<uint8_t> de(1728);
        for (size_t n = 0; n < 1728; n += 64)
            for (size_t i = 0; i < 8; i++) for (size_t j = 0; j < 8; j++) de[n + 8 * j + i] = d[24 + n + 8 * i + j];
        uint8_t f[108]; int corr = 0;
        std::memset(f, 0, sizeof f);
        for (int i = 0; i < 216; i++) {
            uint8_t c = 0;
            for (int b = 0; b < 8; b++) c |= (uint8_t)(de[(size_t)i * 8 + (size_t)b] << b);
            uint8_t nib = 0;
            const int r = hamDec(c, nib);
            if (r < 0) return;
            corr += r;
            f[i / 2] = (uint8_t)(f[i / 2] << 4) | nib;
        }
        SondeFix o;
        o.corrected = corr;
        if (imet54ParseFrame(f, sizeof f, o)) out.push_back(o);
    }
};

// --------------------------------------------------------------------------------- Meisei iMS-100
// BCH(63,51) generator x^12+x^10+x^8+x^5+x^4+x^3+1, shortened to (46,34); t = 2
const uint32_t kMeiseiGen = (1u << 12) | (1u << 10) | (1u << 8) | (1u << 5) | (1u << 4) | (1u << 3) | 1u;
uint32_t meiseiRem(uint64_t cw46) {   // bit 45 = first transmitted
    uint64_t r = cw46;
    for (int i = 45; i >= 12; i--) if ((r >> i) & 1) r ^= (uint64_t)kMeiseiGen << (i - 12);
    return (uint32_t)(r & 0xfff);
}
uint32_t meiseiCheck(uint64_t msg34) { return meiseiRem(msg34 << 12); }

bool meiseiFix(uint64_t& cw, int& nCorr) {
    uint32_t s = meiseiRem(cw);
    nCorr = 0;
    if (!s) return true;
    static uint32_t syn[46];
    static bool init = false;
    if (!init) { for (int i = 0; i < 46; i++) syn[i] = meiseiRem(1ULL << i); init = true; }
    for (int i = 0; i < 46; i++) if (syn[i] == s) { cw ^= 1ULL << i; nCorr = 1; return true; }
    for (int i = 0; i < 46; i++) for (int j = i + 1; j < 46; j++) if ((syn[i] ^ syn[j]) == s) { cw ^= (1ULL << i) | (1ULL << j); nCorr = 2; return true; }
    return false;
}

class MeiseiDecoder : public SondeBitDecoder {
public:
    const char* type() const override { return "iMS-100"; }
    double symbolRate() const override { return 2400; }
    void reset() override { hdr_ = 0; run_ = false; std::memset(cfg_, 0, sizeof cfg_); sn_ = 0; }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override { for (size_t i = 0; i < n; i++) step(sym[i], out); }
private:
    static constexpr uint64_t kSync0 = 0b101010101011010100101011001101001100101011001101ULL;
    static constexpr uint64_t kSync1 = 0b110011001101001101001101010100101010110010101010ULL;
    uint64_t hdr_ = 0; bool run_ = false, odd_ = false; int nSym_ = 0; std::vector<uint8_t> raw_;
    float cfg_[64]; int sn_ = 0, cnt_ = -1, ms_ = -1, hh_ = -1, mm_ = -1, date_ = -1;
    double lastLat_ = 0, lastLon_ = 0, lastAlt_ = 0; bool havePos_ = false; double vH_ = 0, vD_ = 0, vU_ = 0;
    void step(int s, std::vector<SondeFix>& out) {
        if (!run_) {
            hdr_ = (hdr_ << 1) | (uint64_t)(s & 1);
            const uint64_t m = (1ULL << 48) - 1, v = hdr_ & m;
            if (v == kSync0) { which_ = 0; inv_ = false; } else if (v == kSync1) { which_ = 1; inv_ = false; }
            else if (v == (kSync0 ^ m)) { which_ = 0; inv_ = true; } else if (v == (kSync1 ^ m)) { which_ = 1; inv_ = true; } else return;
            run_ = true; raw_.clear();
            return;
        }
        raw_.push_back((uint8_t)(s & 1));
        if (raw_.size() == 552) sub(out);    // 276 bits x 2
    }
    int which_ = 0; bool inv_ = false;
    // biphase-S: bit = 1 when the two symbols are equal
    int bitAt(int b) const { return raw_[(size_t)b * 2] == raw_[(size_t)b * 2 + 1] ? 1 : 0; }
    uint64_t blk_[6];
    void sub(std::vector<SondeFix>& out) {
        run_ = false;
        int corr = 0;
        for (int b = 0; b < 6; b++) {
            uint64_t cw = 0;
            for (int i = 0; i < 46; i++) cw = (cw << 1) | (uint64_t)bitAt(b * 46 + i);
            int c = 0;
            if (!meiseiFix(cw, c)) return;
            corr += c;
            blk_[b] = cw;
        }
        word_[0] = 0;
        auto wordOf = [&](int k) -> uint32_t { return (uint32_t)((blk_[k / 2] >> (k % 2 ? 13 : 30)) & 0xFFFF); };
        auto parityOk = [&](int k) { uint32_t w = wordOf(k); int n = 0; while (w) { n += w & 1; w >>= 1; } const int pb = (int)((blk_[k / 2] >> (k % 2 ? 12 : 29)) & 1); return pb == ((n % 2) == 0 ? 1 : 0); };
        for (int k = 0; k < 12; k++) if (!parityOk(k)) return;
        if (which_ == 0) {
            cnt_ = (int)wordOf(0);
            // the config table: 32-bit float, second word high half, iMS-100 format (custom exponent)
            uint32_t w = (wordOf(3) << 16) | wordOf(2);
            cfg_[cnt_ & 63] = imsFloat(w);
            if ((cnt_ & 1) == 0) { ms_ = (int)wordOf(10); hh_ = (int)(wordOf(11) >> 8); mm_ = (int)(wordOf(11) & 0xff); }
            if ((cnt_ & 15) == 0) { const float f = cfg_[cnt_ & 63]; if (f > 0 && f < 1e9f) sn_ = (int)std::lround(f); }
        } else if ((cnt_ & 1) == 0) {
            date_ = (int)wordOf(0);
            const uint32_t latR = (wordOf(1) << 16) | wordOf(2), lonR = (wordOf(3) << 16) | wordOf(4);
            lastLat_ = nmea(latR); lastLon_ = nmea(lonR);
            lastAlt_ = (double)((wordOf(5) << 8) | (wordOf(6) >> 8)) / 100.0;
            vD_ = wordOf(9) / 100.0; vH_ = wordOf(10) / 194.384;
            havePos_ = true;
        } else {
            const int16_t vu = (int16_t)wordOf(1);
            vU_ = vu ? vu / 19.4384 : 0;
        }
        (void)corr;
        if (which_ == 1 && (cnt_ & 1) == 0 && sn_ > 0 && havePos_ && ms_ >= 0 && hh_ >= 0) {
            SondeFix o;
            o.type = "iMS-100"; o.subtype = "iMS-100"; o.crcOk = true; o.corrected = corr;
            o.serial = std::to_string(sn_);
            o.frame = cnt_;
            o.hasPos = true; o.lat = lastLat_; o.lon = lastLon_; o.altM = lastAlt_;
            o.hasVel = true; o.hSpeed = vH_; o.headingDeg = vD_; o.vSpeed = vU_;
            if (date_ >= 0) {
                int d = date_ / 1000, mo = (date_ / 10) % 100, yd = date_ % 10;
                const time_t now = time(nullptr);
                const struct tm* g = gmtime(&now);
                int y = 1900 + g->tm_year; y = y - y % 10 + yd;
                struct tm t = {};
                t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = hh_; t.tm_min = mm_; t.tm_sec = ms_ / 1000;
#ifdef _WIN32
                o.unixTime = (double)_mkgmtime(&t) + (ms_ % 1000) / 1000.0;
#else
                o.unixTime = (double)timegm(&t) + (ms_ % 1000) / 1000.0;
#endif
                o.hasTime = true;
            }
            out.push_back(o);
        }
    }
    uint32_t word_[12];
    static double nmea(uint32_t r) { const int deg = (int)(r / 1000000); return deg + (r - (uint32_t)deg * 1000000) / 1.0e4 / 60.0; }
    static float imsFloat(uint32_t w) { return f32(((w & 0xFF000000u) >> 1 & 0x7F800000u) | ((w & 0x00800000u) << 8) | (w & 0x007FFFFFu)) / 4.0f; }
};

} // namespace

bool mrzParseFrame(const uint8_t* f, size_t n, SondeFix& o) {
    if (n < 51) return false;
    const bool ll = rd16le(f + 30) == 0xFFFF;
    const int crcLen = ll ? 42 : 45;
    const uint16_t c = mrzCrc(f + 3, (size_t)crcLen);
    if (c != rd16le(f + 3 + crcLen)) return false;
    o.type = "MRZ"; o.subtype = ll ? "MRZ-N1 (lat/lon)" : "MRZ-N1";
    o.crcOk = true;
    o.frame = f[3] & 15;
    const int h = f[4], m = f[5], s = f[6];
    o.hasTime = h < 24 && m < 60 && s < 61;
    if (o.hasTime) o.unixTime = utcToday(h, m, s, (int64_t)time(nullptr));
    if (ll) {
        o.lat = (int32_t)rd32le(f + 7) * 1e-6; o.lon = (int32_t)rd32le(f + 11) * 1e-6; o.altM = (int32_t)rd32le(f + 15) * 0.01;
        o.hSpeed = (int16_t)rd16le(f + 19) / 100.0; o.headingDeg = rd16le(f + 21) / 100.0; o.hasVel = true;
        o.hasPos = true;
    } else {
        double x = (int32_t)rd32le(f + 8) * 0.01, y = (int32_t)rd32le(f + 12) * 0.01, z = (int32_t)rd32le(f + 16) * 0.01;
        o.hasPos = std::hypot(x, y) > 1e5;
        if (o.hasPos) ecefToLla(x, y, z, o.lat, o.lon, o.altM);
        const double vx = (int16_t)rd16le(f + 20) / 100.0, vy = (int16_t)rd16le(f + 22) / 100.0, vz = (int16_t)rd16le(f + 24) / 100.0;
        if (o.hasPos) {
            const double la = o.lat * M_PI / 180, lo = o.lon * M_PI / 180;
            const double e = -std::sin(lo) * vx + std::cos(lo) * vy, nn = -std::sin(la) * std::cos(lo) * vx - std::sin(la) * std::sin(lo) * vy + std::cos(la) * vz;
            const double u = std::cos(la) * std::cos(lo) * vx + std::cos(la) * std::sin(lo) * vy + std::sin(la) * vz;
            o.hSpeed = std::hypot(e, nn); o.vSpeed = u; o.headingDeg = std::atan2(e, nn) * 180 / M_PI;
            if (o.headingDeg < 0) o.headingDeg += 360;
            o.hasVel = true;
        }
        o.sats = f[26];
        o.tempC = (int16_t)rd16le(f + 29) / 100.0; o.hasTemp = o.tempC > -120 && o.tempC < 80;
        o.humidity = (int16_t)rd16le(f + 31) / 100.0; o.hasHumidity = o.humidity >= 0 && o.humidity <= 100;
    }
    return true;
}

bool imet54ParseFrame(const uint8_t* f, size_t n, SondeFix& o) {
    if (n < 108) return false;
    uint32_t a0 = 0, a1 = 0;
    imet54StdCrc(f, a0, a1);
    const uint32_t d0 = (uint32_t)(f[100] << 8) | f[101], d1 = (uint32_t)(f[106] << 8) | f[107];
    const uint32_t x0 = a0 ^ d0 ^ 0x5000, x1 = a1 ^ d1 ^ 0x1DAD;
    if (x1 != 0 || (x0 & 0xF000) != 0) return false;
    o.type = "iMet-54"; o.subtype = "iMet-54"; o.crcOk = true;
    const uint32_t serial = rd32be(f);
    o.serial = "iMet54-" + std::to_string(serial);
    const int32_t tv = (int32_t)rd32be(f + 4);
    const int hms = tv / 1000;
    int h = hms / 10000, m = (hms / 100) % 100, s = hms % 100;
    o.hasTime = h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 61;
    if (o.hasTime) o.unixTime = utcToday(h, m, s + (tv % 1000) / 1000.0, (int64_t)time(nullptr));
    auto geo = [](int32_t v) { const double dd = v / 1e6; const int d = (int)dd; return d + (dd - d) * 100.0 / 60.0; };
    o.lat = geo((int32_t)rd32be(f + 8)); o.lon = geo((int32_t)rd32be(f + 12));
    o.altM = (int32_t)rd32be(f + 16) / 10.0;
    o.hasPos = o.lat >= -90 && o.lat <= 90 && o.lon >= -180 && o.lon <= 180 && o.altM > -500;
    const float t = f32(rd32be(f + 0x1C));
    if (rd32be(f + 0x1C) != 0x4E6E6B28 && t > -120 && t < 80) { o.tempC = t; o.hasTemp = true; }
    return true;
}

std::unique_ptr<SondeBitDecoder> makeImetDecoder() { return std::make_unique<ImetDecoder>(); }
std::unique_ptr<SondeBitDecoder> makeMrzDecoder() { return std::make_unique<MrzDecoder>(); }
std::unique_ptr<SondeBitDecoder> makeMeiseiDecoder() { return std::make_unique<MeiseiDecoder>(); }
std::unique_ptr<SondeBitDecoder> makeImet54Decoder() { return std::make_unique<Imet54Decoder>(); }

// --------------------------------------------------------------------------------- test signals
namespace {
void afskAdd(std::vector<uint8_t>& s, int bit, double& ph) {
    // 22 sub-symbols per bit at 26400 sub-symbols per second, the FM deviation is the tone's sign
    const double f = bit ? 1200.0 : 2200.0;
    for (int i = 0; i < 22; i++) {
        s.push_back(std::sin(ph) >= 0 ? 1 : 0);
        ph += 2 * M_PI * f / 26400.0;
    }
}
void imetPacket(std::vector<uint8_t>& bits, std::vector<uint8_t> p) {
    const uint16_t c = imetCrc(p.data(), p.size());
    p.push_back((uint8_t)(c >> 8)); p.push_back((uint8_t)c);
    for (uint8_t b : p) pushUart(bits, b);
}
} // namespace

std::vector<uint8_t> imetBits(const SondeTruth& t) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 40; i++) b.push_back(1);
    std::vector<uint8_t> ptu(12);
    ptu[0] = 1; ptu[1] = 1; wr16le(&ptu[2], (uint32_t)t.frame);
    const uint32_t p = (uint32_t)std::lround(t.pressureHpa * 100);
    ptu[4] = (uint8_t)p; ptu[5] = (uint8_t)(p >> 8); ptu[6] = (uint8_t)(p >> 16);
    wr16le(&ptu[7], (uint32_t)(int16_t)std::lround(t.tempC * 100)); wr16le(&ptu[9], (uint32_t)std::lround(t.humidity * 100)); ptu[11] = (uint8_t)std::lround(t.batteryV * 10);
    imetPacket(b, ptu);
    for (int i = 0; i < 8; i++) b.push_back(1);
    std::vector<uint8_t> g(16);
    g[0] = 1; g[1] = 2; wr32le(&g[2], u32f((float)t.lat)); wr32le(&g[6], u32f((float)t.lon)); wr16le(&g[10], (uint32_t)std::lround(t.altM + 5000)); g[12] = (uint8_t)t.sats;
    const int64_t s = (int64_t)t.unixTime; g[13] = (uint8_t)((s / 3600) % 24); g[14] = (uint8_t)((s / 60) % 60); g[15] = (uint8_t)(s % 60);
    imetPacket(b, g);
    for (int i = 0; i < 8; i++) b.push_back(1);
    return b;
}

std::vector<uint8_t> imetSymbols(const SondeTruth& t) {
    std::vector<uint8_t> s; double ph = 0;
    for (uint8_t b : imetBits(t)) afskAdd(s, b, ph);
    return s;
}

std::vector<uint8_t> mrzFrameBytes(const SondeTruth& t, int sub, bool latLon) {
    uint8_t f[51];
    std::memset(f, 0, sizeof f);
    f[0] = 0xAA; f[1] = 0xBF; f[2] = 0x35;
    f[3] = (uint8_t)(sub & 15);
    const int64_t s = (int64_t)t.unixTime;
    f[4] = (uint8_t)((s / 3600) % 24); f[5] = (uint8_t)((s / 60) % 60); f[6] = (uint8_t)(s % 60);
    if (latLon) {
        wr32le(f + 7, (uint32_t)(int32_t)std::lround(t.lat * 1e6)); wr32le(f + 11, (uint32_t)(int32_t)std::lround(t.lon * 1e6)); wr32le(f + 15, (uint32_t)(int32_t)std::lround(t.altM * 100));
        wr16le(f + 19, (uint32_t)(uint16_t)std::lround(t.hSpeed * 100)); wr16le(f + 21, (uint32_t)std::lround(t.headingDeg * 100));
        f[30] = 0xFF; f[31] = 0xFF;
        const uint32_t cfg = sub == 0xC ? 11223u : sub == 0xD ? 4455u : 0u;
        wr32le(f + 41, cfg);
        wr16le(f + 45, mrzCrc(f + 3, 42));
    } else {
        double x, y, z;
        lla2ecef(t.lat, t.lon, t.altM, x, y, z);
        wr32le(f + 8, (uint32_t)(int32_t)std::lround(x * 100)); wr32le(f + 12, (uint32_t)(int32_t)std::lround(y * 100)); wr32le(f + 16, (uint32_t)(int32_t)std::lround(z * 100));
        wr16le(f + 20, 0u); wr16le(f + 22, 0u); wr16le(f + 24, 0u);
        f[26] = (uint8_t)t.sats;
        wr16le(f + 29, (uint32_t)(int16_t)std::lround(t.tempC * 100)); wr16le(f + 31, (uint32_t)(int16_t)std::lround(t.humidity * 100));
        const uint32_t cfg = sub == 0xC ? 11223u : sub == 0xD ? 4455u : 0u;
        wr32le(f + 44, cfg);
        wr16le(f + 48, mrzCrc(f + 3, 45));
    }
    return std::vector<uint8_t>(f, f + 51);
}

std::vector<uint8_t> mrzSymbols(const SondeTruth& t, int sub, bool latLon) {
    std::vector<uint8_t> s;
    const auto f = mrzFrameBytes(t, sub, latLon);
    // Manchester (1 -> 10, 0 -> 01): preamble AA, then AA BF 35 ... (the receiver's 44-symbol header ends 6 bits into the BF)
    auto put = [&s](uint8_t v) { for (int i = 7; i >= 0; i--) { const int b = (v >> i) & 1; s.push_back((uint8_t)b); s.push_back((uint8_t)!b); } };
    // the same frame goes out 6 times in a row
    for (int r = 0; r < 6; r++) {
        for (int i = 0; i < (r == 0 ? 3 : 1); i++) put(0xAA);
        for (size_t b = 1; b < f.size(); b++) put(f[b]);
    }
    return s;
}

std::vector<uint8_t> imet54FrameBytes(const SondeTruth& t, int serial) {
    uint8_t f[108];
    std::memset(f, 0, sizeof f);
    wr32be(f, (uint32_t)serial);
    const int64_t s = (int64_t)t.unixTime;
    wr32be(f + 4, (uint32_t)(((s / 3600) % 24) * 10000000 + ((s / 60) % 60) * 100000 + (s % 60) * 1000));
    auto geo = [](double d) { const int dd = (int)d; return (int32_t)std::lround(dd * 1e6 + (d - dd) * 60.0 * 1e4); };
    wr32be(f + 8, (uint32_t)geo(t.lat)); wr32be(f + 12, (uint32_t)geo(t.lon));
    wr32be(f + 16, (uint32_t)(int32_t)std::lround(t.altM * 10));
    wr32be(f + 0x1C, u32f((float)t.tempC));
    uint32_t a0, a1;
    imet54StdCrc(f, a0, a1);
    const uint32_t d0 = (a0 ^ 0x5000) & 0xFFFF, d1 = (a1 ^ 0x1DAD) & 0xFFFF;
    f[100] = (uint8_t)(d0 >> 8); f[101] = (uint8_t)d0; f[106] = (uint8_t)(d1 >> 8); f[107] = (uint8_t)d1;
    return std::vector<uint8_t>(f, f + 108);
}

std::vector<uint8_t> imet54Symbols(const SondeTruth& t, int serial) {
    std::vector<uint8_t> s;
    for (int i = 0; i < 10; i++) { pushUart(s, 0x00); pushUart(s, 0xAA); }
    for (int i = 0; i < 2; i++) pushUart(s, 0x24);
    // 3 more synch bytes, not protected
    pushUart(s, 0x24); pushUart(s, 0x24); pushUart(s, 0x42);
    const auto f = imet54FrameBytes(t, serial);
    std::vector<uint8_t> c;
    for (int i = 0; i < 108; i++) { c.push_back(hamEncNib(f[(size_t)i] >> 4)); c.push_back(hamEncNib(f[(size_t)i] & 15)); }
    std::vector<uint8_t> d(1728);
    for (int i = 0; i < 216; i++) for (int b = 0; b < 8; b++) d[(size_t)i * 8 + (size_t)b] = (c[(size_t)i] >> b) & 1;
    std::vector<uint8_t> in(1728);
    for (size_t n = 0; n < 1728; n += 64) for (size_t i = 0; i < 8; i++) for (size_t j = 0; j < 8; j++) in[n + 8 * i + j] = d[n + 8 * j + i];
    for (size_t i = 0; i < 1728; i += 8) { s.push_back(0); for (size_t b = 0; b < 8; b++) s.push_back(in[i + b]); s.push_back(1); }
    return s;
}

namespace {
uint64_t imsBlock(uint32_t w0, uint32_t w1) {
    auto par = [](uint32_t w) { int n = 0; while (w) { n += w & 1; w >>= 1; } return n % 2 == 0 ? 1u : 0u; };
    const uint64_t msg = ((uint64_t)w0 << 18) | ((uint64_t)par(w0) << 17) | ((uint64_t)w1 << 1) | par(w1);
    return (msg << 12) | meiseiCheck(msg);
}
void imsEmitBlock(std::vector<uint8_t>& s, uint64_t cw) {
    for (int i = 45; i >= 0; i--) {
        const int bit = (int)((cw >> i) & 1);
        // biphase-S: every bit starts with a transition; a 1 has no transition in the middle
        s.push_back(s.empty() ? 1 : (uint8_t)!s.back());
        s.push_back(bit ? s.back() : (uint8_t)!s.back());
    }
}
void imsSub(std::vector<uint8_t>& s, uint64_t sync, const uint32_t w[12]) {
    for (int i = 47; i >= 0; i--) s.push_back((uint8_t)((sync >> i) & 1));
    for (int b = 0; b < 6; b++) imsEmitBlock(s, imsBlock(w[b * 2], w[b * 2 + 1]));
}
uint32_t imsFromFloat(float f) {
    const uint32_t v = u32f(f * 4.0f);
    return ((v & 0x7F800000u) << 1) | ((v & 0x80000000u) >> 8) | (v & 0x007FFFFFu);
}
} // namespace

std::vector<uint8_t> meiseiSymbols(const SondeTruth& t, int cnt, int serial) {
    std::vector<uint8_t> s;
    const int64_t us = (int64_t)t.unixTime;
    const time_t tu = (time_t)us;
    const struct tm* g = gmtime(&tu);
    uint32_t a[12] = {}, b[12] = {};
    a[0] = (uint32_t)(cnt & 0xFFFF);
    const uint32_t cfg = imsFromFloat(((cnt & 15) == 0) ? (float)serial : 0.0f);
    a[2] = cfg & 0xFFFF; a[3] = cfg >> 16;
    a[10] = (uint32_t)((g->tm_sec) * 1000);
    a[11] = (uint32_t)((g->tm_hour << 8) | g->tm_min);
    a[7] = 0xC1; b[7] = 0xC1;
    auto nm = [](double d) { const int dd = (int)d; return (uint32_t)std::lround(dd * 1e6 + (d - dd) * 60.0 * 1e4); };
    const uint32_t latR = nm(t.lat), lonR = nm(t.lon);
    b[0] = (uint32_t)(g->tm_mday * 1000 + (g->tm_mon + 1) * 10 + (1900 + g->tm_year) % 10);
    b[1] = latR >> 16; b[2] = latR & 0xFFFF;
    b[3] = lonR >> 16; b[4] = lonR & 0xFFFF;
    const uint32_t a24 = (uint32_t)std::lround(t.altM * 100);
    b[5] = a24 >> 8;
    b[6] = (a24 & 0xff) << 8;
    b[9] = (uint32_t)std::lround(t.headingDeg * 100);
    b[10] = (uint32_t)std::lround(t.hSpeed * 194.384);
    imsSub(s, 0b101010101011010100101011001101001100101011001101ULL, a);
    imsSub(s, 0b110011001101001101001101010100101010110010101010ULL, b);
    return s;
}

} // namespace dect2
