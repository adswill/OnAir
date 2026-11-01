// FY-3 AHRPT / MPT frame layers (see fy3_proto.h). The instrument formats follow SatDump's fengyun3 plug-in (GPL-3.0-or-later, https://github.com/SatDump/SatDump,
// plugins/fengyun3_support): the frame layouts, sync words and field positions are its findings; the code here is written for OnAir's own chain.
#include "dect2/fy3/fy3_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/common/bits.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::fy3 {

// --- satellites ----------------------------------------------------------------------------------------------------------------------
const Sat* sats(int& n) {
    static const Sat s[] = {{"FY-3A", 49, 32958}, {"FY-3B", 50, 37214}, {"FY-3C", 51, 39260}, {"FY-3D", 52, 43010},
                            {"FY-3E", 56, 49008}, {"FY-3F", 60, 57490}, {"FY-3G", 64, 56232}, {"FY-3H", 68, 65815}};
    n = (int)(sizeof s / sizeof s[0]);
    return s;
}
int satIndexByScid(int scid) {
    int n; const Sat* s = sats(n);
    for (int i = 0; i < n; i++) if (s[i].scid == scid) return i;
    return -1;
}

// --- instruments -----------------------------------------------------------------------------------------------------------------------
const char* instName(int i) {
    static const char* const n[InstCount] = {"-", "VIRR", "MERSI", "MERSI-II", "MERSI-LL", "MERSI-3", "MERSI-RM", "MWRI", "MWRI-2", "MWRI-RM", "MWHS", "MWTS", "ERM",
                                             "GAS", "WAAI", "X-EUVI", "WindRad", "PMR"};
    return i >= 0 && i < InstCount ? n[i] : "-";
}
bool instIsImage(int i) { return i == Virr || i == Mersi1 || i == Mersi2 || i == MersiLL || i == Mersi3 || i == MersiRM; }

int instForVcid(int scid, int vcid, int product) {
    switch (scid) {
    case 49: case 50: case 51:   // FY-3A, B, C: VIRR on the AHRPT, MERSI on the MPT
        if (vcid == 5 && product == 0) return Virr;
        if (vcid == 3 && product == 1) return Mersi1;
        return InstNone;
    case 52: return vcid == 3 ? Mersi2 : vcid == 9 ? Gas : vcid == 5 ? Waai : vcid == 10 ? Mwri : InstNone;
    case 56: return vcid == 3 ? MersiLL : vcid == 5 ? Xeuvi : (vcid == 10 || vcid == 18) ? WindRad : InstNone;
    case 60: case 68: return vcid == 3 ? Mersi3 : vcid == 42 ? Mwri2 : InstNone;
    case 64: return vcid == 38 || vcid == 47 ? Pmr : vcid == 42 ? MwriRM : vcid == 35 ? MersiRM : InstNone;
    default:
        // an unknown spacecraft: the virtual channel numbers of the commonest layouts
        if (vcid == 5 && product == 0) return Virr;
        if (vcid == 3) return product == 1 ? Mersi1 : Mersi2;
        return InstNone;
    }
}

const MersiCfg& mersiCfg(int inst) {
    static const MersiCfg c1{5, 15, 8192, 206048, 99120, 25392, 92, 6, 1e3, false};
    static const MersiCfg c2{6, 19, 8192, 1329256, 98856, 25128, 59, 6, 1e3, false};
    static const MersiCfg c3{6, 19, 8192, 1329256, 98904, 25176, 65, 6, 1e4, false};
    static const MersiCfg cll{2, 16, 6144, 419176, 74976, 19680, 146, 6, 1e4, false};
    static const MersiCfg crm{0, 8, 1560 * 4, 42864, 18816, 18816, 2, 6, 1e4, true};
    switch (inst) {
    case Mersi2: return c2;
    case Mersi3: return c3;
    case MersiLL: return cll;
    case MersiRM: return crm;
    default: return c1;
    }
}

int instChannels(int inst) {
    if (inst == Virr) return kVirrChannels;
    if (inst == Mersi1 || inst == Mersi2 || inst == Mersi3 || inst == MersiLL || inst == MersiRM) {
        const MersiCfg& c = mersiCfg(inst);
        return c.ch250 + c.ch1000;
    }
    return 0;
}
int instWidth(int inst) {
    if (inst == Virr) return kVirrWidth;
    if (inst == Mersi1 || inst == Mersi2 || inst == Mersi3 || inst == MersiLL || inst == MersiRM) return mersiCfg(inst).width250 / 4;
    return 0;
}

