// See wxsat_proto.h. Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors), code written for OnAir.
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/ccsds/ccsds.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

const WxProfileInfo& wxProfile(int p) {
    static const WxProfileInfo t[kWxNumProfiles] = {
        {"noaaapt", "NOAA APT", kWxFirstFamily + 0, 137.1, 1.024e6, 60e3, 0, "APT", true},
        {"noaahrpt", "NOAA HRPT", kWxFirstFamily + 1, 1698.0, 6e6, 3e6, 2 * 665400.0, "AVHRR", true},
        {"noaagac", "NOAA GAC", kWxFirstFamily + 2, 1702.5, 8e6, 6e6, 2661600.0, "AVHRR GAC", true},
        {"noaadsb", "NOAA DSB", kWxFirstFamily + 3, 137.77, 1.024e6, 40e3, 2 * 8320.0, "HIRS", true},
        {"meteorhrpt", "Meteor HRPT", kWxFirstFamily + 4, 1700.0, 6e6, 3e6, 2 * 665400.0, "MSU-MR", false},
        {"elektrordas", "Elektro-L RDAS", kWxFirstFamily + 5, 7500.0, 45e6, 40e6, 30720000.0, "MSU-GS IR", false},
        {"arktikardas", "Arktika-M RDAS", kWxFirstFamily + 6, 7865.0, 45e6, 40e6, 30720000.0, "MSU-GS IR", false},
        {"arktikaggak", "Arktika-M GGAK", kWxFirstFamily + 7, 1703.0, 1.024e6, 20e3, 5000.0, "GGAK", false},
        {"awsl", "AWS L-band", kWxFirstFamily + 8, 1707.0, 6e6, 4e6, 1785000.0, "MWR", false},
    };
    return t[p < 0 || p >= kWxNumProfiles ? 0 : p];
}

