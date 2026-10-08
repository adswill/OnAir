// RS41 frame decoder and builder (see sonde_rs41.h for the sources).
#include "dect2/sonde_rs41.h"
#include "dect2/sonde_geo.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {

// rs1729/RS demod/mod/rs41mod.c: mask[MASK_LEN]
const uint8_t kMask[64] = {
    0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
    0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
    0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
    0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
// rs41_header_bytes (the frame bytes before whitening)
const uint8_t kHeader[8] = {0x86, 0x35, 0xF4, 0x40, 0x93, 0xDF, 0x1A, 0x60};

// ---- GF(256), x^8 + x^4 + x^3 + x^2 + 1 ----
struct Gf {
    uint8_t exp[512], log[256];
    uint8_t gen[25];       // generator polynomial, ascending powers, monic
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = (uint8_t)x; log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        std::memset(gen, 0, sizeof gen);
        gen[0] = 1;
        int deg = 0;
        for (int r = 0; r < 24; r++) {                  // multiply by (x + alpha^r)
            for (int j = deg + 1; j >= 1; j--) gen[j] = (uint8_t)(gen[j - 1] ^ mul(gen[j], exp[r]));
            gen[0] = mul(gen[0], exp[r]);
            deg++;
        }
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[log[a] + 255 - log[b]] : 0; }
};
const Gf& gf() { static const Gf g; return g; }