const char* instChannelName(int inst, int ch) {
    static char buf[4][32];
    static int k = 0;
    char* b = buf[k++ & 3];
    if (inst == Virr) {
        static const char* const n[kVirrChannels] = {"1 (0.63 um red)", "2 (0.865 um)", "3 (3.74 um)", "4 (10.8 um)", "5 (12 um)", "6 (1.6 um)", "7 (0.455 um blue)", "8 (0.505 um)",
                                                      "9 (0.555 um green)", "10 (1.36 um)"};
        return ch >= 0 && ch < kVirrChannels ? n[ch] : "?";
    }
    const MersiCfg& c = mersiCfg(inst);
    if (ch < 0 || ch >= c.ch250 + c.ch1000) return "?";
    if (inst == Mersi1) {
        static const char* const n250[5] = {"1 (0.47 um blue)", "2 (0.55 um green)", "3 (0.65 um red)", "4 (0.865 um)", "5 (10.8 um)"};
        if (ch < 5) return n250[ch];
        snprintf(b, 32, "%d (1 km)", ch + 1);
        return b;
    }
    if (inst == Mersi2 || inst == Mersi3) {
        static const char* const n250[6] = {"1 (0.47 um blue)", "2 (0.55 um green)", "3 (0.65 um red)", "4 (0.865 um)", "24 (10.8 um)", "25 (12 um)"};
        if (ch < 6) return n250[ch];
        snprintf(b, 32, "%d (1 km)", ch - 6 + 5);
        return b;
    }
    if (ch < c.ch250) snprintf(b, 32, "%d (250 m)", ch + 1); else snprintf(b, 32, "%d (1 km)", ch - c.ch250 + 1);
    return b;
}

// --- Viterbi -----------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kTb = 96, kEmit = 64;
inline int parity7(unsigned v) { return dect2::bits::parity(v & 0xFFu); }
struct VitTab {
    int16_t sa[4][32], sb[4][32];   // branch signs, as hrpt_proto.cpp has them (the register's newest bit at the least significant end)
    VitTab() {
        for (int j = 0; j < 32; j++)
            for (int k = 0; k < 4; k++) {
                const int p = j | ((k & 1) ? 32 : 0), bit = k >> 1;
                const unsigned r = (unsigned)((p << 1 | bit) & 0x7F);
                sa[k][j] = parity7(r & 0x4F) ? 1 : -1;
                sb[k][j] = parity7(r & 0x6D) ? 1 : -1;
            }
    }
};
const VitTab& vt() { static const VitTab t; return t; }
typedef int16_t V8 __attribute__((vector_size(16)));
} // namespace

void RailViterbi::reset() { for (auto& v : m_) v = 0; n_ = 0; sinceNorm_ = 0; stepsTotal_ = 0; acc_ = 0; out_.clear(); }

int RailViterbi::best() const { int b = m_[0]; for (int i = 1; i < 64; i++) b = std::max<int>(b, m_[i]); return b; }