namespace wxsat {

int gacPn(int i) {
    static const std::vector<uint8_t> tab = [] {
        std::vector<uint8_t> s = {0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0};
        s.resize(1023);
        for (int n = 11; n < 1022; n++) s[(size_t)n] = s[(size_t)n - 1] ^ s[(size_t)n - 5] ^ s[(size_t)n - 6] ^ s[(size_t)n - 8] ^ s[(size_t)n - 11];
        return s;
    }();
    return tab[(size_t)(i % 1023)];
}

void gacScramble(uint8_t* f) {
    for (int b = 60; b < kGacBytes * 8; b++)
        if (gacPn(b - 60)) f[b >> 3] ^= (uint8_t)(0x80 >> (b & 7));
}

void unpack10(const uint8_t* in, int n, uint16_t* w) {
    size_t bit = 0;
    for (int i = 0; i < n; i++) {
        uint16_t v = 0;
        for (int b = 0; b < 10; b++, bit++) v = (uint16_t)(v << 1 | (in[bit >> 3] >> (7 - (bit & 7)) & 1));
        w[i] = v;
    }
}

void pack10(const uint16_t* w, int n, uint8_t* out) {
    size_t bit = 0;
    for (int i = 0; i < n; i++)
        for (int b = 9; b >= 0; b--, bit++) if (w[i] >> b & 1) out[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
}

// ------------------------------------------------------------ Deframer
void Deframer::configure(uint64_t sync, int syncBits, int frameBits, int tolAcq, int tolTrack, bool allowInverted) {
    sync_ = sync; syncBits_ = syncBits; frameBits_ = frameBits; tolAcq_ = tolAcq; tolTrack_ = tolTrack; allowInv_ = allowInverted;
    mask_ = syncBits >= 64 ? ~0ull : (1ull << syncBits) - 1;
    frame_.assign((size_t)(frameBits + 7) / 8, 0);
    reset();
}

void Deframer::reset() { lock_ = false; inv_ = false; pos_ = 0; misses_ = 0; sr_ = 0; hits = 0; }

void Deframer::start() {
    std::fill(frame_.begin(), frame_.end(), 0);
    for (int b = 0; b < syncBits_; b++) if (sync_ >> (syncBits_ - 1 - b) & 1) frame_[(size_t)b >> 3] |= (uint8_t)(0x80 >> (b & 7));
    pos_ = syncBits_;
}

void Deframer::push(int bit) {
    sr_ = sr_ << 1 | (uint64_t)(bit & 1);
    if (lock_ && pos_ >= syncBits_) {
        if (((bit & 1) ^ (inv_ ? 1 : 0)) != 0) frame_[(size_t)pos_ >> 3] |= (uint8_t)(0x80 >> (pos_ & 7));
        if (++pos_ == frameBits_) { if (onFrame) onFrame(frame_.data()); pos_ = 0; }
        return;
    }
    const int e = __builtin_popcountll((sr_ ^ sync_) & mask_), ei = allowInv_ ? syncBits_ - e : syncBits_ + 1;
    if (lock_) {
        if (++pos_ < syncBits_) return;
        const int m = std::min(e, ei);
        if (m <= tolTrack_) { misses_ = 0; inv_ = ei < e; hits++; start(); return; }
        if (++misses_ > 3) { lock_ = false; pos_ = 0; return; }
        start();   // keep flywheeling on the expected frame position
        return;
    }
    if (e <= tolAcq_ || ei <= tolAcq_) { lock_ = true; inv_ = ei < e; hits++; misses_ = 0; start(); }
}

// ------------------------------------------------------------ TIP / HIRS
static constexpr int kHirsPos[36] = {16, 17, 22, 23, 26, 27, 30, 31, 34, 35, 38, 39, 42, 43, 54, 55, 58, 59, 62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 84, 85, 88, 89, 92, 93};
static constexpr int kHirsCh[20] = {0, 16, 1, 2, 12, 3, 17, 10, 18, 6, 7, 19, 9, 13, 5, 4, 14, 11, 15, 8};

bool HirsDecoder::push(const uint8_t* b, std::vector<uint16_t>& out, int& outLine) {
    uint8_t d[36];
    for (int i = 0; i < 36; i++) d[i] = b[kHirsPos[i]];
    const int el = (d[2] % 32) << 1 | d[3] >> 7;
    const int enc = d[0];
    const bool valid = el < kHirsWidth && (d[35] & 2) != 0;
    bool done = false;
    if (valid) {
        auxCounter++;
        // 13-bit words start two bits into byte 3 (33 bytes)
        uint8_t tmp[33];
        for (int i = 0; i < 33; i++) tmp[i] = (uint8_t)(d[3 + i] << 2 | (i + 1 < 33 ? d[3 + i + 1] >> 6 : 0));
        size_t bit = 0;
        for (int w = 0; w < 20; w++) {
            int v = 0;
            for (int k = 0; k < 13; k++, bit++) v = v << 1 | (tmp[bit >> 3] >> (7 - (bit & 7)) & 1);
            if (enc < 57 || enc == 68 || enc == 156 || enc == 59 || enc == 99) v = (v >> 12) == 1 ? (v & 0xFFF) + 4096 : 4096 - (v & 0xFFF);
            cur[(size_t)kHirsCh[w] * kHirsWidth + (size_t)(55 - el)] = (uint16_t)v;
        }
        haveAny = true;
    }
    const int next = (b[22] % 32) << 1 | b[23] >> 7;
    if (haveAny && (next == 55 || (enc == 0 && auxCounter > 10))) {
        out = cur;
        outLine = line++;
        std::fill(cur.begin(), cur.end(), 0);
        auxCounter = 0;
        haveAny = false;
        done = true;
    }
    return done;
}

void encodeHirsTip(uint8_t t[kDsbBytes], int scid, int minor, int el, const uint16_t words[kHirsChannels]) {
    std::memset(t, 0x55, kDsbBytes);
    t[0] = 0xED; t[1] = 0xE2; t[2] = (uint8_t)(scid & 15); t[4] = (uint8_t)(minor >> 8 & 1); t[5] = (uint8_t)minor;
    uint8_t d[36] = {};
    d[0] = 10;                                // encoder position: Earth view, data as sign and magnitude
    d[2] = (uint8_t)(el >> 1);
    d[3] = (uint8_t)((el & 1) << 7);
    size_t bit = 0;
    uint8_t tmp[33] = {};
    for (int w = 0; w < 20; w++)
        for (int k = 12; k >= 0; k--, bit++) if (words[w] >> k & 1) tmp[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    for (int i = 0; i < 33; i++) { d[3 + i] |= (uint8_t)(tmp[i] >> 2); if (i + 1 < 33) d[3 + i + 1] |= (uint8_t)(tmp[i] << 6); }
    d[35] |= 2;
    for (int i = 0; i < 36; i++) t[kHirsPos[i]] = d[i];
    const int next = el;                      // the next element number sits at byte 22/23 too (its own element in this copy)
    t[22] = (uint8_t)((d[2] & 0xE0) | (next >> 1));
    t[23] = (uint8_t)((d[3] & 0x7F) | ((next & 1) << 7));
}

uint16_t wxTestPixel(int c, int x, int y) {
    const double fx = x / 512.0, fy = (y % 64) / 64.0;
    double v;
    switch (c % 3) {
    case 0: v = 150 + 500 * std::fmod(fx, 1.0) + 100 * std::sin(fy * 6.283); break;
    case 1: v = 200 + 500 * fy + 100 * std::cos(fx * 25.13); break;
    default: v = 512 + 350 * std::cos((x % 256) / 40.0 + (y % 64) / 10.0); break;
    }
    return (uint16_t)std::clamp((int)std::lround(v), 0, 1023);
}

void noaaTipBlocks(const uint16_t* w, bool gac, const std::function<void(const uint8_t*)>& out) {
    uint8_t f[kDsbBytes];
    if (!gac) {
        const int n = (w[6] >> 7) & 3;
        if (n != 1) return;
    }
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < kDsbBytes; j++) f[j] = (uint8_t)(w[104 * (i + 1) + j - 1] >> 2);
        out(f);
    }
}

int noaaScid(const uint16_t* w) { return (w[6] & 0x078) >> 3 & 15; }
const char* noaaSatName(int id) { return id == 7 ? "NOAA-15" : id == 13 ? "NOAA-18" : id == 15 ? "NOAA-19" : ""; }

void gacDecodeLine(const uint16_t* w, std::vector<uint16_t>& px) {
    px.assign((size_t)kHrptChannels * kGacWidth, 0);
    for (int c = 0; c < kHrptChannels; c++)
        for (int x = 0; x < kGacWidth; x++) px[(size_t)c * kGacWidth + (size_t)x] = w[kGacImageWord + c + x * kHrptChannels];
}

void gacBuildFrame(int line, int scid, uint8_t out[kGacBytes]) {
    std::vector<uint16_t> w((size_t)kGacWords, 0);
    w[0] = 0x284; w[1] = 0x16F; w[2] = 0x35C; w[3] = 0x19D; w[4] = 0x20F; w[5] = 0x095;
    w[6] = (uint16_t)((scid & 15) << 3 | 1 << 7);
    for (int c = 0; c < kHrptChannels; c++)
        for (int x = 0; x < kGacWidth; x++) w[(size_t)kGacImageWord + (size_t)c + (size_t)x * kHrptChannels] = wxTestPixel(c, x, line);
    for (int i = 0; i < 5; i++) {
        uint8_t tip[kDsbBytes];
        uint16_t h[kHirsChannels];
        const int minor = (line * 5 + i) % 320, el = (line * 5 + i) % kHirsWidth;
        for (int k = 0; k < kHirsChannels; k++) h[k] = (uint16_t)(0x1000 | wxTestPixel(k, el, line));
        encodeHirsTip(tip, scid, minor, el, h);
        for (int j = 0; j < kDsbBytes; j++) w[(size_t)(104 * (i + 1) + j - 1)] = (uint16_t)(tip[j] << 2);
    }
    std::memset(out, 0, kGacBytes);
    pack10(w.data(), kGacWords, out);
    gacScramble(out);
}

// ------------------------------------------------------------ Meteor HRPT
static constexpr int kMetOff[4] = {22, 278, 534, 790};
static constexpr int kMetLen[4] = {238, 238, 238, 234};

void meteorPayload(const uint8_t c[kCadu], uint8_t out[kMeteorPayload]) {
    int p = 0;
    for (int s = 0; s < 4; s++) { std::memcpy(out + p, c + kMetOff[s], (size_t)kMetLen[s]); p += kMetLen[s]; }
}

void meteorBuildCadu(const uint8_t pl[kMeteorPayload], uint32_t counter, uint8_t c[kCadu]) {
    std::memset(c, 0x55, kCadu);
    c[0] = 0x1A; c[1] = 0xCF; c[2] = 0xFC; c[3] = 0x1D;
    c[4] = (uint8_t)(counter >> 16); c[5] = (uint8_t)(counter >> 8); c[6] = (uint8_t)counter;
    int p = 0;
    for (int s = 0; s < 4; s++) { std::memcpy(c + kMetOff[s], pl + p, (size_t)kMetLen[s]); p += kMetLen[s]; }
}

void msuMrDecodeFrame(const uint8_t* b, std::vector<uint16_t>& px) {
    px.assign((size_t)kMsuMrChannels * kMsuMrWidth, 0);
    for (int ch = 0; ch < kMsuMrChannels; ch++)
        for (int l = 0; l < 393; l++) {
            const uint8_t* p = b + 50 + l * 30 + ch * 5;
            uint16_t* o = &px[(size_t)ch * kMsuMrWidth + (size_t)l * 4];
            o[0] = (uint16_t)(p[0] << 2 | p[1] >> 6);
            o[1] = (uint16_t)((p[1] & 63) << 4 | p[2] >> 4);
            o[2] = (uint16_t)((p[2] & 15) << 6 | p[3] >> 2);
            o[3] = (uint16_t)((p[3] & 3) << 8 | p[4]);
        }
}

void msuMrBuildFrame(int line, std::vector<uint8_t>& f) {
    f.assign((size_t)kMsuMrFrame, 0);
    for (int b = 0; b < 8; b++) f[(size_t)b] = (uint8_t)(kMsuSync >> (56 - 8 * b));
    f[8] = (uint8_t)(10 + line / 3600); f[9] = (uint8_t)(line / 60 % 60); f[10] = (uint8_t)(line % 60);
    for (int ch = 0; ch < kMsuMrChannels; ch++)
        for (int l = 0; l < 393; l++) {
            uint8_t* p = &f[(size_t)(50 + l * 30 + ch * 5)];
            uint16_t v[4];
            for (int i = 0; i < 4; i++) v[i] = wxTestPixel(ch, l * 4 + i, line);
            p[0] = (uint8_t)(v[0] >> 2);
            p[1] = (uint8_t)((v[0] & 3) << 6 | v[1] >> 4);
            p[2] = (uint8_t)((v[1] & 15) << 4 | v[2] >> 6);
            p[3] = (uint8_t)((v[2] & 63) << 2 | v[3] >> 8);
            p[4] = (uint8_t)v[3];
        }
}

// ------------------------------------------------------------ RDAS
int rdasVcid(const uint8_t c[kCadu]) {
    const int a = c[5] >> 1 & 7, b = c[11] >> 1 & 7;
    for (int v : {2, 3, 4, 5}) if (a == v || b == v) return v;
    return a;
}

void rdasBuildCadu(int vcid, uint32_t counter, const uint8_t pl[kRdasPayload], uint8_t c[kCadu]) {
    std::memset(c, 0, kCadu);
    c[0] = 0x1A; c[1] = 0xCF; c[2] = 0xFC; c[3] = 0x1D;
    c[4] = 0x40; c[5] = (uint8_t)(vcid << 1);
    c[6] = (uint8_t)(counter >> 16); c[7] = (uint8_t)(counter >> 8); c[8] = (uint8_t)counter;
    c[11] = (uint8_t)(vcid << 1);
    std::memcpy(c + 24, pl, kRdasPayload);
    meteor::pnXor(c + 4, kCadu - 4);
}

static void words5(const uint8_t* p, uint16_t* o) {
    o[0] = (uint16_t)(p[0] << 2 | p[1] >> 6);
    o[1] = (uint16_t)((p[1] & 63) << 4 | p[2] >> 4);
    o[2] = (uint16_t)((p[2] & 15) << 6 | p[3] >> 2);
    o[3] = (uint16_t)((p[3] & 3) << 8 | p[4]);
}
static void put5(uint8_t* p, const uint16_t* v) {
    p[0] = (uint8_t)(v[0] >> 2);
    p[1] = (uint8_t)((v[0] & 3) << 6 | v[1] >> 4);
    p[2] = (uint8_t)((v[1] & 15) << 4 | v[2] >> 6);
    p[3] = (uint8_t)((v[2] & 63) << 2 | v[3] >> 8);
    p[4] = (uint8_t)v[3];
}
static constexpr int kIrOff[7] = {113, 305, 497, 689, 881, 1073, 1265};

void msuGsIrDecode(const uint8_t* f, MsuGsIr& o) {
    o.counter = (f[8] & 31) << 8 | f[9];
    o.segment = f[10] & 63;
    o.valid = o.segment % 2 == 0 && o.segment < 36 && o.counter < kMsuGsIrLines;
    if (!o.valid) return;
    uint16_t w[1548];
    int pos = 5;
    for (int i = 0; i < 1544 && pos + 5 <= kMsuGsIrFrame; i += 4, pos += 5) words5(f + pos, w + i);
    for (int ch = 0; ch < kMsuGsIrChannels; ch++)
        for (int i = 0; i < 183; i++) o.seg[(size_t)ch * 183 + (size_t)i] = w[kIrOff[ch] + i];
}

void msuGsIrBuild(int counter, int segment, std::vector<uint8_t>& f) {
    f.assign(kMsuGsIrFrame, 0);
    for (int b = 0; b < 8; b++) f[(size_t)b] = (uint8_t)(kMsuSync >> (56 - 8 * b));
    f[8] = (uint8_t)(counter >> 8 & 31); f[9] = (uint8_t)counter; f[10] = (uint8_t)segment;
    uint16_t w[1548] = {};
    for (int ch = 0; ch < kMsuGsIrChannels; ch++)
        for (int i = 0; i < 183; i++) w[kIrOff[ch] + i] = wxTestPixel(ch, segment / 2 * 183 + i, counter);
    // the words before 112 overlap the sync word and the counter (only the receiver reads them, and ignores them)
    int pos = 5 + 112 / 4 * 5;
    for (int i = 112; i < 1544 && pos + 5 <= kMsuGsIrFrame; i += 4, pos += 5) put5(f.data() + pos, w + i);
}

int msuGsVisDecode(const uint8_t* f, std::vector<uint16_t>& a, std::vector<uint16_t>& b) {
    const int counter = (f[8] << 8 | f[9]) & 0x7FFF;
    if (counter >= kMsuGsVisLines) return -1;
    a.resize(kMsuGsVisFullWidth); b.resize(kMsuGsVisFullWidth);
    uint16_t w[12048];
    int pos = 5 * 38;
    for (int i = 0; i < 12044 && pos + 5 <= kMsuGsVisFrame; i += 4, pos += 5) words5(f + pos, w + i);
    for (int i = 0; i < kMsuGsVisFullWidth; i++) { a[(size_t)i] = w[i * 2]; b[(size_t)i] = w[i * 2 + 1]; }
    return counter;
}

void msuGsVisBuild(int counter, std::vector<uint8_t>& f) {
    f.assign(kMsuGsVisFrame, 0);
    for (int b = 0; b < 8; b++) f[(size_t)b] = (uint8_t)(kMsuSync >> (56 - 8 * b));
    f[8] = (uint8_t)(counter >> 8 & 0x7F); f[9] = (uint8_t)counter;
    std::vector<uint16_t> w(12048, 0);
    for (int i = 0; i < kMsuGsVisFullWidth; i++) { w[(size_t)i * 2] = wxTestPixel(0, i / 8, counter / 4); w[(size_t)i * 2 + 1] = wxTestPixel(1, i / 8, counter / 4); }
    int pos = 5 * 38;
    for (int i = 0; i < 12044 && pos + 5 <= kMsuGsVisFrame; i += 4, pos += 5) put5(f.data() + pos, w.data() + i);
}

// ------------------------------------------------------------ AWS
int awsDecodeCadu(const uint8_t coded[kAwsCoded], uint8_t vcdu[kAwsVcdu]) {
    uint8_t d[kAwsCoded];
    std::memcpy(d, coded, kAwsCoded);
    meteor::pnXor(d, kAwsCoded);
    int total = 0;
    for (int c = 0; c < kAwsInterleave; c++) {
        uint8_t cw[255];
        for (int i = 0; i < 255; i++) cw[i] = d[i * kAwsInterleave + c];
        const int e = meteor::rsDecode(cw, true);
        if (e < 0) return -1;
        total += e;
        for (int i = 0; i < 223; i++) vcdu[i * kAwsInterleave + c] = cw[i];
    }
    return total;
}

void awsEncodeCadu(const uint8_t vcdu[kAwsVcdu], uint8_t cadu[kAwsCadu]) {
    cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
    uint8_t* d = cadu + 4;
    for (int c = 0; c < kAwsInterleave; c++) {
        uint8_t data[223], par[32];
        for (int i = 0; i < 223; i++) data[i] = vcdu[i * kAwsInterleave + c];
        meteor::rsEncode(data, par, true);
        for (int i = 0; i < 223; i++) d[i * kAwsInterleave + c] = data[i];
        for (int i = 0; i < 32; i++) d[(223 + i) * kAwsInterleave + c] = par[i];
    }
    meteor::pnXor(d, kAwsCoded);
}

void AwsDemux::take(const uint8_t* p, size_t n) {
    buf_.insert(buf_.end(), p, p + n);
    size_t off = 0;
    while (buf_.size() - off >= 6) {
        const int len = (buf_[off + 4] << 8 | buf_[off + 5]) + 1;
        if (buf_.size() - off < 6 + (size_t)len) break;
        AwsPacket pk;
        pk.apid = (buf_[off] & 7) << 8 | buf_[off + 1];
        pk.seqFlag = buf_[off + 2] >> 6;
        pk.data.assign(buf_.begin() + (std::ptrdiff_t)off + 6, buf_.begin() + (std::ptrdiff_t)(off + 6 + (size_t)len));
        packets++;
        if (onPacket) onPacket(pk);
        off += 6 + (size_t)len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)off);
}