float f32le(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }       // hosts are little endian
void putF32(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
uint32_t u3(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
uint32_t u4(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t u2(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Calibration offsets inside the 816 byte calibration area (calibytes[subframe * 16 + i]); rs41mod.c get_PTU
constexpr int kOfRf1 = 61, kOfRf2 = 65, kOfCo1 = 77, kOfCalT1 = 89, kOfCalH = 117;

struct TCal { double rf1, rf2, co[3], ca[3]; double calH0; };
TCal readCal(const uint8_t* c) {
    TCal t;
    t.rf1 = f32le(c + kOfRf1); t.rf2 = f32le(c + kOfRf2);
    for (int i = 0; i < 3; i++) { t.co[i] = f32le(c + kOfCo1 + 4 * i); t.ca[i] = f32le(c + kOfCalT1 + 4 * i); }
    t.calH0 = f32le(c + kOfCalH);
    return t;
}

// rs41mod.c get_T: f, f1, f2 are the 24 bit counts of the sensor and of the two reference resistors
bool calcT(const TCal& k, double f, double f1, double f2, double& T) {
    if (!(f1 > 0 && f2 > f1 && f > 0) || !(k.rf2 > k.rf1)) return false;
    const double g = (f2 - f1) / (k.rf2 - k.rf1);
    const double Rb = (f1 * k.rf2 - f2 * k.rf1) / (f2 - f1);
    const double Rc = f / g - Rb;
    const double R = Rc * k.ca[0];
    T = (k.co[0] + k.co[1] * R + k.co[2] * R * R + k.ca[1]) * (1.0 + k.ca[2]);
    return std::isfinite(T) && T > -150 && T < 100;
}
// rs41mod.c get_RHemp
constexpr double kRhA0 = 7.5, kRhT1 = -20.0, kRhT2 = -40.0;
bool calcRH(const TCal& k, double f, double f1, double f2, double T, double& rh) {
    if (!(f2 > f1) || !(k.calH0 > 1) || !(f1 > 0)) return false;
    const double a1 = 350.0 / k.calH0;
    const double fh = (f - f1) / (f2 - f1);
    rh = 100.0 * (a1 * fh - kRhA0);
    rh += 0.0 - T / 5.5;
    if (T < kRhT1) rh *= 1.0 + (kRhT1 - T) / 100.0;
    if (T < kRhT2) rh *= 1.0 + (kRhT2 - T) / 120.0;
    if (rh < 0) rh = 0;
    if (rh > 100) rh = 100;
    return std::isfinite(rh);
}

const uint64_t kHeaderBits = [] {      // first bit sent in the top bit
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t b = (uint8_t)(kHeader[i] ^ kMask[i]);
        for (int k = 0; k < 8; k++) v = (v << 1) | ((b >> k) & 1);
    }
    return v;
}();

int popcnt64(uint64_t v) { return __builtin_popcountll(v); }

} // namespace

const uint8_t* rs41Mask() { return kMask; }

uint16_t rs41Crc16(const uint8_t* p, int n) {
    uint32_t rem = 0xFFFF;
    for (int i = 0; i < n; i++) {
        rem ^= (uint32_t)p[i] << 8;
        for (int j = 0; j < 8; j++) rem = (rem & 0x8000) ? ((rem << 1) ^ 0x1021) & 0xFFFF : (rem << 1) & 0xFFFF;
    }
    return (uint16_t)rem;
}

void rs41RsParity(const uint8_t* data, int k, uint8_t* parity24) {
    const Gf& g = gf();
    std::vector<uint8_t> r((size_t)(24 + k), 0);
    for (int i = 0; i < k; i++) r[(size_t)(24 + i)] = data[i];
    for (int i = 24 + k - 1; i >= 24; i--) {
        const uint8_t c = r[(size_t)i];
        if (!c) continue;
        for (int j = 0; j <= 24; j++) r[(size_t)(i - 24 + j)] ^= g.mul(c, g.gen[j]);
    }
    for (int i = 0; i < 24; i++) parity24[i] = r[(size_t)i];
}

int rs41RsDecode(uint8_t* cw, int n) {
    const Gf& g = gf();
    if (n <= 24 || n > 255) return -1;
    uint8_t S[24];
    bool any = false;
    for (int j = 0; j < 24; j++) {
        uint8_t s = 0;
        for (int i = n - 1; i >= 0; i--) s = (uint8_t)(g.mul(s, g.exp[j]) ^ cw[i]);
        S[j] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    uint8_t C[25] = {1}, B[25] = {1}, T[25];
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int nn = 0; nn < 24; nn++) {
        uint8_t d = S[nn];
        for (int i = 1; i <= L; i++) d ^= g.mul(C[i], S[nn - i]);
        if (d == 0) { m++; continue; }
        const uint8_t coef = g.div(d, b);
        if (2 * L <= nn) {
            std::memcpy(T, C, sizeof C);
            for (int i = 0; i + m <= 24; i++) C[i + m] ^= g.mul(coef, B[i]);
            L = nn + 1 - L;
            std::memcpy(B, T, sizeof B);
            b = d; m = 1;
        } else {
            for (int i = 0; i + m <= 24; i++) C[i + m] ^= g.mul(coef, B[i]);
            m++;
        }
    }
    if (L > 12 || L == 0) return -1;
    // roots of C(x) at x = alpha^-pos
    int pos[12], np = 0;
    for (int p = 0; p < 255; p++) {
        const int e = (255 - p) % 255;
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = (uint8_t)(g.mul(v, g.exp[e]) ^ C[i]);
        if (v == 0) {
            if (np >= 12) return -1;
            pos[np++] = p;
        }
    }
    if (np != L) return -1;
    for (int i = 0; i < np; i++) if (pos[i] >= n) return -1;
    // error values: e = X * Omega(1/X) / C'(1/X), Omega = S * C mod x^24 (first consecutive root 0)
    uint8_t Om[24] = {};
    for (int i = 0; i < 24; i++) {
        uint8_t v = 0;
        for (int j = 0; j <= i && j <= L; j++) v ^= g.mul(C[j], S[i - j]);
        Om[i] = v;
    }
    uint8_t fix[12];
    for (int k = 0; k < np; k++) {
        const int p = pos[k];
        const int ex = (255 - p) % 255;                    // x = alpha^-p
        uint8_t om = 0;
        for (int i = 23; i >= 0; i--) om = (uint8_t)(g.mul(om, g.exp[ex]) ^ Om[i]);
        uint8_t dv = 0;                                    // C'(x): odd powers only
        for (int i = 1; i <= L; i += 2) dv ^= g.mul(C[i], g.exp[(ex * (i - 1)) % 255]);
        if (!dv) return -1;
        fix[k] = g.div(g.mul(g.exp[p], om), dv);
    }
    for (int k = 0; k < np; k++) cw[pos[k]] ^= fix[k];
    for (int j = 0; j < 24; j++) {                         // the result must be a codeword
        uint8_t s = 0;
        for (int i = n - 1; i >= 0; i--) s = (uint8_t)(g.mul(s, g.exp[j]) ^ cw[i]);
        if (s) { for (int k = 0; k < np; k++) cw[pos[k]] ^= fix[k]; return -1; }
    }
    return np;
}

// ------------------------------------------------------------------------------------------------ decoder

struct Rs41Decoder::Impl {
    // bit stage
    uint64_t sr = 0;
    bool collecting = false, inverted = false;
    int bitCount = 0, wantBits = 0;
    uint8_t cur = 0;
    std::vector<uint8_t> frame;
    double lastTime = -1;
    uint64_t badHeaderOk = 0;
    // calibration, per sonde
    std::string serial;
    uint8_t cal[kRs41CalFrames * 16] = {};
    bool chk[kRs41CalFrames] = {};
    int calDone = 0;
    double freqHz = 0;
    int killS = -1;
    bool sgp = false;

    void clearCal() {
        std::memset(cal, 0, sizeof cal);
        std::memset(chk, 0, sizeof chk);
        calDone = 0; freqHz = 0; killS = -1; sgp = false;
    }
};

Rs41Decoder::Rs41Decoder() : p_(std::make_unique<Impl>()) { p_->frame.assign(kRs41FrameLenExt, 0); }
Rs41Decoder::~Rs41Decoder() = default;
double Rs41Decoder::announcedFreqHz() const { return p_->freqHz; }
int Rs41Decoder::calibrationDone() const { return p_->calDone; }
uint64_t Rs41Decoder::framesBadHeaderOk() const { return p_->badHeaderOk; }

void Rs41Decoder::reset() {
    p_->sr = 0; p_->collecting = false; p_->bitCount = 0; p_->lastTime = -1;
}

bool Rs41Decoder::decodeFrame(uint8_t* f, int len, SondeFix& fix) {
    Impl& s = *p_;
    if (len != kRs41FrameLen && len != kRs41FrameLenExt) return false;
    if (std::memcmp(f, kHeader, 8) != 0) return false;
    fix = SondeFix();
    fix.type = "RS41";
    // Reed-Solomon: two interleaved codewords
    const int k = (len - 56) / 2, n = 24 + k;
    int corrected = 0;
    bool rsFail = false;
    for (int w = 0; w < 2; w++) {
        uint8_t cw[255];
        for (int i = 0; i < 24; i++) cw[i] = f[8 + 24 * w + i];
        for (int i = 0; i < k; i++) cw[24 + i] = f[56 + 2 * i + w];
        const int r = rs41RsDecode(cw, n);
        if (r < 0) { rsFail = true; continue; }
        if (r > 0) {
            corrected += r;
            for (int i = 0; i < 24; i++) f[8 + 24 * w + i] = cw[i];
            for (int i = 0; i < k; i++) f[56 + 2 * i + w] = cw[24 + i];
        }
    }
    fix.corrected = corrected;
    // blocks
    const uint8_t* st = nullptr; const uint8_t* ptu = nullptr; const uint8_t* g1 = nullptr; const uint8_t* g3 = nullptr;
    int stLen = 0, goodBlocks = 0, badBlocks = 0;
    int pos = 0x39;
    while (pos + 4 <= len) {
        const int id = f[pos], bl = f[pos + 1];
        if (id < 0x76 || id > 0x8F) break;
        if (pos + 4 + bl > len) { badBlocks++; break; }
        const bool ok = u2(f + pos + 2 + bl) == rs41Crc16(f + pos + 2, bl);
        if (!ok) { badBlocks++; pos += bl + 4; continue; }
        goodBlocks++;
        if (id == 0x79) { st = f + pos + 2; stLen = bl; }
        else if (id == 0x7A) ptu = f + pos + 2;
        else if (id == 0x7C && bl >= 6) g1 = f + pos + 2;
        else if (id == 0x7B && bl >= 21) g3 = f + pos + 2;
        pos += bl + 4;
    }
    fix.crcOk = st != nullptr && stLen >= 40 && badBlocks == 0 && goodBlocks >= 3 && !rsFail;
    if (!fix.crcOk && st == nullptr) { s.badHeaderOk++; return true; }
    if (st && stLen >= 40) {
        fix.frame = u2(st);
        std::string ser;
        for (int i = 0; i < 8; i++) { const char c = (char)st[2 + i]; if (c >= 32 && c < 127) ser.push_back(c); }
        while (!ser.empty() && ser.back() == ' ') ser.pop_back();
        fix.serial = ser;
        fix.batteryV = st[10] / 10.0;
        if (ser != s.serial) { s.clearCal(); s.serial = ser; }
        const int calfr = st[23];
        if (calfr < kRs41CalFrames && !s.chk[calfr]) {
            std::memcpy(s.cal + calfr * 16, st + 24, 16);
            s.chk[calfr] = true;
            s.calDone++;
            // rs41mod.c: 400 MHz + 40 kHz * byte 1 + the top two bits of byte 0 in 10 kHz steps
            if (calfr == 0) s.freqHz = (400000.0 + 40.0 * s.cal[3] + 10.0 * (s.cal[2] >> 6)) * 1000.0;
            if (calfr == 2) { const int kt = u2(s.cal + 32 + 7); s.killS = (kt == 0xFFFF) ? -1 : kt; }
            if (calfr == 0x21) s.sgp = s.cal[0x21F] == 'P';
        }
        fix.burstKillS = s.killS;
    }
    fix.subtype = s.sgp ? "RS41-SGP" : "RS41-SG";
    if (g1) {
        const int week = u2(g1);
        const uint32_t tow = u4(g1 + 2);
        if (week > 0 && tow < 604800000u) { fix.hasTime = true; fix.unixTime = sondegeo::gpsToUnix(week, tow / 1000.0); }
    }
    if (g3) {
        const double X = (int32_t)u4(g3) / 100.0, Y = (int32_t)u4(g3 + 4) / 100.0, Z = (int32_t)u4(g3 + 8) / 100.0;
        const double r = std::sqrt(X * X + Y * Y + Z * Z);
        fix.sats = g3[18];
        if (r > 6.3e6 && r < 6.5e6) {
            sondegeo::ecefToLla(X, Y, Z, fix.lat, fix.lon, fix.altM);
            fix.hasPos = true;
            const double vx = (int16_t)u2(g3 + 12) / 100.0, vy = (int16_t)u2(g3 + 14) / 100.0, vz = (int16_t)u2(g3 + 16) / 100.0;
            double e, nn, u;
            sondegeo::ecefVelToEnu(fix.lat, fix.lon, vx, vy, vz, e, nn, u);
            fix.hasVel = true;
            fix.hSpeed = std::hypot(e, nn);
            fix.vSpeed = u;
            double h = std::atan2(e, nn) * 180 / sondegeo::kPi;
            if (h < 0) h += 360;
            fix.headingDeg = h;
        }
    }
    // temperature and humidity from the calibration collected so far
    if (ptu && s.chk[3] && s.chk[4] && s.chk[5] && s.chk[6]) {
        uint32_t m[12];
        for (int i = 0; i < 12; i++) m[i] = u3(ptu + 3 * i);
        const TCal k2 = readCal(s.cal);
        double T;
        if (calcT(k2, m[0], m[1], m[2], T)) {
            fix.hasTemp = true; fix.tempC = T;
            double rh;
            if (s.chk[7] && calcRH(k2, m[3], m[4], m[5], T, rh)) { fix.hasHumidity = true; fix.humidity = rh; }
        }
    }
    if (s.calDone < kRs41CalFrames) fix.note = "calibrating " + std::to_string(s.calDone) + "/" + std::to_string(kRs41CalFrames);
    if (!fix.crcOk) s.badHeaderOk++;
    return true;
}

void Rs41Decoder::push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) {
    Impl& s = *p_;
    const double dt = 1.0 / 4800.0;
    if (s.lastTime >= 0 && timeSec - s.lastTime > 4 * dt) { s.collecting = false; s.sr = 0; }     // a gap: whatever was half received is lost
    s.lastTime = timeSec + (double)n * dt;
    for (size_t i = 0; i < n; i++) {
        const uint8_t bit = sym[i] & 1;
        s.sr = (s.sr << 1) | bit;
        if (!s.collecting) {
            const int dn = popcnt64(s.sr ^ kHeaderBits), di = popcnt64(s.sr ^ ~kHeaderBits);
            if (dn <= 1 || di <= 1) {
                s.collecting = true;
                s.inverted = di < dn;
                s.bitCount = 0; s.cur = 0;
                s.wantBits = (kRs41FrameLen - 8) * 8;
                std::memcpy(s.frame.data(), kHeader, 8);
            }
            continue;
        }
        const uint8_t b = s.inverted ? (uint8_t)(bit ^ 1) : bit;
        s.cur = (uint8_t)(s.cur | (b << (s.bitCount & 7)));            // LSB first
        s.bitCount++;
        if ((s.bitCount & 7) == 0) {
            const int idx = 8 + (s.bitCount >> 3) - 1;
            s.frame[(size_t)idx] = (uint8_t)(s.cur ^ kMask[idx & 63]);
            s.cur = 0;
            if (idx == 0x38 && s.frame[0x38] == 0xF0) s.wantBits = (kRs41FrameLenExt - 8) * 8;
        }
        if (s.bitCount == s.wantBits) {
            s.collecting = false;
            s.sr = 0;
            SondeFix fx;
            const int len = 8 + (s.bitCount >> 3);
            if (decodeFrame(s.frame.data(), len, fx)) out.push_back(fx);
        }
    }
}

std::unique_ptr<SondeBitDecoder> makeRs41Decoder() { return std::make_unique<Rs41Decoder>(); }

// ------------------------------------------------------------------------------------------------ builder

Rs41Cal rs41MakeCal(double freqHz, int killTimerS) {
    Rs41Cal c;
    // subframe 0: announced frequency in 10 kHz steps: 400 MHz + 40 kHz * k1 + 10 kHz * (top two bits of k0)
    const int steps = (int)std::lround((freqHz / 1000.0 - 400000.0) / 10.0);
    c.bytes[2] = (uint8_t)((steps & 3) << 6); c.bytes[3] = (uint8_t)(steps >> 2);
    // subframe 2: kill timer
    const int kt = killTimerS < 0 ? 0xFFFF : std::min(killTimerS, 0xFFFE);
    c.bytes[32 + 7] = (uint8_t)(kt & 255); c.bytes[32 + 8] = (uint8_t)(kt >> 8);
    // temperature: reference resistors, T(R) polynomial (Pt1000 fit over -95..+50 C), scale, offset, gain
    putF32(c.bytes + kOfRf1, 750.f); putF32(c.bytes + kOfRf2, 1100.f);
    putF32(c.bytes + kOfCo1, -245.38665f); putF32(c.bytes + kOfCo1 + 4, 0.234898627f); putF32(c.bytes + kOfCo1 + 8, 1.04790903e-5f);
    putF32(c.bytes + kOfCalT1, 1.0f); putF32(c.bytes + kOfCalT1 + 4, 0.f); putF32(c.bytes + kOfCalT1 + 8, 0.f);
    putF32(c.bytes + kOfCalH, 42.0f); putF32(c.bytes + kOfCalH + 4, 0.f);
    return c;
}

namespace {
constexpr uint32_t kTf1 = 132630, kTf2 = 193349, kHf1 = 464547, kHf2 = 532098;     // reference counts of the example frame in rs41.txt
}

void rs41PtuCounts(const Rs41Cal& cal, double tempC, double rh, uint32_t meas[12]) {
    const TCal k = readCal(cal.bytes);
    for (int i = 0; i < 12; i++) meas[i] = 0;
    // temperature: invert p0 + p1 R + p2 R^2 + c1 = T / (1 + c2) for R (the root near the sensor's range)
    const double target = tempC / (1.0 + k.ca[2]) - k.ca[1] - k.co[0];
    double R = (target) / k.co[1];
    for (int i = 0; i < 20; i++) R -= (k.co[1] * R + k.co[2] * R * R - target) / (k.co[1] + 2 * k.co[2] * R);
    const double Rc = R / k.ca[0];
    const double g = (double)(kTf2 - kTf1) / (k.rf2 - k.rf1);
    const double Rb = ((double)kTf1 * k.rf2 - (double)kTf2 * k.rf1) / (double)(kTf2 - kTf1);
    const double fT = g * (Rc + Rb);
    meas[0] = (uint32_t)std::lround(fT); meas[1] = kTf1; meas[2] = kTf2;
    // humidity: undo the temperature terms of get_RHemp, then fh
    double fac = 1.0;
    if (tempC < kRhT1) fac *= 1.0 + (kRhT1 - tempC) / 100.0;
    if (tempC < kRhT2) fac *= 1.0 + (kRhT2 - tempC) / 120.0;
    const double raw = rh / fac;                        // 100 (a1 fh - a0) - T / 5.5
    const double a1 = 350.0 / k.calH0;
    const double fh = ((raw + tempC / 5.5) / 100.0 + kRhA0) / a1;
    meas[3] = (uint32_t)std::lround(kHf1 + fh * (double)(kHf2 - kHf1)); meas[4] = kHf1; meas[5] = kHf2;
    meas[6] = meas[0]; meas[7] = kTf1; meas[8] = kTf2;      // the humidity sensor's own temperature: the same here
}

std::vector<uint8_t> rs41Frame(const SondeTruth& t, const Rs41Cal& cal, int subframe) {
    std::vector<uint8_t> f(kRs41FrameLen, 0);
    std::memcpy(f.data(), kHeader, 8);
    f[0x38] = 0x0F;
    auto block = [&](int pos, int id, int len) {            // data is written by the caller at pos + 2; returns the CRC fixer
        f[(size_t)pos] = (uint8_t)id; f[(size_t)pos + 1] = (uint8_t)len;
    };
    auto seal = [&](int pos) {
        const int len = f[(size_t)pos + 1];
        const uint16_t c = rs41Crc16(f.data() + pos + 2, len);
        f[(size_t)pos + 2 + len] = (uint8_t)(c & 255); f[(size_t)pos + 3 + len] = (uint8_t)(c >> 8);
    };
    // 0x79 status
    block(0x39, 0x79, 0x28);
    uint8_t* d = &f[0x3B];
    d[0] = (uint8_t)(t.frame & 255); d[1] = (uint8_t)((t.frame >> 8) & 255);
    for (int i = 0; i < 8; i++) d[2 + i] = i < (int)t.serial.size() ? (uint8_t)t.serial[(size_t)i] : (uint8_t)' ';
    d[10] = (uint8_t)std::lround(std::clamp(t.batteryV, 0.0, 25.0) * 10);
    static const uint8_t kMid[12] = {0x00, 0x00, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x2f, 0x00, 0x07, 0x32};   // as in the example frame
    std::memcpy(d + 11, kMid, 12);
    d[23] = (uint8_t)(subframe % kRs41CalFrames);
    std::memcpy(d + 24, cal.bytes + (subframe % kRs41CalFrames) * 16, 16);
    seal(0x39);
    // 0x7A PTU
    block(0x65, 0x7A, 42);
    uint32_t m[12];
    rs41PtuCounts(cal, t.tempC, t.humidity, m);
    for (int i = 0; i < 12; i++) { f[0x67 + 3 * (size_t)i] = (uint8_t)m[i]; f[0x68 + 3 * (size_t)i] = (uint8_t)(m[i] >> 8); f[0x69 + 3 * (size_t)i] = (uint8_t)(m[i] >> 16); }
    seal(0x65);
    // 0x7C GPS time and satellite table
    block(0x93, 0x7C, 30);
    const double gps = t.unixTime + sondegeo::kGpsMinusUtcS - sondegeo::kGpsEpochUnix;
    const int week = (int)std::floor(gps / 604800.0);
    const uint32_t towMs = (uint32_t)std::llround((gps - week * 604800.0) * 1000.0);
    f[0x95] = (uint8_t)(week & 255); f[0x96] = (uint8_t)(week >> 8);
    for (int i = 0; i < 4; i++) f[0x97 + (size_t)i] = (uint8_t)(towMs >> (8 * i));
    for (int i = 0; i < 12; i++) {
        if (i < t.sats) { f[0x9B + 2 * (size_t)i] = (uint8_t)(1 + 2 * i); f[0x9C + 2 * (size_t)i] = (uint8_t)(0xB0 + i); }
        else { f[0x9B + 2 * (size_t)i] = 0xFF; f[0x9C + 2 * (size_t)i] = 0x00; }
    }
    seal(0x93);
    // 0x7D raw measurements: not used by the receiver, zeros
    block(0xB5, 0x7D, 89);
    seal(0xB5);
    // 0x7B position and velocity
    block(0x112, 0x7B, 21);
    double x, y, z;
    sondegeo::llaToEcef(t.lat, t.lon, t.altM, x, y, z);
    double vx, vy, vz;
    const double hd = t.headingDeg * sondegeo::kPi / 180;
    sondegeo::enuVelToEcef(t.lat, t.lon, t.hSpeed * std::sin(hd), t.hSpeed * std::cos(hd), t.vSpeed, vx, vy, vz);
    const int32_t pos3[3] = {(int32_t)std::llround(x * 100), (int32_t)std::llround(y * 100), (int32_t)std::llround(z * 100)};
    for (int a = 0; a < 3; a++) for (int i = 0; i < 4; i++) f[0x114 + 4 * (size_t)a + (size_t)i] = (uint8_t)((uint32_t)pos3[a] >> (8 * i));
    const int16_t v3[3] = {(int16_t)std::clamp(std::lround(vx * 100), -32767L, 32767L), (int16_t)std::clamp(std::lround(vy * 100), -32767L, 32767L), (int16_t)std::clamp(std::lround(vz * 100), -32767L, 32767L)};
    for (int a = 0; a < 3; a++) { f[0x120 + 2 * (size_t)a] = (uint8_t)((uint16_t)v3[a] & 255); f[0x121 + 2 * (size_t)a] = (uint8_t)((uint16_t)v3[a] >> 8); }
    f[0x126] = (uint8_t)t.sats; f[0x127] = 0x04; f[0x128] = 0x10;
    seal(0x112);
    // 0x76 padding block
    block(0x12B, 0x76, 17);
    seal(0x12B);
    // Reed-Solomon parity
    const int k = (kRs41FrameLen - 56) / 2;
    for (int w = 0; w < 2; w++) {
        uint8_t data[231];
        for (int i = 0; i < k; i++) data[i] = f[(size_t)(56 + 2 * i + w)];
        rs41RsParity(data, k, &f[(size_t)(8 + 24 * w)]);
    }
    return f;
}

std::vector<uint8_t> rs41Symbols(const std::vector<uint8_t>& frame, bool preamble) {
    std::vector<uint8_t> s;
    s.reserve(frame.size() * 8 + 320);
    if (preamble) for (int i = 0; i < 320; i++) s.push_back((uint8_t)(i & 1));
    for (size_t i = 0; i < frame.size(); i++) {
        const uint8_t b = (uint8_t)(frame[i] ^ kMask[i & 63]);
        for (int k = 0; k < 8; k++) s.push_back((uint8_t)((b >> k) & 1));
    }
    return s;
}

} // namespace dect2