void RailViterbi::push(int a, int b) {
    const VitTab& T = vt();
    const int16_t A = (int16_t)std::clamp(a, -127, 127), B = (int16_t)std::clamp(b, -127, 127);
    alignas(16) int16_t ev[32], od[32], s0[32], s1[32];
    for (int j = 0; j < 32; j += 8) {
        V8 lo, hi, sa0, sb0, sa1, sb1, sa2, sb2, sa3, sb3;
        std::memcpy(&lo, m_ + j, 16); std::memcpy(&hi, m_ + j + 32, 16);
        std::memcpy(&sa0, T.sa[0] + j, 16); std::memcpy(&sb0, T.sb[0] + j, 16);
        std::memcpy(&sa1, T.sa[1] + j, 16); std::memcpy(&sb1, T.sb[1] + j, 16);
        std::memcpy(&sa2, T.sa[2] + j, 16); std::memcpy(&sb2, T.sb[2] + j, 16);
        std::memcpy(&sa3, T.sa[3] + j, 16); std::memcpy(&sb3, T.sb[3] + j, 16);
        const V8 c0 = lo + sa0 * A + sb0 * B, c1 = hi + sa1 * A + sb1 * B;
        const V8 c2 = lo + sa2 * A + sb2 * B, c3 = hi + sa3 * A + sb3 * B;
        const V8 m0 = c1 > c0, m1 = c3 > c2;
        const V8 e = (m0 & c1) | (~m0 & c0), o = (m1 & c3) | (~m1 & c2);
        std::memcpy(ev + j, &e, 16); std::memcpy(od + j, &o, 16);
        std::memcpy(s0 + j, &m0, 16); std::memcpy(s1 + j, &m1, 16);
    }
    alignas(16) int16_t nm[64];
    uint8_t* d = dec_[n_++];
    for (int j = 0; j < 32; j++) {
        nm[2 * j] = ev[j]; nm[2 * j + 1] = od[j];
        d[2 * j] = (uint8_t)(s0[j] & 1); d[2 * j + 1] = (uint8_t)(s1[j] & 1);
    }
    stepsTotal_++;
    if (++sinceNorm_ >= 32) {   // 32 steps grow a metric by at most 32 x 254
        sinceNorm_ = 0;
        const int16_t base = nm[0];
        acc_ += base;
        for (int s = 0; s < 64; s++) nm[s] = (int16_t)(nm[s] - base);
    }
    for (int s = 0; s < 64; s++) m_[s] = nm[s];
    if (n_ >= kTb + kEmit) traceback(kEmit, false);
}

void RailViterbi::traceback(int emit, bool all) {
    int s = 0;
    for (int i = 1; i < 64; i++) if (m_[i] > m_[s]) s = i;
    uint8_t bitsOut[160];
    for (int t = n_ - 1; t >= 0; t--) {
        bitsOut[t] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)(dec_[t][s] << 5);
    }
    const int k = all ? n_ : emit;
    out_.insert(out_.end(), bitsOut, bitsOut + k);
    for (int i = k; i < n_; i++) std::memcpy(dec_[i - k], dec_[i], 64);
    n_ -= k;
}

void RailViterbi::flush() { if (n_ > 0) traceback(0, true); }

// --- frames -----------------------------------------------------------------------------------------------------------------------------
bool decodeCadu(const uint8_t coded[1020], Cadu& o) {
    uint8_t v[meteor::kVcduBytes];
    const int e = meteor::decodeCadu(coded, v);
    if (e < 0) return false;
    o.rsErrors = e;
    parseVcdu(v, o);
    return true;
}

void parseVcdu(const uint8_t v[892], Cadu& o) {
    o.scid = (v[0] & 0x3F) << 2 | v[1] >> 6;
    o.vcid = v[1] & 0x3F;
    o.counter = (uint32_t)v[2] << 16 | (uint32_t)v[3] << 8 | v[4];
    o.fhp = (v[8] & 7) << 8 | v[9];
    std::memcpy(o.data, v + 10, 882);
}

void encodeVcdu(const Cadu& c, uint8_t cadu[1024]) {
    uint8_t v[meteor::kVcduBytes];
    std::memset(v, 0, sizeof v);
    meteor::buildVcduHeader(v, c.scid, c.vcid, c.counter, c.fhp);
    std::memcpy(v + 10, c.data, 882);
    meteor::encodeCadu(v, cadu);
}

// --- bit helpers ----------------------------------------------------------------------------------------------------------------------------
namespace {
inline uint32_t getBits(const uint8_t* d, size_t bitPos, int n) {   // n <= 24
    uint32_t v = 0;
    size_t byte = bitPos >> 3;
    const int off = (int)(bitPos & 7);
    // 4 bytes cover 24 bits at any offset
    uint64_t w = 0;
    for (int i = 0; i < 5; i++) w = w << 8 | d[byte + (size_t)i];
    v = (uint32_t)((w >> (40 - off - n)) & ((1ull << n) - 1));
    return v;
}
inline void putBits(uint8_t* d, size_t bitPos, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--, bitPos++) {
        const uint8_t m = (uint8_t)(0x80 >> (bitPos & 7));
        if (v >> i & 1) d[bitPos >> 3] |= m; else d[bitPos >> 3] &= (uint8_t)~m;
    }
}
constexpr size_t kVirrTime = 26041 * 8 + 2;   // 8 bytes of 6 bits each, the 6th one unused
} // namespace

