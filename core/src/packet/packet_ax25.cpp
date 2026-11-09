// AX.25 link layer: see packet_ax25.h. Written from the AX.25 2.2 specification.
#include "dect2/packet/packet_ax25.h"
#include <algorithm>
#include <cstdlib>
#include <map>

namespace dect2 {
namespace ax25 {

namespace {
void putAddr(std::vector<uint8_t>& o, const Address& a, bool last) {
    for (size_t i = 0; i < 6; i++) o.push_back((uint8_t)((i < a.call.size() ? a.call[i] : ' ') << 1));
    o.push_back((uint8_t)(0x60 | (a.h ? 0x80 : 0) | ((a.ssid & 15) << 1) | (last ? 1 : 0)));
}
} // namespace

void appendFcs(std::vector<uint8_t>& f) {
    const uint16_t c = fcs(f.data(), f.size());
    f.push_back((uint8_t)(c & 0xFF));
    f.push_back((uint8_t)(c >> 8));
}
std::vector<uint8_t> buildFrame(const Frame& f) {
    std::vector<uint8_t> o;
    putAddr(o, f.to, false);
    putAddr(o, f.from, f.path.empty());
    for (size_t i = 0; i < f.path.size(); i++) putAddr(o, f.path[i], i + 1 == f.path.size());
    o.push_back(f.control);
    if (f.pid >= 0) o.push_back((uint8_t)f.pid);
    o.insert(o.end(), f.info.begin(), f.info.end());
    appendFcs(o);
    return o;
}

std::vector<uint8_t> hdlcBits(const std::vector<uint8_t>& fr, int before, int after) {
    std::vector<uint8_t> b;
    auto flag = [&] { static const uint8_t f[8] = {0, 1, 1, 1, 1, 1, 1, 0}; b.insert(b.end(), f, f + 8); };
    for (int i = 0; i < before; i++) flag();
    int ones = 0;
    for (uint8_t by : fr)
        for (int k = 0; k < 8; k++) {
            const int v = (by >> k) & 1;
            b.push_back((uint8_t)v);
            if (v) { if (++ones == 5) { b.push_back(0); ones = 0; } } else ones = 0;
        }
    for (int i = 0; i < after; i++) flag();
    return b;
}

std::vector<uint8_t> nrziEncode(const std::vector<uint8_t>& bits, uint8_t level) {
    std::vector<uint8_t> o;
    o.reserve(bits.size());
    for (uint8_t b : bits) { if (!b) level ^= 1; o.push_back(level); }
    return o;
}

std::vector<uint8_t> scramble(const std::vector<uint8_t>& bits, uint32_t& st) {
    std::vector<uint8_t> o;
    o.reserve(bits.size());
    for (uint8_t b : bits) {
        const uint8_t s = (uint8_t)((b ^ (st >> 11) ^ (st >> 16)) & 1);     // taps 12 and 17 bits back
        st = ((st << 1) | s) & 0x1FFFF;
        o.push_back(s);
    }
    return o;
}

void HdlcRx::bit(int level) {
    if (bitTap_) bitTap_(level);
    size_t n = 0;
    const int ev = hdlcStep(st_, level, nrzi_, n);
    if (rawCb_) {
        int d = level & 1;
        if (nrzi_) { d = d == rprev_ ? 1 : 0; rprev_ = level & 1; }
        rpat_ = (uint8_t)((rpat_ >> 1) | (d << 7));
        raw_.push_back((uint8_t)d);
        if (rpat_ == 0x7E) {
            // the eight bits of this flag are the last ones kept: the frame's stuffed bits are what came before them
            if (rawOk_ && ev != 1 && raw_.size() >= 8 + 17 * 8 && raw_.size() <= 8 + 3900) {
                raw_.resize(raw_.size() - 8);
                rawCb_(raw_);
            }
            raw_.clear();
            rawOk_ = true;
        } else if (rpat_ == 0xFE || raw_.size() > 4000) {   // seven ones (an abort, or noise), or far too long
            raw_.clear();
            rawOk_ = false;
        }
    }
    if (ev && cb_) cb_(std::vector<uint8_t>(st_.buf, st_.buf + n), ev == 1);
}

bool frameFromStuffedBits(const std::vector<uint8_t>& bits, std::vector<uint8_t>& out) {
    out.clear();
    int ones = 0, cnt = 0;
    uint8_t acc = 0;
    for (uint8_t b : bits) {
        if (ones == 5) {                           // after five ones the sender put a zero; a one would be a flag or an abort
            if (b) return false;
            ones = 0;
            continue;
        }
        ones = b ? ones + 1 : 0;
        acc = (uint8_t)((acc >> 1) | (b ? 0x80 : 0));
        if (++cnt == 8) { out.push_back(acc); cnt = 0; acc = 0; }
    }
    if (cnt != 0 || out.size() < 17 || out.size() > 402 || !fcsOk(out.data(), out.size())) return false;
    out.resize(out.size() - 2);
    return true;
}

std::vector<std::vector<int>> fixPatterns(int level, bool scrambled) {
    std::vector<std::vector<int>> p;
    if (level <= 0) return p;
    if (scrambled) p = {{0, 1, 12, 13, 17, 18}, {0}};
    else p = {{0, 1}, {0}};
    if (level >= 2) p.push_back(scrambled ? std::vector<int>{0, 2, 12, 14, 17, 19} : std::vector<int>{0, 2});
    return p;
}

int repairFrame(const std::vector<uint8_t>& bits, const std::vector<std::vector<int>>& patterns, std::vector<uint8_t>& out,
                const std::function<bool(const std::vector<uint8_t>&)>& sane) {
    std::vector<uint8_t> b = bits;
    for (size_t k = 0; k < patterns.size(); k++) {
        const std::vector<int>& pt = patterns[k];
        if (pt.empty()) continue;
        const size_t span = (size_t)pt.back() + 1;
        for (size_t i = 0; i + span <= b.size(); i++) {
            for (int o : pt) b[i + (size_t)o] ^= 1;
            if (frameFromStuffedBits(b, out) && (!sane || sane(out))) return (int)k + 1;
            for (int o : pt) b[i + (size_t)o] ^= 1;
        }
    }
    out.clear();
    return 0;
}

bool voteRepair(const std::vector<std::vector<uint8_t>>& copies, const std::function<bool(const std::vector<uint8_t>&)>& sane, VoteResult& r, int maxContested) {
    // the largest set of copies of one length
    std::map<size_t, std::vector<const std::vector<uint8_t>*>> byLen;
    for (const auto& c : copies) if (c.size() >= 17 * 8 && c.size() <= 4000) byLen[c.size()].push_back(&c);
    const std::vector<const std::vector<uint8_t>*>* best = nullptr;
    for (const auto& kv : byLen) if (kv.second.size() >= 2 && (!best || kv.second.size() > best->size())) best = &kv.second;
    if (!best) return false;
    const size_t n = (*best)[0]->size(), m = best->size();
    std::vector<uint8_t> base(n);
    struct Pos { size_t i; int margin; };
    std::vector<Pos> contested;
    for (size_t i = 0; i < n; i++) {
        int ones = 0;
        for (const auto* c : *best) ones += (*c)[i] & 1;
        const int zeros = (int)m - ones;
        base[i] = ones > zeros ? 1 : ones < zeros ? 0 : ((*(*best)[0])[i] & 1);
        if (ones && zeros) contested.push_back({i, std::abs(ones - zeros)});
    }
    std::stable_sort(contested.begin(), contested.end(), [](const Pos& a, const Pos& b) { return a.margin < b.margin; });
    if ((int)contested.size() > maxContested) contested.resize((size_t)maxContested);
    const int k = (int)contested.size();
    r.copies = (int)m; r.contested = k;
    std::vector<uint32_t> masks((size_t)1 << k);
    for (uint32_t i = 0; i < masks.size(); i++) masks[i] = i;
    std::stable_sort(masks.begin(), masks.end(), [](uint32_t a, uint32_t b) { return __builtin_popcount(a) < __builtin_popcount(b); });
    std::vector<uint8_t> b = base, out;
    for (uint32_t mask : masks) {
        for (int j = 0; j < k; j++) if (mask >> j & 1) b[contested[(size_t)j].i] ^= 1;
        if (frameFromStuffedBits(b, out) && (!sane || sane(out))) { r.frame = out; r.flips = __builtin_popcount(mask); return true; }
        for (int j = 0; j < k; j++) if (mask >> j & 1) b[contested[(size_t)j].i] ^= 1;
    }
    return false;
}

bool plausibleFrame(const std::vector<uint8_t>& f) {
    size_t p = 0;
    int naddr = 0;
    bool last = false, srcDigit = false;
    while (!last) {
        if (p + 7 > f.size() || naddr >= 10) return false;
        bool space = false;
        for (int i = 0; i < 6; i++) {
            const uint8_t by = f[p + (size_t)i];
            if (by & 1) return false;
            const char c = (char)(by >> 1);
            if (c == ' ') { if (i == 0) return false; space = true; continue; }
            if (space || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            if (naddr == 1 && c >= '0' && c <= '9') srcDigit = true;
        }
        last = (f[p + 6] & 1) != 0;
        p += 7;
        naddr++;
    }
    if (naddr < 2 || !srcDigit || p + 2 > f.size()) return false;   // a source callsign has a digit; repaired frames are APRS-like UI frames
    if ((f[p] & 0xEF) != 0x03 || f[p + 1] != 0xF0) return false;
    for (size_t i = p + 2; i < f.size(); i++) {
        const uint8_t c = f[i];
        if (c < 0x1C && c != '\r' && c != '\n' && c != '\t') return false;
        if (c == 0x7F) return false;
    }
    return true;
}

} // namespace ax25
} // namespace dect2
