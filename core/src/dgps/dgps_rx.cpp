// DGPS beacons: MSK demodulation at 25 - 200 bit/s with a bit clock that is followed across blocks, and the RTCM SC-104 word, header and message layer (see dgps_rx.h
// for the sources).
#include "dect2/dgps/dgps_rx.h"
#include <algorithm>
#include <cmath>

namespace dect2::dgps {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint8_t kPreamble[8] = {0, 1, 1, 0, 0, 1, 1, 0};   // 0x66, MSB first

// Parity equations of the 30-bit word (IS-GPS-200 table 20-XIV, used unchanged by RTCM SC-104): each parity bit is the XOR of the listed data bits (1-based, d1 first)
// and of D29* (D25, D27, D30) or D30* (D26, D28, D29), the last two bits of the previous word.
constexpr int kParityTaps[6][15] = {
    {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23, 0},       // D25
    {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24, 0},       // D26
    {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22, 0},        // D27
    {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23, 0},        // D28
    {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24},       // D29
    {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24, 0, 0},        // D30
};
constexpr int kParityUsesD29[6] = {1, 0, 1, 0, 0, 1};

void parityOf(const uint8_t* d, int d29, int d30, uint8_t* p) {
    for (int k = 0; k < 6; k++) {
        uint8_t x = (uint8_t)(kParityUsesD29[k] ? d29 : d30);
        for (int t = 0; t < 15 && kParityTaps[k][t]; t++) x ^= d[kParityTaps[k][t] - 1];
        p[k] = x;
    }
}

// Decode one word from 30 channel bits. pol inverts the channel; d29 / d30 are the last two bits of the previous word in true polarity (the 24 data bits are inverted
// when d30 is set). The preamble is checked only on the first word of a message.
bool decodeWord(const uint8_t* raw, int pol, int d29, int d30, bool preamble, uint8_t* d, int& last29, int& last30) {
    uint8_t t[kWordBits];
    for (int i = 0; i < kWordBits; i++) t[i] = (uint8_t)(raw[i] ^ pol);
    for (int i = 0; i < 24; i++) d[i] = (uint8_t)(t[i] ^ d30);
    if (preamble)
        for (int i = 0; i < 8; i++)
            if (d[i] != kPreamble[i]) return false;
    uint8_t p[6];
    parityOf(d, d29, d30, p);
    for (int k = 0; k < 6; k++)
        if (p[k] != t[24 + k]) return false;
    last29 = t[28];
    last30 = t[29];
    return true;
}

// Value of len bits from d (MSB first) starting at index at.
int field(const uint8_t* d, size_t at, int len) {
    int v = 0;
    for (int i = 0; i < len; i++) v = (v << 1) | d[at + (size_t)i];
    return v;
}
int sfield(const uint8_t* d, size_t at, int len) {   // two's complement
    int v = field(d, at, len);
    if (v & (1 << (len - 1))) v -= 1 << len;
    return v;
}
long long sfield32(const uint8_t* d, size_t at) {
    unsigned long long v = 0;
    for (int i = 0; i < 32; i++) v = (v << 1) | d[at + (size_t)i];
    return (long long)(int32_t)(uint32_t)v;
}

// Look for a message header (two words) starting at b. Tries both channel polarities and all four values of the two bits before the message, since none is known when
// the search starts.
bool findHeader(const uint8_t* b, Header& h, int& pol, int& last29, int& last30) {
    for (pol = 0; pol < 2; pol++) {
        for (int p29 = 0; p29 < 2; p29++) {
            for (int p30 = 0; p30 < 2; p30++) {
                uint8_t d1[24], d2[24];
                int a29 = 0, a30 = 0;
                if (!decodeWord(b, pol, p29, p30, true, d1, a29, a30)) continue;
                if (!decodeWord(b + kWordBits, pol, a29, a30, false, d2, last29, last30)) continue;
                h.type = field(d1, 8, 6);
                h.stationId = field(d1, 14, 10);
                h.zcount = field(d2, 0, 13);
                h.seq = field(d2, 13, 3);
                h.length = field(d2, 16, 5);
                h.health = field(d2, 21, 3);
                return true;
            }
        }
    }
    return false;
}

// The message body from its data bits (24 per word, concatenated)
void parseBody(const std::vector<uint8_t>& bits, Message& m) {
    const size_t nb = bits.size();
    const uint8_t* d = bits.data();
    switch (m.h.type) {
    case 1: case 9:   // 40 bits per satellite: SF 1, UDRE 2, satellite 5, PRC 16, RRC 8, IOD 8 (the rest is fill)
        for (size_t at = 0; at + 40 <= nb; at += 40) {
            SatCorr s;
            s.sf = d[at] != 0;
            s.udre = field(d, at + 1, 2);
            s.sat = field(d, at + 3, 5);
            if (s.sat == 0) s.sat = 32;
            s.prcRaw = sfield(d, at + 8, 16);
            s.rrcRaw = sfield(d, at + 24, 8);
            s.iod = field(d, at + 32, 8);
            m.sats.push_back(s);
        }
        break;
    case 3:           // ECEF X, Y, Z, 32 bits each, 0.01 m
        if (nb >= 96) {
            m.x = (double)sfield32(d, 0) * 0.01; m.y = (double)sfield32(d, 32) * 0.01; m.z = (double)sfield32(d, 64) * 0.01;
            m.hasPosition = true;
            ecefToGeodetic(m.x, m.y, m.z, m.lat, m.lon, m.height);
        }
        break;
    case 5:           // one word per satellite
        for (size_t at = 0; at + 24 <= nb; at += 24) {
            SatHealth s;
            s.sat = field(d, at + 1, 5);
            if (s.sat == 0) s.sat = 32;
            s.iodLink = d[at + 6] != 0;
            s.dataHealth = field(d, at + 7, 3);
            s.cn0 = field(d, at + 10, 5);
            s.healthEnable = d[at + 15] != 0;
            s.newNavData = d[at + 16] != 0;
            s.lossWarning = d[at + 17] != 0;
            s.timeToUnhealthy = field(d, at + 18, 4);
            m.satHealth.push_back(s);
        }
        break;
    case 7: {         // three words per beacon: latitude 16, longitude 16, range 10, frequency 12, health 2, station 10, bit rate 3, modulation, sync, encoding
        static const int kRates[8] = {25, 50, 100, 110, 150, 200, 250, 300};
        for (size_t at = 0; at + 72 <= nb; at += 72) {
            Almanac a;
            a.latitude = sfield(d, at, 16) * (90.0 / 32767.0);
            a.longitude = sfield(d, at + 16, 16) * (180.0 / 32767.0);
            a.rangeKm = field(d, at + 32, 10);
            a.frequencyKhz = field(d, at + 42, 12) * 0.1 + 190.0;
            a.health = field(d, at + 54, 2);
            a.stationId = field(d, at + 56, 10);
            a.bitRate = kRates[field(d, at + 66, 3)];
            a.modMode = d[at + 69]; a.syncType = d[at + 70]; a.encoding = d[at + 71];
            m.almanac.push_back(a);
        }
        break;
    }
    case 16:          // ASCII, three characters per word, up to the first NUL
        for (size_t at = 0; at + 8 <= nb; at += 8) {
            const int c = field(d, at, 8);
            if (c == 0) break;
            m.text.push_back((char)c);
        }
        break;
    default: break;
    }
}

// Append one 24-bit data word and its parity to the channel bits. prev29 / prev30: the last two bits of the previous word in true polarity.
void putWord(uint32_t data, int pol, int& prev29, int& prev30, std::vector<uint8_t>& out) {
    uint8_t d[24], p[6];
    for (int i = 0; i < 24; i++) d[i] = (uint8_t)((data >> (23 - i)) & 1);
    parityOf(d, prev29, prev30, p);
    for (int i = 0; i < 24; i++) out.push_back((uint8_t)((d[i] ^ prev30) ^ pol));
    for (int k = 0; k < 6; k++) out.push_back((uint8_t)(p[k] ^ pol));
    prev29 = p[4];
    prev30 = p[5];
}

void pushBits(std::vector<uint32_t>& words, const std::vector<uint8_t>& bits) {
    for (size_t at = 0; at < bits.size(); at += 24) {
        uint32_t w = 0;
        for (size_t i = 0; i < 24; i++) w = (w << 1) | (at + i < bits.size() ? bits[at + i] : (i % 2 == 0 ? 1u : 0u));   // fill: alternating 1 and 0 (type 16 uses zeros, set by the caller)
        words.push_back(w);
    }
}
void putBits(std::vector<uint8_t>& b, long long v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }

}  // namespace