void AwsDemux::push(const uint8_t v[kAwsVcdu]) {
    const int64_t counter = (int64_t)v[2] << 16 | (int64_t)v[3] << 8 | v[4];
    const bool gap = lastCounter_ >= 0 && ((lastCounter_ + 1) & 0xFFFFFF) != counter;
    lastCounter_ = counter;
    const int fhp = (v[6] & 7) << 8 | v[7];
    const uint8_t* d = v + 8;
    const size_t n = kAwsVcdu - 8;
    if (gap) { if (!buf_.empty()) dropped++; buf_.clear(); have_ = false; }
    if (fhp == 0x7FF) { if (have_) take(d, n); return; }
    if (fhp >= (int)n) return;
    if (have_) take(d, (size_t)fhp);
    buf_.clear();
    have_ = true;
    take(d + fhp, n - (size_t)fhp);
}

bool MwrDecoder::push(const AwsPacket& p, std::vector<uint16_t>& px) {
    if (p.apid != 100 || p.data.size() < 19) return false;
    bool done = false;
    const auto line = [&] {
        if (wip.empty()) return;
        wip.resize(kMwrLineBytes);
        px.assign((size_t)kMwrChannels * kMwrWidth, 0);
        for (int c = 0; c < kMwrWidth; c++)
            for (int i = 0; i < kMwrChannels; i++)
                px[(size_t)i * kMwrWidth + (size_t)c] = (uint16_t)(wip[(size_t)(1799 + c * 40 + i * 2 + 1)] << 8 | wip[(size_t)(1799 + c * 40 + i * 2)]);
        lines++;
        done = true;
    };
    if (p.seqFlag == 1) { line(); wip.clear(); }
    if (p.seqFlag <= 2) wip.insert(wip.end(), p.data.begin() + 17, p.data.end() - 2);
    if (p.seqFlag == 2) { line(); wip.clear(); }
    return done;
}