// --- VIRR ---------------------------------------------------------------------------------------------------------------------------------
bool decodeVirrFrame(const uint8_t* f, size_t n, VirrLine& o) {
    if (n < (size_t)kVirrFrameBytes - 8) return false;
    std::vector<uint8_t> tmp;
    if (n < (size_t)kVirrFrameBytes) { tmp.assign(f, f + n); tmp.resize(kVirrFrameBytes + 8, 0); f = tmp.data(); }
    for (int i = 0; i < kVirrWidth; i++)
        for (int c = 0; c < kVirrChannels; c++)
            o.px[(size_t)c * kVirrWidth + (size_t)i] = (uint16_t)getBits(f, (size_t)kVirrWordBit + (size_t)(i * kVirrChannels + c) * 10, 10);
    // the time stamp: 7 values of 8 bits, each straddling two bytes (the bits below the top two)
    uint8_t t[8] = {};
    for (int k = 0; k < 8; k++) t[k] = (uint8_t)getBits(f, kVirrTime + (size_t)k * 8, 8);
    o.days = (uint32_t)(t[1] & 3) << 10 | (uint32_t)t[2] << 2 | t[3] >> 6;
    o.ms = (uint32_t)(t[3] & 3) << 24 | (uint32_t)t[4] << 16 | (uint32_t)t[5] << 8 | t[6];
    return true;
}

void encodeVirrFrame(const VirrLine& l, uint8_t* f) {
    std::memset(f, 0, kVirrFrameBytes + 8);
    putBits(f, 0, 30, (uint32_t)(kVirrSync >> 30)); putBits(f, 30, 30, (uint32_t)(kVirrSync & 0x3FFFFFFFu));
    // a few header bytes that are not all zero (the real ones carry housekeeping)
    for (int i = 8; i < 400; i++) f[i] = (uint8_t)(0x35 + i * 7);
    for (int i = 0; i < kVirrWidth; i++)
        for (int c = 0; c < kVirrChannels; c++)
            putBits(f, (size_t)kVirrWordBit + (size_t)(i * kVirrChannels + c) * 10, 10, l.px[(size_t)c * kVirrWidth + (size_t)i] & 0x3FF);
    const uint8_t t[7] = {0, (uint8_t)(l.days >> 10 & 3), (uint8_t)(l.days >> 2), (uint8_t)((l.days & 3) << 6 | (l.ms >> 24 & 3)), (uint8_t)(l.ms >> 16), (uint8_t)(l.ms >> 8), (uint8_t)l.ms};
    // t[3] must carry days&3 in its top two bits and ms bits 25..24 in the bottom two: as the decoder reads it
    for (int k = 1; k < 7; k++) putBits(f, kVirrTime + (size_t)k * 8, 8, t[k]);
}

// --- FrameSync ----------------------------------------------------------------------------------------------------------------------------
void FrameSync::push(const uint8_t* d, size_t n, const std::function<void(const std::vector<uint8_t>&)>& cb) {
    for (size_t i = 0; i < n; i++)
        for (int k = 7; k >= 0; k--) {
            const int bit = d[i] >> k & 1;
            shift_ = (shift_ << 1 | (uint64_t)bit) & mask_;
            if (in_) {
                acc_ = (uint8_t)(acc_ << 1 | bit);
                if (++nbits_ % 8 == 0) cur_.push_back(acc_);
                if (nbits_ == frameBits_) { cb(cur_); in_ = false; cur_.clear(); }
            }
            if (shift_ == sync_) {
                if (in_) {   // a new frame while one was running: pad the old one
                    cur_.resize((size_t)(frameBits_ / 8), 0);
                    cb(cur_);
                }
                in_ = true; cur_.clear(); nbits_ = 0; acc_ = 0;
                for (int s = syncBits_ - 1; s >= 0; s--) {
                    acc_ = (uint8_t)(acc_ << 1 | (sync_ >> s & 1));
                    if (++nbits_ % 8 == 0) cur_.push_back(acc_);
                }
            }
        }
}

