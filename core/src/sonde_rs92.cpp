// RS92 frame decoder and builder (see sonde_rs92.h).
#include "dect2/sonde_rs92.h"
#include "dect2/sonde_geo.h"
#include "dect2/sonde_rs41.h"
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {

const uint8_t kHeader[6] = {0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x10};
constexpr int kHeaderSyms = 6 * 10 * 2;                 // 120 raw symbols

uint16_t u2(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u4(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// raw symbols of the header, first symbol first: Manchester of start, 8 data bits LSB first, stop
std::vector<uint8_t> headerSymbols() {
    std::vector<uint8_t> s;
    for (int i = 0; i < 6; i++) {
        uint8_t bits[10];
        bits[0] = 0;
        for (int k = 0; k < 8; k++) bits[1 + k] = (uint8_t)((kHeader[i] >> k) & 1);
        bits[9] = 1;
        for (int k = 0; k < 10; k++) { s.push_back(bits[k] ? 0 : 1); s.push_back(bits[k] ? 1 : 0); }
    }
    return s;
}
const std::vector<uint8_t>& hdrSyms() { static const std::vector<uint8_t> h = headerSymbols(); return h; }

} // namespace

struct Rs92Decoder::Impl {
    // the last 120 raw symbols as a bit history: two 64 bit words (hi holds the older part)
    uint64_t hi = 0, lo = 0;
    uint64_t pat[2] = {0, 0};                 // header pattern in the same layout
    uint64_t mask[2] = {0, 0};
    bool collecting = false, inverted = false;
    int symCount = 0, want = 0;
    std::vector<uint8_t> raw;                 // symbols after the header
    double lastTime = -1;
    // calibration (not used for values: see the header)
    uint8_t cal[kRs92CalFrames * 16] = {};
    bool chk[kRs92CalFrames] = {};
    int calDone = 0;
    double freqHz = 0;
    int tow = -1;
    std::string serial;
    Impl() {
        const auto& h = hdrSyms();
        // history bit i (0 = newest) = symbol (120 - 1 - i) of the header
        for (int i = 0; i < kHeaderSyms; i++) {
            const uint8_t b = h[(size_t)(kHeaderSyms - 1 - i)];
            if (i < 64) pat[1] |= (uint64_t)b << i; else pat[0] |= (uint64_t)b << (i - 64);
        }
        mask[1] = ~0ull;
        mask[0] = (1ull << (kHeaderSyms - 64)) - 1;
    }
    void clearCal() { std::memset(cal, 0, sizeof cal); std::memset(chk, 0, sizeof chk); calDone = 0; freqHz = 0; }
};

Rs92Decoder::Rs92Decoder() : p_(std::make_unique<Impl>()) {}
Rs92Decoder::~Rs92Decoder() = default;
double Rs92Decoder::announcedFreqHz() const { return p_->freqHz; }
int Rs92Decoder::calibrationDone() const { return p_->calDone; }
int Rs92Decoder::towSeconds() const { return p_->tow; }
void Rs92Decoder::reset() { p_->collecting = false; p_->hi = p_->lo = 0; p_->lastTime = -1; p_->raw.clear(); }

bool Rs92Decoder::decodeFrame(uint8_t* f, SondeFix& fix) {
    Impl& s = *p_;
    if (std::memcmp(f, kHeader, 6) != 0) return false;
    fix = SondeFix();
    fix.type = "RS92";
    // Reed-Solomon: message 210 bytes from 6, parity 24 bytes from 216
    uint8_t cw[234];
    for (int i = 0; i < 24; i++) cw[i] = f[216 + i];
    for (int i = 0; i < 210; i++) cw[24 + i] = f[6 + i];
    const int r = rs41RsDecode(cw, 234);
    bool rsFail = false;
    if (r < 0) rsFail = true;
    else if (r > 0) {
        for (int i = 0; i < 24; i++) f[216 + i] = cw[i];
        for (int i = 0; i < 210; i++) f[6 + i] = cw[24 + i];
    }
    fix.corrected = r > 0 ? r : 0;
    // blocks
    const uint8_t* cfg = nullptr; const uint8_t* gps = nullptr;
    int good = 0, bad = 0, cfgLen = 0, gpsLen = 0;
    int pos = 6;
    while (pos + 4 <= 216) {
        const int id = f[pos], bytes = 2 * f[pos + 1];
        if (id < 0x65 || id > 0x6F) break;
        if (pos + 4 + bytes > 216) { bad++; break; }
        if (u2(f + pos + 2 + bytes) == rs41Crc16(f + pos + 2, bytes)) {
            good++;
            if (id == 0x65) { cfg = f + pos + 2; cfgLen = bytes; }
            else if (id == 0x67) { gps = f + pos + 2; gpsLen = bytes; }
        } else bad++;
        pos += bytes + 4;
    }
    fix.crcOk = cfg && cfgLen >= 32 && gps && gpsLen >= 0x62 - 0x48 + 12 * 8 && bad == 0 && good >= 3 && !rsFail;
    if (cfg && cfgLen >= 32) {
        fix.frame = u2(cfg);
        std::string ser;
        for (int i = 0; i < 8; i++) { const char c = (char)cfg[4 + i]; if (c >= 32 && c < 127) ser.push_back(c); }
        while (!ser.empty() && ser.back() == ' ') ser.pop_back();
        fix.serial = ser;
        if (ser != s.serial) { s.clearCal(); s.serial = ser; }
        const int calfr = cfg[15];
        if (fix.crcOk && calfr < kRs92CalFrames && !s.chk[calfr]) {
            std::memcpy(s.cal + calfr * 16, cfg + 16, 16);
            s.chk[calfr] = true; s.calDone++;
            if (calfr == 0) {
                const int v = u2(s.cal + 2);
                s.freqHz = (v > 0 && v <= 1000) ? (400000.0 + 10.0 * v) * 1000.0 : 0.0;
            }
        }
    }
    if (gps && gpsLen >= 0x62 - 0x48 + 96) {
        const uint32_t towMs = u4(gps);
        if (towMs < 604800000u && fix.crcOk) s.tow = (int)(towMs / 1000);
        int tracked = 0;
        for (int i = 0; i < 12; i++) if (gps[0x56 - 0x48 + i] & 0x0F) tracked++;
        fix.sats = tracked;
    }
    fix.subtype = s.freqHz >= 1.5e9 ? "RS92-NGP" : "RS92-SGP";
    // no position, no temperature: see the header
    fix.note = s.calDone < kRs92CalFrames ? "calibrating " + std::to_string(s.calDone) + "/" + std::to_string(kRs92CalFrames) + "; no position (needs GPS ephemeris)"
                                          : "no position (needs GPS ephemeris)";
    return true;
}

void Rs92Decoder::push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) {
    Impl& s = *p_;
    const double dt = 1.0 / 4800.0;
    if (s.lastTime >= 0 && timeSec - s.lastTime > 4 * dt) { s.collecting = false; s.hi = s.lo = 0; }
    s.lastTime = timeSec + (double)n * dt;
    for (size_t i = 0; i < n; i++) {
        const uint8_t bit = sym[i] & 1;
        if (!s.collecting) {
            s.hi = ((s.hi << 1) | (s.lo >> 63)) & s.mask[0];
            s.lo = (s.lo << 1) | bit;
            const int dn = __builtin_popcountll((s.lo ^ s.pat[1])) + __builtin_popcountll((s.hi ^ s.pat[0]) & s.mask[0]);
            const int di = (int)kHeaderSyms - dn;       // distance to the inverted pattern
            if (dn <= 6 || di <= 6) {
                s.collecting = true; s.inverted = di < dn;
                s.raw.clear();
                s.want = (kRs92FrameLen - 6) * 10 * 2;
            }
            continue;
        }
        s.raw.push_back(s.inverted ? (uint8_t)(bit ^ 1) : bit);
        if ((int)s.raw.size() == s.want) {
            s.collecting = false; s.hi = s.lo = 0;
            uint8_t frame[kRs92FrameLen];
            std::memcpy(frame, kHeader, 6);
            int framing = 0;
            for (int by = 0; by < kRs92FrameLen - 6; by++) {
                int v = 0;
                for (int k = 0; k < 10; k++) {
                    const uint8_t a = s.raw[(size_t)(by * 20 + 2 * k)], b = s.raw[(size_t)(by * 20 + 2 * k + 1)];
                    const int d = b;                    // 10 -> 0, 01 -> 1; a pair of equal symbols is an error, take the second
                    if (k == 0) { if (d != 0) framing++; }
                    else if (k == 9) { if (d != 1) framing++; }
                    else v |= d << (k - 1);
                    (void)a;
                }
                frame[6 + by] = (uint8_t)v;
            }
            SondeFix fx;
            if (decodeFrame(frame, fx)) out.push_back(fx);
        }
    }
}

std::unique_ptr<SondeBitDecoder> makeRs92Decoder() { return std::make_unique<Rs92Decoder>(); }

// ------------------------------------------------------------------------------------------------ builder

std::vector<uint8_t> rs92Frame(const Rs92Truth& t, int subframe) {
    std::vector<uint8_t> f(kRs92FrameLen, 0);
    std::memcpy(f.data(), kHeader, 6);
    auto block = [&](int pos, int id, int words) { f[(size_t)pos] = (uint8_t)id; f[(size_t)pos + 1] = (uint8_t)words; };
    auto seal = [&](int pos) {
        const int bytes = 2 * f[(size_t)pos + 1];
        const uint16_t c = rs41Crc16(f.data() + pos + 2, bytes);
        f[(size_t)pos + 2 + (size_t)bytes] = (uint8_t)(c & 255); f[(size_t)pos + 3 + (size_t)bytes] = (uint8_t)(c >> 8);
    };
    block(0x06, 0x65, 0x10);
    uint8_t* d = &f[0x08];
    d[0] = (uint8_t)(t.frame & 255); d[1] = (uint8_t)((t.frame >> 8) & 255); d[2] = 0x20; d[3] = 0x20;
    for (int i = 0; i < 8; i++) d[4 + i] = i < (int)t.serial.size() ? (uint8_t)t.serial[(size_t)i] : (uint8_t)' ';
    d[12] = 0x00; d[13] = 0x61; d[14] = 0x00;
    d[15] = (uint8_t)(subframe % kRs92CalFrames);
    if (subframe % kRs92CalFrames == 0) {
        const int v = (int)std::lround((t.freqHz / 1000.0 - 400000.0) / 10.0);
        d[16 + 2] = (uint8_t)(v & 255); d[16 + 3] = (uint8_t)(v >> 8); d[16 + 4] = 0xFF; d[16 + 5] = 0xFF; d[16 + 6] = 0x01;
    } else {
        for (int i = 0; i < 16; i++) d[16 + i] = (uint8_t)(0x37 * (subframe + 1) + 11 * i);        // calibration bytes: made up
    }
    seal(0x06);
    block(0x2A, 0x69, 0x0C);
    seal(0x2A);
    block(0x46, 0x67, 0x3D);
    const double gps = t.unixTime + sondegeo::kGpsMinusUtcS - sondegeo::kGpsEpochUnix;
    const int week = (int)std::floor(gps / 604800.0);
    const uint32_t towMs = (uint32_t)std::llround((gps - week * 604800.0) * 1000.0);
    for (int i = 0; i < 4; i++) f[0x48 + (size_t)i] = (uint8_t)(towMs >> (8 * i));
    uint64_t prn = 0;
    for (int i = 0; i < 12; i++) prn |= (uint64_t)((i < t.sats ? 1 + 2 * i : 0) & 31) << (5 * i);
    for (int i = 0; i < 8; i++) f[0x4E + (size_t)i] = (uint8_t)(prn >> (8 * i));
    for (int i = 0; i < 12; i++) f[0x56 + (size_t)i] = i < t.sats ? 0x0F : 0x00;
    seal(0x46);
    block(0xC4, 0x68, 5);
    seal(0xC4);
    static const uint8_t kTail[6] = {0xFF, 0x02, 0x02, 0x00, 0x02, 0x00};      // as in the example frame
    std::memcpy(&f[0xD2], kTail, 6);
    uint8_t par[24];
    rs41RsParity(&f[6], 210, par);
    std::memcpy(&f[216], par, 24);
    return f;
}

std::vector<uint8_t> rs92Symbols(const std::vector<uint8_t>& frame) {
    std::vector<uint8_t> s;
    for (size_t i = 0; i < frame.size(); i++) {
        uint8_t bits[10];
        bits[0] = 0;
        for (int k = 0; k < 8; k++) bits[1 + k] = (uint8_t)((frame[i] >> k) & 1);
        bits[9] = 1;
        for (int k = 0; k < 10; k++) { s.push_back(bits[k] ? 0 : 1); s.push_back(bits[k] ? 1 : 0); }
    }
    return s;
}

} // namespace dect2
