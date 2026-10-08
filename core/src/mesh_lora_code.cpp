// LoRa coding chain and the test signal's frame waveform. Sources are named in mesh_lora.h.
#include "dect2/mesh_lora.h"
#include <cmath>

namespace dect2 {
namespace lora {

bool autoLdro(int sf, double bwHz) { return symbolSeconds(sf, bwHz) >= 0.016 - 1e-9; }
double symbolSeconds(int sf, double bwHz) { return (double)(1 << sf) / bwHz; }

uint8_t whitening(size_t i) {
    // gr-lora_sdr tables.h lists 255 bytes; they are the 8-bit LFSR x^8 + x^6 + x^5 + x^4 + 1 started at 0xFF
    // (next = state << 1 | parity(state & 0xB8)), which this table reproduces
    static const std::vector<uint8_t> seq = [] {
        std::vector<uint8_t> s(255);
        uint8_t r = 0xFF;
        for (auto& v : s) {
            v = r;
            const uint8_t fb = (uint8_t)(__builtin_popcount(r & 0xB8) & 1);
            r = (uint8_t)((r << 1) | fb);
        }
        return s;
    }();
    return seq[i % 255];
}

uint8_t hammingEncode(uint8_t nib, int cr) {
    const int b0 = nib & 1, b1 = (nib >> 1) & 1, b2 = (nib >> 2) & 1, b3 = (nib >> 3) & 1;
    if (cr <= 5) return (uint8_t)(b0 << 4 | b1 << 3 | b2 << 2 | b3 << 1 | (b0 ^ b1 ^ b2 ^ b3));
    const int p0 = b0 ^ b1 ^ b2, p1 = b1 ^ b2 ^ b3, p2 = b0 ^ b1 ^ b3, p3 = b0 ^ b2 ^ b3;
    const int full = b0 << 7 | b1 << 6 | b2 << 5 | b3 << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3;
    return (uint8_t)(full >> (8 - cr));
}

uint8_t hammingDecode(uint8_t cw, int cr, int* errors) {
    // nearest of the 16 codewords; 4/7 and 4/8 correct one bit, 4/5 and 4/6 only detect
    int best = 0, bestD = 99, ties = 0;
    for (int v = 0; v < 16; v++) {
        const int d = __builtin_popcount((unsigned)(hammingEncode((uint8_t)v, cr) ^ cw));
        if (d < bestD) { bestD = d; best = v; ties = 0; }
        else if (d == bestD) ties++;
    }
    auto systematic = [&]() {
        const int s = cr <= 5 ? 4 : cr - 1;
        return (uint8_t)(((cw >> s) & 1) | ((cw >> (s - 1)) & 1) << 1 | ((cw >> (s - 2)) & 1) << 2 | ((cw >> (s - 3)) & 1) << 3);
    };
    if (bestD == 0) { if (errors) *errors = 0; return (uint8_t)best; }
    if (cr >= 7 && bestD == 1 && ties == 0) { if (errors) *errors = 1; return (uint8_t)best; }
    if (errors) *errors = 2;
    return systematic();
}

uint8_t headerChecksum(uint8_t n0, uint8_t n1, uint8_t n2) {
    // gr-lora_sdr header_impl.cc
    auto b = [](uint8_t n, int k) { return (n >> k) & 1; };
    const int c4 = b(n0, 3) ^ b(n0, 2) ^ b(n0, 1) ^ b(n0, 0);
    const int c3 = b(n0, 3) ^ b(n1, 3) ^ b(n1, 2) ^ b(n1, 1) ^ b(n2, 0);
    const int c2 = b(n0, 2) ^ b(n1, 3) ^ b(n1, 0) ^ b(n2, 3) ^ b(n2, 1);
    const int c1 = b(n0, 1) ^ b(n1, 2) ^ b(n1, 0) ^ b(n2, 2) ^ b(n2, 1) ^ b(n2, 0);
    const int c0 = b(n0, 0) ^ b(n1, 1) ^ b(n2, 3) ^ b(n2, 2) ^ b(n2, 1) ^ b(n2, 0);
    return (uint8_t)(c4 << 4 | c3 << 3 | c2 << 2 | c1 << 1 | c0);
}

uint16_t payloadCrc(const uint8_t* p, size_t n) {
    // gr-lora_sdr add_crc_impl.cc: CRC-16 (0x1021, initial 0) over all but the last two bytes, then XOR with those two
    uint16_t crc = 0;
    for (size_t i = 0; i + 2 < n; i++) {
        uint8_t v = p[i];
        for (int k = 0; k < 8; k++) {
            if (((crc & 0x8000) >> 8) ^ (v & 0x80)) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else crc = (uint16_t)(crc << 1);
            v = (uint8_t)(v << 1);
        }
    }
    if (n >= 1) crc ^= p[n - 1];
    if (n >= 2) crc ^= (uint16_t)(p[n - 2] << 8);
    return crc;
}

uint32_t grayDecode(uint32_t g) {
    uint32_t x = g;
    for (uint32_t s = g >> 1; s; s >>= 1) x ^= s;
    return x;
}

int dataSymbols(const Params& p, size_t len) {
    // SX1261/2 datasheet 6.1.4, explicit header
    const int de = p.ldro ? 1 : 0, crc = p.crc ? 1 : 0;
    const int num = 8 * (int)len - 4 * p.sf + 28 + 16 * crc;
    const int den = 4 * (p.sf - 2 * de);
    const int blocks = num > 0 ? (num + den - 1) / den : 0;
    return 8 + blocks * p.cr;
}

double airSeconds(const Params& p, size_t len) {
    return (p.preamble + 4.25 + dataSymbols(p, len)) * symbolSeconds(p.sf, p.bwHz);
}

namespace {
// one interleaver block: sfApp codewords of cwLen bits -> cwLen symbols (gr-lora_sdr interleaver_impl.cc, gray_demap_impl.cc)
void interleave(const uint8_t* cw, int cwLen, int sf, int sfApp, bool reduced, std::vector<uint16_t>& out) {
    const uint32_t N = 1u << sf;
    for (int i = 0; i < cwLen; i++) {
        uint32_t v = 0;
        for (int j = 0; j < sfApp; j++) {
            const int r = ((i - j - 1) % sfApp + sfApp) % sfApp;
            const uint32_t bit = (cw[r] >> (cwLen - 1 - i)) & 1;
            v |= bit << (sfApp - 1 - j);
        }
        const uint32_t s = reduced ? 4 * grayDecode(v) + 1 : grayDecode(v) + 1;
        out.push_back((uint16_t)(s % N));
    }
}
} // namespace

std::vector<uint16_t> encode(const Params& p, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> nib;
    const uint8_t n0 = (uint8_t)((n >> 4) & 0xF), n1 = (uint8_t)(n & 0xF), n2 = (uint8_t)(((p.cr - 4) << 1) | (p.crc ? 1 : 0));
    const uint8_t chk = headerChecksum(n0, n1, n2);
    nib.insert(nib.end(), {n0, n1, n2, (uint8_t)(chk >> 4), (uint8_t)(chk & 0xF)});
    for (size_t i = 0; i < n; i++) {
        const uint8_t w = payload[i] ^ whitening(i);
        nib.push_back(w & 0xF);
        nib.push_back(w >> 4);
    }
    if (p.crc) {
        const uint16_t c = payloadCrc(payload, n);
        nib.insert(nib.end(), {(uint8_t)(c & 0xF), (uint8_t)((c >> 4) & 0xF), (uint8_t)((c >> 8) & 0xF), (uint8_t)(c >> 12)});
    }
    std::vector<uint16_t> out;
    size_t k = 0;
    uint8_t cw[12];
    // header block: sf - 2 codewords at 4/8, sent at the reduced rate
    {
        const int sfApp = p.sf - 2;
        for (int r = 0; r < sfApp; r++) cw[r] = hammingEncode(k < nib.size() ? nib[k++] : 0, 8);
        interleave(cw, 8, p.sf, sfApp, true, out);
    }
    const int sfApp = p.ldro ? p.sf - 2 : p.sf;
    while (k < nib.size()) {
        for (int r = 0; r < sfApp; r++) cw[r] = hammingEncode(k < nib.size() ? nib[k++] : 0, p.cr);
        interleave(cw, p.cr, p.sf, sfApp, p.ldro, out);
    }
    return out;
}

void FrameDecoder::start(int sf, bool ldro) {
    sf_ = sf; ldro_ = ldro; n_ = 0; need_ = 8; corrected_ = 0;
    hdr_ = Header();
    bins_.clear(); nib_.clear();
}

void FrameDecoder::block(const uint16_t* bins, int cwLen, int sfApp, bool reduced) {
    const uint32_t N = 1u << sf_;
    uint8_t cw[12] = {};
    for (int i = 0; i < cwLen; i++) {
        const uint32_t x = (bins[i] + N - 1) % N;
        const uint32_t v = reduced ? gray(((x + 2) >> 2) & ((1u << sfApp) - 1)) : gray(x);
        for (int j = 0; j < sfApp; j++) {
            const int r = ((i - j - 1) % sfApp + sfApp) % sfApp;
            cw[r] |= (uint8_t)(((v >> (sfApp - 1 - j)) & 1) << (cwLen - 1 - i));
        }
    }
    for (int r = 0; r < sfApp; r++) {
        int e = 0;
        nib_.push_back(hammingDecode(cw[r], cwLen, &e));
        if (e) corrected_++;
    }
}

void FrameDecoder::push(uint16_t bin) {
    bins_.push_back(bin);
    n_++;
    if (n_ == 8) {
        block(bins_.data(), 8, sf_ - 2, true);
        const uint8_t n0 = nib_[0], n1 = nib_[1], n2 = nib_[2], n3 = nib_[3], n4 = nib_[4];
        hdr_.len = n0 << 4 | n1;
        hdr_.crc = n2 & 1;
        hdr_.cr = (n2 >> 1) + 4;
        hdr_.ok = n3 <= 1 && headerChecksum(n0, n1, n2) == (uint8_t)((n3 & 1) << 4 | n4) && hdr_.cr >= 5 && hdr_.cr <= 8 && hdr_.len > 0;
        if (hdr_.ok) {
            Params p;
            p.sf = sf_; p.cr = hdr_.cr; p.ldro = ldro_; p.crc = hdr_.crc;
            need_ = dataSymbols(p, (size_t)hdr_.len);
        }
    }
}

bool FrameDecoder::finish(std::vector<uint8_t>& payload, bool& crcOk, int& corrected) {
    payload.clear(); crcOk = false;
    if (!done()) return false;
    const int sfApp = ldro_ ? sf_ - 2 : sf_;
    for (int at = 8; at + hdr_.cr <= n_; at += hdr_.cr) block(bins_.data() + at, hdr_.cr, sfApp, ldro_);
    const size_t len = (size_t)hdr_.len;
    if (nib_.size() < 5 + 2 * len + (hdr_.crc ? 4 : 0)) return false;
    payload.resize(len);
    for (size_t i = 0; i < len; i++) payload[i] = (uint8_t)((nib_[5 + 2 * i] | nib_[6 + 2 * i] << 4) ^ whitening(i));
    if (hdr_.crc) {
        const size_t c = 5 + 2 * len;
        const uint16_t got = (uint16_t)(nib_[c] | nib_[c + 1] << 4 | nib_[c + 2] << 8 | nib_[c + 3] << 12);
        crcOk = got == payloadCrc(payload.data(), len);
    }
    corrected = corrected_;
    return true;
}

double TxFrame::endSec() const {
    return startSec + (p.preamble + 4.25 + (double)data.size()) * symbolSeconds(p.sf, p.bwHz) * (1 + sroPpm * 1e-6);
}

namespace {
const std::vector<cf32>& sinTable() {
    static const std::vector<cf32> t = [] {
        std::vector<cf32> v(1 << 16);
        for (size_t i = 0; i < v.size(); i++) {
            const double a = 2 * M_PI * (double)i / (double)v.size();
            v[i] = cf32((float)std::cos(a), (float)std::sin(a));
        }
        return v;
    }();
    return t;
}
} // namespace

void TxFrame::render(cf32* out, size_t n, double t0, double rate) const {
    const auto& tab = sinTable();
    const int N = 1 << p.sf;
    const double chipRate = p.bwHz / (1 + sroPpm * 1e-6);
    const double P = p.preamble;
    const double dataStart = (P + 4.25) * N;
    const double total = dataStart + (double)data.size() * N;
    const int sw1 = ((p.syncWord >> 4) & 0xF) << 3, sw2 = (p.syncWord & 0xF) << 3;
    // the first sample inside the frame
    size_t k0 = 0;
    if (t0 < startSec) {
        const double d = std::ceil((startSec - t0) * rate - 1e-9);
        if (d >= (double)n) return;
        k0 = (size_t)d;
    }
    for (size_t k = k0; k < n; k++) {
        const double t = t0 + (double)k / rate - startSec;
        const double u = t * chipRate;
        if (u >= total) break;
        if (u < 0) continue;
        double c;                                // chirp phase in cycles
        auto up = [&](double v, double x) {
            double ph = ((v - N / 2.0) * x + 0.5 * x * x) / N;
            if (x > N - v) ph -= x - (N - v);
            return ph;
        };
        auto down = [&](double x) { return (N / 2.0 * x - 0.5 * x * x) / N; };
        if (u < dataStart) {
            const int s = (int)(u / N);
            const double x = u - (double)s * N;
            if (s < (int)P) c = up(0, x);
            else if (s == (int)P) c = up(sw1, x);
            else if (s == (int)P + 1) c = up(sw2, x);
            else c = down(x);
        } else {
            const double ud = u - dataStart;
            const size_t s = (size_t)(ud / N);
            c = up(data[s], ud - (double)s * N);
        }
        double cyc = c + freqHz * t + phase0;
        cyc -= std::floor(cyc);
        out[k] += amp * tab[(size_t)(cyc * 65536.0) & 0xFFFF];
    }
}

} // namespace lora
} // namespace dect2