void FrameSync::gap(size_t bytes) {
    if (!in_) { shift_ = 0; return; }
    const size_t room = (size_t)(frameBits_ - nbits_);
    if (bytes * 8 >= room) { cur_.resize((size_t)(frameBits_ / 8), 0); in_ = false; cur_.clear(); nbits_ = 0; shift_ = 0; return; }
    for (size_t i = 0; i < bytes * 8; i++) {
        acc_ = (uint8_t)(acc_ << 1);
        if (++nbits_ % 8 == 0) cur_.push_back(acc_);
    }
    shift_ = 0;
}

// --- MERSI ---------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr uint64_t kMersiHeadSync = 0x55AA55AA55AAull;
constexpr uint32_t kMersiScanSync = 0b0111111111111000000000000100u;   // 28 bits
inline int gridW(const MersiCfg& c) { return c.width250 / 4; }
}

MersiReader::MersiReader(int inst) : cfg_(mersiCfg(inst)) {
    const size_t w = (size_t)gridW(cfg_);
    acc_.assign((size_t)cfg_.ch250 * 10 * w, 0);
    cnt_.assign((size_t)cfg_.ch250 * 10, 0);
    km_.assign((size_t)cfg_.ch1000 * 10 * w, 0);
}

void MersiReader::push(const uint8_t* d, size_t n) {
    const int c250end = cfg_.ch250 * 40;
    for (size_t i = 0; i < n; i++)
        for (int k = 7; k >= 0; k--) {
            const int bit = d[i] >> k & 1;
            shHead_ = (shHead_ << 1 | (uint64_t)bit) & 0xFFFFFFFFFFFFull;
            shScan_ = (shScan_ << 1 | (uint32_t)bit) & 0x0FFFFFFFu;
            if (inFrame_) {
                byteAcc_ = (uint8_t)(byteAcc_ << 1 | bit);
                curBits_++;
                if (curBits_ % 8 == 0) cur_.push_back(byteAcc_);
                if (curBits_ == curSize_) { frameDone(); cur_.clear(); inFrame_ = false; curSize_ = -1; }
                if (curBits_ == 80 && !wasHead_ && cur_.size() >= 2) {
                    const int marker = cur_[0] << 2 | cur_[1] >> 6;
                    curSize_ = marker < c250end || cfg_.ch250 == 0 ? cfg_.scan250Bits : cfg_.scan1000Bits;
                    if (cfg_.ch250 == 0) curSize_ = cfg_.scan1000Bits;
                }
            }
            if (shHead_ == kMersiHeadSync && !wasHead_) {
                if (!cur_.empty()) frameDone();
                curBits_ = 0; curSize_ = cfg_.headBits; inFrame_ = true; wasHead_ = true; cur_.clear(); byteAcc_ = 0;
            } else if (shScan_ == kMersiScanSync) {
                if (!cur_.empty()) frameDone();
                curBits_ = 0; curSize_ = cfg_.ch250 ? cfg_.scan250Bits : cfg_.scan1000Bits; inFrame_ = true; wasHead_ = false; cur_.clear(); byteAcc_ = 0;
            }
        }
}

void MersiReader::gap(size_t bytes) {
    if (!inFrame_) { shHead_ = shScan_ = 0; return; }
    // zeros stand in for the lost bits; a frame that would end inside the gap ends there
    for (size_t i = 0; i < bytes * 8 && inFrame_; i++) {
        byteAcc_ = (uint8_t)(byteAcc_ << 1);
        curBits_++;
        if (curBits_ % 8 == 0) cur_.push_back(byteAcc_);
        if (curBits_ == curSize_) { frameDone(); cur_.clear(); inFrame_ = false; curSize_ = -1; }
    }
    shHead_ = shScan_ = 0;
}

void MersiReader::frameDone() {
    for (int b = curBits_; b < curSize_; b += 8) cur_.push_back(0);
    if (wasHead_) processHead(); else processScan();
}