void mwrBuildPackets(int ln, int vcid, int& seq, std::vector<uint8_t>& s) {
    (void)vcid;
    std::vector<uint8_t> scan(kMwrLineBytes, 0);
    for (int c = 0; c < kMwrWidth; c++)
        for (int i = 0; i < kMwrChannels; i++) {
            const int v = wxTestPixel(i, c * 8, ln) << 6;
            scan[(size_t)(1799 + c * 40 + i * 2)] = (uint8_t)v;
            scan[(size_t)(1799 + c * 40 + i * 2 + 1)] = (uint8_t)(v >> 8);
        }
    constexpr int kChunk = 1000;
    const int nSeg = (kMwrLineBytes + kChunk - 1) / kChunk;
    for (int k = 0; k < nSeg; k++) {
        const int from = k * kChunk, take = std::min(kChunk, kMwrLineBytes - from);
        const int flag = nSeg == 1 ? 3 : k == 0 ? 1 : k == nSeg - 1 ? 2 : 0;
        const int len = 17 + take + 2;
        s.push_back((uint8_t)(0x08 | 100 >> 8)); s.push_back((uint8_t)100);
        s.push_back((uint8_t)(flag << 6 | (seq >> 8 & 63))); s.push_back((uint8_t)seq);
        s.push_back((uint8_t)((len - 1) >> 8)); s.push_back((uint8_t)(len - 1));
        s.insert(s.end(), 17, 0);
        s.insert(s.end(), scan.begin() + from, scan.begin() + from + take);
        s.push_back(0); s.push_back(0);
        seq = (seq + 1) & 0x3FFF;
    }
}

void awsBuildVcdu(int vcid, uint32_t counter, std::vector<uint8_t>& s, std::vector<uint8_t>& starts, uint8_t v[kAwsVcdu]) {
    std::memset(v, 0, kAwsVcdu);
    v[0] = (uint8_t)(0x40 | (kAwsScid >> 2));
    v[1] = (uint8_t)((kAwsScid & 3) << 6 | (vcid & 63));
    v[2] = (uint8_t)(counter >> 16); v[3] = (uint8_t)(counter >> 8); v[4] = (uint8_t)counter;
    const size_t n = kAwsVcdu - 8;
    int fhp = 0x7FF;
    for (size_t i = 0; i < starts.size() && i < n; i++) if (starts[i]) { fhp = (int)i; break; }
    v[6] = (uint8_t)(fhp >> 8); v[7] = (uint8_t)fhp;
    const size_t m = std::min(n, s.size());
    std::memcpy(v + 8, s.data(), m);
    s.erase(s.begin(), s.begin() + (std::ptrdiff_t)m);
    starts.erase(starts.begin(), starts.begin() + (std::ptrdiff_t)std::min(starts.size(), m));
}

} // namespace wxsat
} // namespace dect2