void ecefToGeodetic(double x, double y, double z, double& latDeg, double& lonDeg, double& h) {
    const double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    const double p = std::sqrt(x * x + y * y);
    double lat = std::atan2(z, p * (1 - e2));
    double ht = 0;
    for (int i = 0; i < 10; i++) {
        const double s = std::sin(lat), N = a / std::sqrt(1 - e2 * s * s);
        ht = p / std::cos(lat) - N;
        lat = std::atan2(z, p * (1 - e2 * N / (N + ht)));
    }
    latDeg = lat * 180.0 / kPi;
    lonDeg = std::atan2(y, x) * 180.0 / kPi;
    h = ht;
}

void Receiver::reset() {
    st_ = Stats{};
    st_.bitRate = bitRate_;
    pending_.clear();
    fbuf_.clear();
    pos_ = 0;
    timed_ = false;
    havePrev_ = false;
    haveCarrier_ = false;
    consumedBits_ = 0;
}

void Receiver::process(const std::complex<float>* iq, size_t n, double fs) {
    if (!iq || n < 2 || fs <= 0) return;
    const double sps = fs / bitRate_;
    if (sps < 4) return;

    double power = 0;
    for (size_t k = 0; k < n; k++) power += (double)iq[k].real() * iq[k].real() + (double)iq[k].imag() * iq[k].imag();
    st_.levelDb = 10.0 * std::log10(std::max(power / (double)n, 1e-20));

    // Instantaneous frequency from the phase step between samples (the first step uses the last sample of the previous block).
    const size_t before = fbuf_.size();
    for (size_t k = 0; k < n; k++) {
        const std::complex<float> a = k == 0 ? prev_ : iq[k - 1];
        if (k == 0 && !havePrev_) continue;
        const double re = (double)iq[k].real() * a.real() + (double)iq[k].imag() * a.imag();
        const double im = (double)iq[k].imag() * a.real() - (double)iq[k].real() * a.imag();
        fbuf_.push_back(std::atan2(im, re) * fs / (2.0 * kPi));
    }
    prev_ = iq[n - 1];
    havePrev_ = true;
    const size_t fresh = fbuf_.size() - before;
    if (!fresh) return;

    // The data is MSK, so the instantaneous frequency sits on two clusters either side of the carrier: the carrier is the midpoint of the two cluster means. The split
    // point is refined (k-means) until it settles, so an unbalanced bit pattern (a long run of one tone) does not pull the estimate away like a plain mean does.
    if ((double)fresh >= sps * 8) {
        double mean = 0;
        for (size_t k = before; k < fbuf_.size(); k++) mean += fbuf_[k];
        mean /= (double)fresh;
        double split = mean, mid = mean;
        for (int it = 0; it < 12; it++) {
            double hi = 0, lo = 0; size_t nh = 0, nl = 0;
            for (size_t k = before; k < fbuf_.size(); k++) { if (fbuf_[k] >= split) { hi += fbuf_[k]; nh++; } else { lo += fbuf_[k]; nl++; } }
            if (!nh || !nl) { mid = mean; break; }
            mid = 0.5 * (hi / (double)nh + lo / (double)nl);
            if (std::fabs(mid - split) < 1e-6) break;
            split = mid;
        }
        st_.carrierOffsetHz = haveCarrier_ ? 0.6 * st_.carrierOffsetHz + 0.4 * mid : mid;
        haveCarrier_ = true;
    }
    const double carrier = st_.carrierOffsetHz;

    std::vector<double> cum(fbuf_.size() + 1, 0.0);
    for (size_t k = 0; k < fbuf_.size(); k++) cum[k + 1] = cum[k] + (fbuf_[k] - carrier);
    auto integral = [&](double start, double len, int shift) {
        const long long a = std::llround(start) + shift, b = std::llround(start + len) + shift;
        return cum[(size_t)b] - cum[(size_t)a];
    };

    if (!timed_) {
        // Acquisition: the sample phase with the largest total |integral| over the bits of this block (the MSK tones move the frequency to one side or the other every bit).
        const size_t nbits = (size_t)std::floor((double)fbuf_.size() / sps) - 2;
        if (nbits < 8 || (double)fbuf_.size() < sps * 40) return;   // wait for more
        const size_t phases = (size_t)std::floor(sps);
        double bestE = -1;
        size_t best = 0;
        for (size_t ph = 0; ph < phases; ph++) {
            double e = 0;
            for (size_t k = 0; k + 1 < nbits; k++) e += std::fabs(integral((double)ph + 5 + (double)k * sps, sps, 0));
            if (e > bestE) { bestE = e; best = ph; }
        }
        pos_ = (double)best + 5;
        timed_ = true;
    }

    // Slice bits; the energy at boundaries shifted by -4 .. +4 samples says whether the bit clock has drifted (a sender / sound card clock difference), and the boundary
    // is moved a sample at a time.
    double E[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    while (pos_ + sps + 6 <= (double)fbuf_.size()) {
        const double v = integral(pos_, sps, 0);
        pending_.push_back(v > 0 ? 1 : 0);
        for (int s = -4; s <= 4; s++) E[s + 4] += std::fabs(integral(pos_, sps, s));
        pos_ += sps;
        st_.bitsSeen++;
    }
    int bestS = 4;
    for (int i = 0; i < 9; i++) if (E[i] > E[bestS]) bestS = i;
    if (E[bestS] > E[4] * 1.02) pos_ += (double)(bestS - 4);
    const size_t cut = (size_t)std::max<long long>(0, (long long)std::floor(pos_) - 5);
    fbuf_.erase(fbuf_.begin(), fbuf_.begin() + (std::ptrdiff_t)std::min(cut, fbuf_.size()));
    pos_ -= (double)cut;

    // Scan for messages. A header needs 60 bits; a header that promises N data words waits until they are all in.
    size_t s = 0;
    while (s + 2 * kWordBits <= pending_.size()) {
        Header h;
        int pol = 0, a29 = 0, a30 = 0;
        if (!findHeader(&pending_[s], h, pol, a29, a30)) { s++; continue; }
        const size_t total = (size_t)kWordBits * (size_t)(2 + h.length);
        if (s + total > pending_.size()) break;
        Message m;
        m.h = h;
        m.bitIndex = consumedBits_ + s;
        std::vector<uint8_t> body;
        bool allOk = true;
        for (int w = 0; w < h.length; w++) {
            uint8_t d[24];
            int b29 = 0, b30 = 0;
            if (decodeWord(&pending_[s + (size_t)kWordBits * (size_t)(2 + w)], pol, a29, a30, false, d, b29, b30)) {
                body.insert(body.end(), d, d + 24);
                a29 = b29; a30 = b30;
            } else {
                allOk = false;
                m.wordsBad++;
                // carry the chain on from the bits as received so that the next word can still be judged
                const uint8_t* r = &pending_[s + (size_t)kWordBits * (size_t)(2 + w)];
                a29 = r[28] ^ pol; a30 = r[29] ^ pol;
                body.insert(body.end(), 24, 0);
            }
        }
        st_.framesOk++;
        st_.typesSeen.insert(h.type);
        st_.last = h;
        st_.inverted = pol != 0;
        st_.wordsBad += m.wordsBad;
        if (allOk) { m.complete = true; parseBody(body, m); st_.messagesComplete++; }
        st_.messages.push_back(m);
        if (st_.messages.size() > kKeep) st_.messages.erase(st_.messages.begin());
        s += allOk ? total : (size_t)(2 * kWordBits);
    }
    consumedBits_ += std::min(s, pending_.size());
    pending_.erase(pending_.begin(), pending_.begin() + (std::ptrdiff_t)std::min(s, pending_.size()));
}

std::vector<uint8_t> encodeMessage(const Header& h, const std::vector<uint32_t>& extra, int pol) {
    std::vector<uint8_t> out;
    int p29 = 0, p30 = 0;
    // Word 1: preamble 8, type 6, station ID 10.
    uint32_t w1 = (0x66u << 16) | ((uint32_t)(h.type & 0x3F) << 10) | (uint32_t)(h.stationId & 0x3FF);
    putWord(w1, pol, p29, p30, out);
    // Word 2: Z-count 13, sequence 3, length 5, health 3.
    uint32_t w2 = ((uint32_t)(h.zcount & 0x1FFF) << 11) | ((uint32_t)(h.seq & 0x7) << 8) |
                  ((uint32_t)(h.length & 0x1F) << 3) | (uint32_t)(h.health & 0x7);
    putWord(w2, pol, p29, p30, out);
    for (uint32_t w : extra) putWord(w & 0xFFFFFFu, pol, p29, p30, out);
    return out;
}

std::vector<uint32_t> bodyType1(const std::vector<SatCorr>& sats) {
    std::vector<uint8_t> b;
    for (const SatCorr& s : sats) {
        putBits(b, s.sf ? 1 : 0, 1); putBits(b, s.udre, 2); putBits(b, s.sat & 31, 5);
        putBits(b, s.prcRaw & 0xFFFF, 16); putBits(b, s.rrcRaw & 0xFF, 8); putBits(b, s.iod & 0xFF, 8);
    }
    std::vector<uint32_t> w;
    pushBits(w, b);
    return w;
}

std::vector<uint32_t> bodyType3(double x, double y, double z) {
    std::vector<uint8_t> b;
    putBits(b, (long long)std::llround(x / 0.01) & 0xFFFFFFFFll, 32);
    putBits(b, (long long)std::llround(y / 0.01) & 0xFFFFFFFFll, 32);
    putBits(b, (long long)std::llround(z / 0.01) & 0xFFFFFFFFll, 32);
    std::vector<uint32_t> w;
    pushBits(w, b);
    return w;
}

std::vector<uint32_t> bodyType5(const std::vector<SatHealth>& sats) {
    std::vector<uint8_t> b;
    for (const SatHealth& s : sats) {
        putBits(b, 0, 1); putBits(b, s.sat & 31, 5); putBits(b, s.iodLink, 1); putBits(b, s.dataHealth, 3); putBits(b, s.cn0, 5);
        putBits(b, s.healthEnable, 1); putBits(b, s.newNavData, 1); putBits(b, s.lossWarning, 1); putBits(b, s.timeToUnhealthy, 4); putBits(b, 0, 2);
    }
    std::vector<uint32_t> w;
    pushBits(w, b);
    return w;
}

std::vector<uint32_t> bodyType7(const std::vector<Almanac>& beacons) {
    std::vector<uint8_t> b;
    for (const Almanac& a : beacons) {
        int rate = 5;
        static const int kRates[8] = {25, 50, 100, 110, 150, 200, 250, 300};
        for (int i = 0; i < 8; i++) if (kRates[i] == a.bitRate) rate = i;
        putBits(b, std::llround(a.latitude / (90.0 / 32767.0)) & 0xFFFF, 16);
        putBits(b, std::llround(a.longitude / (180.0 / 32767.0)) & 0xFFFF, 16);
        putBits(b, a.rangeKm, 10);
        putBits(b, std::llround((a.frequencyKhz - 190.0) / 0.1), 12);
        putBits(b, a.health, 2); putBits(b, a.stationId, 10); putBits(b, rate, 3);
        putBits(b, a.modMode, 1); putBits(b, a.syncType, 1); putBits(b, a.encoding, 1);
    }
    std::vector<uint32_t> w;
    pushBits(w, b);
    return w;
}

std::vector<uint32_t> bodyType16(const std::string& text) {
    std::vector<uint8_t> b;
    for (char c : text) putBits(b, (unsigned char)c, 8);
    while (b.size() % 24) b.push_back(0);   // fill is zeros
    std::vector<uint32_t> w;
    pushBits(w, b);
    return w;
}

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, float amp, double rateScale, double bitRate) {
    std::vector<std::complex<float>> out;
    const double sps = fs / (bitRate * rateScale);
    const double dev = bitRate / 4.0;
    double phase = 0;
    for (size_t k = 0; k < bits.size(); k++) {
        size_t count = (size_t)std::llround((double)(k + 1) * sps) - (size_t)std::llround((double)k * sps);
        double f = offsetHz + (bits[k] ? dev : -dev);
        for (size_t i = 0; i < count; i++) {
            out.emplace_back((float)(amp * std::cos(phase)), (float)(amp * std::sin(phase)));
            phase += 2.0 * kPi * f / fs;
        }
    }
    return out;
}

}  // namespace dect2::dgps