void MersiReader::processHead() {
    if (cur_.size() < 24) return;
    const uint8_t* f = cur_.data();
    double t;
    if (!cfg_.time3g) {
        const uint8_t t0 = (uint8_t)((f[12] & 15) << 4 | f[13] >> 4), t1 = (uint8_t)((f[13] & 15) << 4 | f[11] >> 4), t2 = (uint8_t)((f[11] & 15) << 4 | f[12] >> 4),
                      t3 = (uint8_t)((f[9] & 15) << 4 | f[10] >> 4), t4 = (uint8_t)((f[10] & 15) << 4 | f[8] >> 4), t5 = (uint8_t)((f[8] & 15) << 4 | f[9] >> 4);
        const uint32_t days = (uint32_t)t0 << 8 | t1;
        const uint64_t ms = (uint64_t)t2 << 24 | (uint64_t)t3 << 16 | (uint64_t)t4 << 8 | t5;
        const uint32_t sub = (uint32_t)(f[19] & 15) << 8 | f[17];
        t = (10957.0 + days) * 86400.0 + (double)ms / cfg_.msScale + sub / 3950.0 + 12 * 3600.0;
    } else {
        const uint32_t days = (uint32_t)f[9] << 8 | f[10];
        const uint64_t ms = (uint64_t)f[11] << 24 | (uint64_t)f[12] << 16 | (uint64_t)f[13] << 8 | f[14];
        t = (10957.0 + days) * 86400.0 + (double)ms / cfg_.msScale + 12 * 3600.0;
    }
    if (!(t > 1.1e9 && t < 3.0e9)) t = 0;   // not a plausible date: unknown
    lastTime_ = t;
}

void MersiReader::emit() {
    if (!segOpen_) return;
    segOpen_ = false;
    const size_t w = (size_t)gridW(cfg_);
    MersiSegment s;
    s.number = counterSeg_;
    s.time = segTime_;
    s.lines = linesIn_;
    const int nch = cfg_.ch250 + cfg_.ch1000;
    s.px.assign((size_t)10 * (size_t)nch * w, 0);
    for (int r = 0; r < 10; r++) {
        for (int c = 0; c < cfg_.ch250; c++) {
            const int cn = cnt_[(size_t)c * 10 + (size_t)r];
            if (!cn) continue;
            const uint32_t* a = &acc_[((size_t)c * 10 + (size_t)r) * w];
            uint8_t* o = &s.px[((size_t)r * (size_t)nch + (size_t)c) * w];
            for (size_t x = 0; x < w; x++) o[x] = (uint8_t)std::min<uint32_t>(255, a[x] / (uint32_t)(4 * cn) >> 4);
        }
        for (int c = 0; c < cfg_.ch1000; c++)
            std::memcpy(&s.px[((size_t)r * (size_t)nch + (size_t)cfg_.ch250 + (size_t)c) * w], &km_[((size_t)c * 10 + (size_t)r) * w], w);
    }
    std::fill(acc_.begin(), acc_.end(), 0);
    std::fill(cnt_.begin(), cnt_.end(), 0);
    std::fill(km_.begin(), km_.end(), 0);
    linesIn_ = 0;
    segments++;
    if (onSegment) onSegment(s);
}

void MersiReader::flush() { emit(); }

void MersiReader::processScan() {
    if (cur_.size() < 4) return;
    const int marker = cur_[0] << 2 | cur_[1] >> 6;
    const int c250end = cfg_.ch250 * 40, cmax = c250end + cfg_.ch1000 * 10;
    if (marker >= cmax) return;
    if (marker == 0) {
        emit();
        counterSeg_++;
        segOpen_ = true;
        segTime_ = lastTime_;
    }
    if (!segOpen_) return;
    const size_t w = (size_t)gridW(cfg_);
    const size_t base = (size_t)cfg_.imgByte * 8 + (size_t)cfg_.imgBit;
    const bool is250 = marker < c250end;
    const size_t npx = is250 ? (size_t)cfg_.width250 : w;
    if ((base + npx * 12) / 8 + 6 > cur_.size()) return;
    const uint8_t* f = cur_.data();
    if (is250) {
        const int ch = marker / 40, line = marker % 40, row = line / 4;
        uint32_t* a = &acc_[((size_t)ch * 10 + (size_t)row) * w];
        for (size_t x = 0; x < npx; x += 2) {
            const uint32_t b = getBits(f, base + x * 12, 24);
            a[x / 4] += b >> 12;
            a[(x + 1) / 4] += b & 0xFFF;
        }
        cnt_[(size_t)ch * 10 + (size_t)row]++;
    } else {
        const int m = marker - c250end, ch = m / 10, row = m % 10;
        uint8_t* o = &km_[((size_t)ch * 10 + (size_t)row) * w];
        for (size_t x = 0; x < w; x++) o[x] = (uint8_t)(getBits(f, base + x * 12, 12) >> 4);
    }
    linesIn_++;
    linesTotal++;
}

