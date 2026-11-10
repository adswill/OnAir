// CCSDS shared pieces (ccsds.h): CRC-16 of the space packets and the CCSDS 121.0-B adaptive Rice coder.
//
// A coded block starts with an option ID (3 bits for up to 8 bits a sample, 4 up to 16): all zeros is a low entropy block (one more bit:
// 0 zero blocks, 1 second extension), all ones no compression, otherwise split samples with k = ID - 1. With the predictor on, the first
// block of each reference sample interval carries the reference sample (the raw first sample) after the ID (after the low entropy bit),
// and codes the J - 1 mapped differences that follow. Split samples: the fundamental sequence of every sample's high part, then the k low
// bits of each. Second extension: pairs (a, b) as the fundamental sequence of (a + b)(a + b + 1) / 2 + b; in a reference block the
// first pair is (0, first difference). Zero blocks: a fundamental sequence count, 1 - 4 blocks for 0 - 3, 4 the rest of the segment of
// 64 blocks (or of the interval), n - 1 blocks for n >= 5. Every interval ends on a byte boundary (SZIP).
#include "dect2/ccsds/ccsds.h"
#include <algorithm>
#include "dect2/common/crc.h"

namespace dect2 {
namespace ccsds {

uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc) { return dect2::crc::crc16CcittFalse(p, n, crc); }

namespace {
constexpr int kRos = 5, kSegment = 64, kSeMax = 90;

int idLength(int bps) { return bps <= 8 ? 3 : bps <= 16 ? 4 : 5; }

// unsigned samples, xmin = 0: the mapping of CCSDS 121.0-B 4.3
inline uint32_t mapResidual(int64_t x, int64_t xhat, int64_t xmax) {
    const int64_t d = x - xhat, theta = std::min(xhat, xmax - xhat);
    if (d >= 0 && d <= theta) return (uint32_t)(2 * d);
    if (d < 0 && -d <= theta) return (uint32_t)(-2 * d - 1);
    return (uint32_t)(theta + (d < 0 ? -d : d));
}
inline int64_t unmapResidual(uint32_t m, int64_t xhat, int64_t xmax) {
    const int64_t theta = std::min(xhat, xmax - xhat);
    if ((int64_t)m <= 2 * theta) return xhat + ((m & 1) ? -(int64_t)((m + 1) / 2) : (int64_t)(m / 2));
    const int64_t a = (int64_t)m - theta;
    return xhat - theta == 0 ? xhat + a : xhat - a;       // the side away from the nearer limit
}

struct BitReader {
    const uint8_t* p; size_t n, pos = 0;   // pos in bits
    bool bad = false;
    BitReader(const uint8_t* d, size_t sz) : p(d), n(sz * 8) {}
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            if (pos >= n) { bad = true; return 0; }
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    uint32_t fs() {   // zeros up to a one
        uint32_t z = 0;
        while (true) {
            if (pos >= n) { bad = true; return 0; }
            if ((p[pos >> 3] >> (7 - (pos & 7))) & 1) { pos++; return z; }
            pos++; z++;
            if (z > (1u << 20)) { bad = true; return 0; }
        }
    }
    void align() { pos = (pos + 7) & ~(size_t)7; }
};

struct BitWriter {
    std::vector<uint8_t> out;
    int fill = 0;   // bits used in the last byte
    void put(uint32_t v, int k) {
        for (int i = k - 1; i >= 0; i--) {
            if (fill == 0) out.push_back(0);
            if ((v >> i) & 1) out.back() |= (uint8_t)(0x80 >> fill);
            fill = (fill + 1) & 7;
        }
    }
    void fs(uint32_t z) { for (uint32_t i = 0; i < z; i++) put(0, 1); put(1, 1); }
    void align() { fill = 0; }
};
} // namespace

size_t riceDecode(const uint8_t* in, size_t n, const RiceParams& p, uint16_t* out, size_t samples) {
    const int bps = std::max(1, std::min(16, p.bitsPerSample)), J = std::max(1, p.blockSize), rsiB = std::max(1, p.rsiBlocks);
    const int idLen = idLength(bps), idUncomp = (1 << idLen) - 1;
    const int64_t xmax = (1 << bps) - 1;
    BitReader br(in, n);
    std::vector<uint32_t> res((size_t)rsiB * J);
    size_t done = 0;
    while (done < samples) {
        // one reference sample interval
        const size_t want = std::min((size_t)rsiB * J, ((samples - done + J - 1) / J) * (size_t)J);
        const int blocks = (int)(want / J);
        size_t got = 0;
        for (int b = 0; b < blocks && !br.bad;) {
            const bool ref = p.preprocess && b == 0;
            uint32_t* o = res.data() + (size_t)b * J;
            const uint32_t id = br.get(idLen);
            if (id == 0) {
                const uint32_t se = br.get(1);
                if (ref) o[0] = br.get(bps);
                if (se) {
                    for (int i = ref ? 1 : 0; i < J;) {
                        const uint32_t m = br.fs();
                        if (m > (uint32_t)kSeMax || br.bad) { br.bad = true; break; }
                        int s = 0;
                        while ((s + 1) * (s + 2) / 2 <= (int)m) s++;
                        const uint32_t bb = m - (uint32_t)(s * (s + 1) / 2), aa = (uint32_t)s - bb;
                        if ((i & 1) == 0) { o[i++] = aa; if (i < J) o[i++] = bb; }
                        else o[i++] = bb;
                    }
                    b++;
                } else {
                    const uint32_t f = br.fs();
                    int zb = (int)f + 1;
                    if (zb == kRos) zb = std::min(blocks - b, kSegment - (b % kSegment));
                    else if (zb > kRos) zb--;
                    zb = std::min(zb, blocks - b);
                    for (int i = ref ? 1 : 0; i < zb * J; i++) o[i] = 0;
                    b += zb;
                }
            } else if ((int)id == idUncomp) {
                for (int i = 0; i < J; i++) o[i] = br.get(bps);
                b++;
            } else {
                const int k = (int)id - 1;
                int i0 = 0;
                if (ref) { o[0] = br.get(bps); i0 = 1; }
                for (int i = i0; i < J; i++) o[i] = br.fs() << k;
                if (k) for (int i = i0; i < J; i++) o[i] |= br.get(k);
                b++;
            }
            if (!br.bad) got = (size_t)std::min(b, blocks) * J;
        }
        if (br.bad) got = std::min(got, want);
        // the interval back to samples
        const size_t take = std::min(got, samples - done);
        if (p.preprocess) {
            int64_t x = 0;
            for (size_t i = 0; i < take; i++) {
                x = i == 0 ? std::min<int64_t>(res[0], xmax) : unmapResidual(res[i], x, xmax);
                x = std::max<int64_t>(0, std::min(x, xmax));
                out[done + i] = (uint16_t)x;
            }
        } else {
            for (size_t i = 0; i < take; i++) out[done + i] = (uint16_t)std::min<int64_t>(res[i], xmax);
        }
        done += take;
        if (br.bad || got < want) break;
        if (p.byteAlignRsi) br.align();
    }
    return done;
}

std::vector<uint8_t> riceEncode(const uint16_t* in, size_t samples, const RiceParams& p) {
    const int bps = std::max(1, std::min(16, p.bitsPerSample)), J = std::max(1, p.blockSize), rsiB = std::max(1, p.rsiBlocks);
    const int idLen = idLength(bps), idUncomp = (1 << idLen) - 1, kMax = (1 << idLen) - 3;
    const int64_t xmax = (1 << bps) - 1;
    BitWriter bw;
    const size_t total = ((samples + J - 1) / J) * (size_t)J;
    std::vector<uint32_t> v(total);
    for (size_t i = 0; i < total; i++) v[i] = std::min<uint32_t>(i < samples ? in[i] : (samples ? in[samples - 1] : 0), (uint32_t)xmax);
    for (size_t start = 0; start < total; start += (size_t)rsiB * J) {
        const size_t len = std::min((size_t)rsiB * J, total - start);
        std::vector<uint32_t> r(len);
        for (size_t i = 0; i < len; i++)
            r[i] = !p.preprocess || i == 0 ? v[start + i] : mapResidual(v[start + i], v[start + i - 1], xmax);
        const int blocks = (int)(len / J);
        for (int b = 0; b < blocks; b++) {
            const bool ref = p.preprocess && b == 0;
            const uint32_t* o = r.data() + (size_t)b * J;
            const int i0 = ref ? 1 : 0;
            bool zero = true;
            for (int i = i0; i < J; i++) zero &= o[i] == 0;
            if (zero) {
                // runs of zero blocks: up to 4 counted directly, then the rest of the segment or n - 1 for n >= 5
                int run = 1;
                while (b + run < blocks && run < kSegment - (b % kSegment)) {
                    bool z = true;
                    for (int i = 0; i < J; i++) z &= r[(size_t)(b + run) * J + i] == 0;
                    if (!z) break;
                    run++;
                }
                const int toEnd = std::min(blocks - b, kSegment - (b % kSegment));
                bw.put(0, idLen); bw.put(0, 1);
                if (ref) bw.put(o[0], bps);
                if (run < kRos) bw.fs((uint32_t)run - 1);
                else if (run == toEnd) bw.fs(kRos - 1);
                else bw.fs((uint32_t)run);
                b += run - 1;
                continue;
            }
            // costs in bits after the ID
            uint64_t best = (uint64_t)J * bps;   // no compression
            int opt = -2;                         // -2 none, -1 second extension, k >= 0 split
            for (int k = 0; k <= kMax && k < bps; k++) {
                uint64_t c = 0;
                for (int i = i0; i < J; i++) c += (o[i] >> k) + 1 + (uint64_t)k;
                if (ref) c += (uint64_t)bps;
                if (c < best) { best = c; opt = k; }
            }
            {
                uint64_t c = 1 + (ref ? (uint64_t)bps : 0);
                bool ok = true;
                for (int i = 0; i < J && ok; i += 2) {
                    const uint32_t a = (ref && i == 0) ? 0 : o[i], bb = i + 1 < J ? o[i + 1] : 0;
                    const uint64_t g = (uint64_t)(a + bb) * (a + bb + 1) / 2 + bb;
                    if (g > (uint64_t)kSeMax) ok = false; else c += g + 1;
                }
                if (ok && c < best && J % 2 == 0) { best = c; opt = -1; }
            }
            if (opt == -2) {
                bw.put((uint32_t)idUncomp, idLen);
                for (int i = 0; i < J; i++) bw.put(o[i], bps);
            } else if (opt == -1) {
                bw.put(0, idLen); bw.put(1, 1);
                if (ref) bw.put(o[0], bps);
                for (int i = 0; i < J; i += 2) {
                    const uint32_t a = (ref && i == 0) ? 0 : o[i], bb = o[i + 1];
                    bw.fs((a + bb) * (a + bb + 1) / 2 + bb);
                }
            } else {
                const int k = opt;
                bw.put((uint32_t)(k + 1), idLen);
                if (ref) bw.put(o[0], bps);
                for (int i = i0; i < J; i++) bw.fs(o[i] >> k);
                if (k) for (int i = i0; i < J; i++) bw.put(o[i] & ((1u << k) - 1), k);
            }
        }
        if (p.byteAlignRsi) bw.align();
    }
    bw.align();
    return bw.out;
}

} // namespace ccsds
} // namespace dect2