void encodeMersiSegment(int inst, int segment, double time, const std::function<uint16_t(int, int, int)>& pixel, BitWriter& out) {
    const MersiCfg& c = mersiCfg(inst);
    const int w = gridW(c);
    // head frame: 48 bit sync, then the body with the time stamp
    out.put(kMersiHeadSync, 48);
    std::vector<uint8_t> head((size_t)c.headBits / 8, 0);
    {
        const double tt = time - 12 * 3600.0 - 10957.0 * 86400.0;
        const uint32_t days = (uint32_t)std::floor(tt / 86400.0);
        const double rem = tt - days * 86400.0;
        const uint64_t ms = (uint64_t)std::llround(rem * c.msScale);
        if (!c.time3g) {
            const uint8_t t0 = (uint8_t)(days >> 8), t1 = (uint8_t)days, t2 = (uint8_t)(ms >> 24), t3 = (uint8_t)(ms >> 16), t4 = (uint8_t)(ms >> 8), t5 = (uint8_t)ms;
            head[8] = (uint8_t)((t4 & 15) << 4 | t5 >> 4); head[9] = (uint8_t)((t5 & 15) << 4 | t3 >> 4); head[10] = (uint8_t)((t3 & 15) << 4 | t4 >> 4);
            head[11] = (uint8_t)((t1 & 15) << 4 | t2 >> 4); head[12] = (uint8_t)((t2 & 15) << 4 | t0 >> 4); head[13] = (uint8_t)((t0 & 15) << 4 | t1 >> 4);
        } else {
            head[9] = (uint8_t)(days >> 8); head[10] = (uint8_t)days;
            head[11] = (uint8_t)(ms >> 24); head[12] = (uint8_t)(ms >> 16); head[13] = (uint8_t)(ms >> 8); head[14] = (uint8_t)ms;
        }
        for (size_t i = 24; i < head.size() && i < 4000; i++) head[i] = (uint8_t)(0x20 + i * 13);
    }
    for (uint8_t b : head) out.put(b, 8);
    // scan lines, 250 m channels first
    auto scan = [&](int marker, int bits, int npx, int ch, int y0, int dy) {
        (void)dy;
        out.put(kMersiScanSync, 28);
        std::vector<uint8_t> body((size_t)bits / 8 + 2, 0);
        putBits(body.data(), 0, 10, (uint32_t)marker);
        const size_t base = (size_t)c.imgByte * 8 + (size_t)c.imgBit;
        for (int x = 0; x < npx; x++) putBits(body.data(), base + (size_t)x * 12, 12, pixel(ch, x, y0) & 0xFFF);
        for (int i = 0; i < bits; i++) out.put(body[(size_t)i >> 3] >> (7 - (i & 7)) & 1, 1);
    };
    for (int ch = 0; ch < c.ch250; ch++)
        for (int l = 0; l < 40; l++) scan(ch * 40 + l, c.scan250Bits, c.width250, ch, segment * 40 + l, 1);
    for (int ch = 0; ch < c.ch1000; ch++)
        for (int l = 0; l < 10; l++) scan(c.ch250 * 40 + ch * 10 + l, c.scan1000Bits, w, c.ch250 + ch, segment * 10 + l, 1);
}

uint16_t testPixel(int c, int x, int y) {
    const double fx = x / 2048.0, fy = (y % 64) / 64.0;
    double v;
    switch (c % 3) {
    case 0: v = 100 + 700 * fx + 100 * std::sin(fy * 6.283); break;
    case 1: v = 200 + 600 * fy + 100 * std::cos(fx * 25.13); break;
    default: { const double dx = x - 1024.0, dy = (y % 64) * 8 - 256.0; v = 512 + 350 * std::cos(std::sqrt(dx * dx + dy * dy) / 60.0); } break;
    }
    if (c >= 7) v = 1023 - v;
    if (x % 256 < 3 || y % 16 < 1) v = 940;
    return (uint16_t)std::clamp((int)std::lround(v), 0, 1023);
}

} // namespace dect2::fy3
