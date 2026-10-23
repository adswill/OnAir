// PSK demodulator: symbols to bits (Gray demapping, differential decoding, descramblers, sync-word hunt for the phase ambiguity).
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;
cf rotateBy(cf z, int r, int M) {
    if (!r) return z;
    const double a = 2 * kPi * r / M;
    return z * cf((float)std::cos(a), (float)std::sin(a));
}
int maxErrFor(const BitOptions& b) { return b.syncMaxErr >= 0 ? b.syncMaxErr : (int)b.sync.size() / 12; }
} // namespace

const char* diffName(Diff d) {
    switch (d) {
    case Diff::Dpsk: return "DPSK (symbol differential)";
    case Diff::NrzM: return "NRZ-M (1 = change)";
    case Diff::NrzS: return "NRZ-S / NRZI (0 = change)";
    default: return "none";
    }
}
const char* scramblerName(Scrambler s) {
    switch (s) {
    case Scrambler::G3ruh: return "G3RUH 1+x^12+x^17";
    case Scrambler::Ccsds: return "CCSDS x^8+x^7+x^5+x^3+1";
    default: return "none";
    }
}

void symbolBits(int idx, Order o, uint8_t* out) {
    switch (o) {
    case Order::Bpsk: out[0] = (uint8_t)(idx & 1); break;
    case Order::Qpsk: case Order::Oqpsk: {
        // idx 0: I+ Q+, 1: I- Q+, 2: I- Q-, 3: I+ Q-
        out[0] = (uint8_t)(idx == 1 || idx == 2);
        out[1] = (uint8_t)(idx == 2 || idx == 3);
        break;
    }
    case Order::Psk8: {
        const int g = idx ^ (idx >> 1);
        out[0] = (uint8_t)((g >> 2) & 1); out[1] = (uint8_t)((g >> 1) & 1); out[2] = (uint8_t)(g & 1);
        break;
    }
    default: break;
    }
}

int bitsSymbol(const uint8_t* b, Order o) {
    switch (o) {
    case Order::Bpsk: return b[0] & 1;
    case Order::Qpsk: case Order::Oqpsk: {
        const bool in = b[0] & 1, qn = b[1] & 1;
        return !in ? (!qn ? 0 : 3) : (!qn ? 1 : 2);
    }
    case Order::Psk8: {
        int g = ((b[0] & 1) << 2) | ((b[1] & 1) << 1) | (b[2] & 1);
        int k = 0;
        for (; g; g >>= 1) k ^= g;   // inverse Gray
        return k;
    }
    default: return 0;
    }
}

void symbolSoft(cf z, Order o, float* out) {
    switch (o) {
    case Order::Bpsk: out[0] = z.real(); break;
    case Order::Qpsk: case Order::Oqpsk: out[0] = z.real(); out[1] = z.imag(); break;
    case Order::Psk8: {
        // max-log: distance to the nearest point with the bit 1 minus with 0
        for (int b = 0; b < 3; b++) {
            float d0 = 1e9f, d1 = 1e9f;
            for (int k = 0; k < 8; k++) {
                uint8_t bits[3];
                symbolBits(k, o, bits);
                const cf p((float)std::cos(k * kPi / 4), (float)std::sin(k * kPi / 4));
                const float d = std::norm(z - p);
                if (bits[b]) d1 = std::min(d1, d); else d0 = std::min(d0, d);
            }
            out[b] = (d1 - d0) * 0.5f;
        }
        break;
    }
    default: break;
    }
}

Bits ccsdsSequence(size_t n) {
    Bits o(n);
    uint8_t s[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    for (size_t i = 0; i < n; i++) {
        o[i] = s[0];
        const uint8_t fb = s[0] ^ s[3] ^ s[5] ^ s[7];
        for (int k = 0; k < 7; k++) s[k] = s[k + 1];
        s[7] = fb;
    }
    return o;
}

Bits g3ruhDescramble(const Bits& in) { return signallab::descramble(in, signallab::scramblers()[0]); }
Bits g3ruhScramble(const Bits& in) { return signallab::scramble(in, signallab::scramblers()[0]); }

Bits diffDecode(const Bits& in, Diff d) {
    if (d != Diff::NrzM && d != Diff::NrzS) return in;
    Bits o(in.size());
    uint8_t prev = 0;
    for (size_t i = 0; i < in.size(); i++) {
        const uint8_t ch = (uint8_t)(in[i] != prev);
        o[i] = d == Diff::NrzM ? ch : (uint8_t)(ch ^ 1);
        prev = in[i];
    }
    return o;
}

Bits diffEncode(const Bits& in, Diff d) {
    if (d != Diff::NrzM && d != Diff::NrzS) return in;
    Bits o(in.size());
    uint8_t L = 0;
    for (size_t i = 0; i < in.size(); i++) {
        const bool change = d == Diff::NrzM ? in[i] != 0 : in[i] == 0;
        if (change) L ^= 1;
        o[i] = L;
    }
    return o;
}

std::vector<size_t> findSync(const Bits& b, const Bits& sync, int maxErr) {
    std::vector<size_t> at;
    const size_t L = sync.size();
    if (!L || b.size() < L) return at;
    for (size_t i = 0; i + L <= b.size();) {
        int e = 0;
        for (size_t k = 0; k < L && e <= maxErr; k++) e += b[i + k] != sync[k];
        if (e <= maxErr) { at.push_back(i + L); i += L; }
        else i++;
    }
    return at;
}

std::vector<uint8_t> packBits(const Bits& b, bool lsbFirst) {
    std::vector<uint8_t> o((b.size() + 7) / 8, 0);
    for (size_t i = 0; i < b.size(); i++)
        if (b[i]) o[i / 8] |= (uint8_t)(lsbFirst ? 1u << (i % 8) : 0x80u >> (i % 8));
    return o;
}

Bits parseBitsOrHex(const std::string& s0) {
    std::string s;
    for (char c : s0) if (c != ' ' && c != '\t' && c != '_' && c != ',') s += c;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    else if (!s.empty() && s.find_first_not_of("01") == std::string::npos) {
        Bits b;
        for (char c : s) b.push_back((uint8_t)(c == '1'));
        return b;
    }
    Bits b;
    for (char c : s) {
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return {};
        for (int k = 3; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1));
    }
    return b;
}

std::string bitsHex(const Bits& b, size_t from, size_t n) {
    std::string o;
    char t[4];
    for (size_t i = from; i + 8 <= b.size() && i < from + n; i += 8) {
        int v = 0;
        for (int k = 0; k < 8; k++) v = (v << 1) | (b[i + (size_t)k] & 1);
        std::snprintf(t, sizeof t, "%02X ", v);
        o += t;
    }
    return o;
}

std::string bitsAscii(const Bits& b, size_t from, size_t n) {
    std::string o;
    for (size_t i = from; i + 8 <= b.size() && i < from + n; i += 8) {
        int v = 0;
        for (int k = 0; k < 8; k++) v = (v << 1) | (b[i + (size_t)k] & 1);
        o += v >= 32 && v < 127 ? (char)v : '.';
    }
    return o;
}

// ---------------------------------------------------------------- streaming decoder
void BitDecoder::configure(Order o, const BitOptions& b) { o_ = o; b_ = b; reset(); }

void BitDecoder::reset() {
    rot_ = b_.rotation; conj_ = b_.conj;
    rail_ = o_ == Order::Oqpsk ? b_.railShift : 0; haveRail_ = false; lastIm_ = 0;
    resolved_ = b_.sync.empty() || !b_.autoAmbiguity;
    hunt_.clear();
    prevIdx_ = 0; prevBit_ = 0; havePrev_ = false;
    g3_ = 0;
    tail_.clear();
    sinceSync_ = 0; ccsdsPos_ = 0; ccsdsOn_ = false;
    syncs_ = 0; sinceHit_ = 0; period_ = 0; huntRounds_ = 0;
}

void BitDecoder::emit(cf z, Bits& out, std::vector<float>* soft) {
    const int M = orderPoints(o_), nb = orderBits(o_);
    if (conj_) z = std::conj(z);
    z = rotateBy(z, rot_, M);
    int idx = 0;
    decide(z, o_, &idx);
    uint8_t raw[3] = {};
    if (b_.diff == Diff::Dpsk) {
        const int d = havePrev_ ? ((idx - prevIdx_) % M + M) % M : 0;
        symbolBits(d, o_, raw);
    } else symbolBits(idx, o_, raw);
    prevIdx_ = idx; havePrev_ = true;
    float sb[3] = {};
    if (soft) symbolSoft(z, o_, sb);
    static const Bits seq = ccsdsSequence(1 << 16);
    const size_t L = b_.sync.size();
    const int maxErr = maxErrFor(b_);
    for (int k = 0; k < nb; k++) {
        uint8_t v = raw[k];
        if (b_.diff == Diff::NrzM || b_.diff == Diff::NrzS) {
            const uint8_t ch = (uint8_t)(v != prevBit_);
            prevBit_ = v;
            v = b_.diff == Diff::NrzM ? ch : (uint8_t)(ch ^ 1);
        }
        if (b_.scr == Scrambler::G3ruh) {
            const uint8_t o = (uint8_t)(v ^ ((g3_ >> 11) & 1) ^ ((g3_ >> 16) & 1));
            g3_ = ((g3_ << 1) | v) & 0x1FFFF;
            v = o;
        }
        // the sync word is looked for on these bits; the CCSDS randomiser restarts after each one
        if (L) {
            tail_.push_back(v);
            if (tail_.size() > L) tail_.erase(tail_.begin());
        }
        sinceHit_++;
        bool hit = false;
        if (L && tail_.size() == L) {
            int e = 0;
            for (size_t i = 0; i < L && e <= maxErr; i++) e += tail_[i] != b_.sync[i];
            hit = e <= maxErr && sinceHit_ >= L;
        }
        if (b_.scr == Scrambler::Ccsds && L) {
            // output lags by L bits so the sync word itself is never de-randomised
            pend_.push_back(v);
            if (hit) {
                for (uint8_t p : pend_) out.push_back((uint8_t)(p ^ (b_.invert ? 1 : 0)));
                pend_.clear();
                ccsdsOn_ = true; ccsdsPos_ = 0;
            } else if (pend_.size() > L) {
                uint8_t p = pend_.front();
                pend_.erase(pend_.begin());
                if (ccsdsOn_ && (b_.ccsdsFrameBits == 0 || ccsdsPos_ < b_.ccsdsFrameBits)) p ^= seq[ccsdsPos_ % seq.size()];
                if (ccsdsOn_) ccsdsPos_++;
                out.push_back((uint8_t)(p ^ (b_.invert ? 1 : 0)));
            }
        } else {
            out.push_back((uint8_t)(v ^ (b_.invert ? 1 : 0)));
            if (soft && b_.diff == Diff::None && b_.scr == Scrambler::None) soft->push_back(b_.invert ? -sb[k] : sb[k]);
        }
        if (hit) {
            // the spacing of the sync words: once known, a missing run of them means a slip (re-hunt sooner)
            if (syncs_ > 0 && sinceHit_ < (1u << 20)) period_ = period_ == 0 ? (double)sinceHit_ : 0.8 * period_ + 0.2 * (double)sinceHit_;
            syncs_++; sinceHit_ = 0;
        }
    }
}

void BitDecoder::flush(Bits& out) {
    for (uint8_t p : pend_) out.push_back((uint8_t)(p ^ (b_.invert ? 1 : 0)));
    pend_.clear();
}

bool BitDecoder::pre(cf in, cf& out) {
    if (!rail_) { out = in; return true; }
    // pair the Q rail of this symbol with the I rail of the next: (Im z[k], Re z[k+1])
    if (!haveRail_) { lastIm_ = in.imag(); haveRail_ = true; return false; }
    out = cf(lastIm_, in.real());
    lastIm_ = in.imag();
    return true;
}

void BitDecoder::push(const cf* sym, size_t n, Bits& out, std::vector<float>* soft) {
    const size_t huntN = std::max<size_t>(1024, (b_.sync.size() + 1) * 48 / std::max(1, orderBits(o_)));
    for (size_t i = 0; i < n; i++) {
        if (resolved_) {
            cf z;
            if (pre(sym[i], z)) emit(z, out, soft);
            // lost: no sync word for a long time (a cycle slip) -> hunt again
            const uint64_t lostAfter = period_ > 0 ? std::max<uint64_t>((uint64_t)(3.5 * period_), 4 * b_.sync.size()) : std::max<uint64_t>(2048, 64 * b_.sync.size());
            if (!b_.sync.empty() && b_.autoAmbiguity && sinceHit_ > lostAfter && syncs_ > 0) {
                resolved_ = false;
                hunt_.clear();
                huntRounds_ = 0;
            }
            continue;
        }
        hunt_.push_back(sym[i]);
        if (hunt_.size() < huntN) continue;
        int bestR = rot_, bestRail = rail_; bool bestC = conj_;
        const int bestHits = huntBest(bestR, bestC, bestRail);
        // two sync words agreeing decide; a single one only after a few windows (sync words further apart than the window)
        if (bestHits >= 2 || (bestHits == 1 && huntRounds_ >= 3)) {
            huntRounds_ = 0;
            rot_ = bestR; conj_ = bestC; resolved_ = true;
            if (bestRail != rail_) { rail_ = bestRail; haveRail_ = false; }
            sinceHit_ = 0;
            std::vector<cf> held;
            held.swap(hunt_);
            cf z;
            for (const cf& h : held) if (pre(h, z)) emit(z, out, soft);
        } else {
            // nothing yet: emit the older half as it is and keep hunting on the rest
            huntRounds_++;
            const size_t h = hunt_.size() / 2;
            cf z;
            for (size_t k = 0; k < h; k++) if (pre(hunt_[k], z)) emit(z, out, soft);
            hunt_.erase(hunt_.begin(), hunt_.begin() + (long)h);
        }
    }
}

BitResult symbolsToBits(const std::vector<cf>& sym, Order o, const BitOptions& b) {
    BitResult r;
    BitDecoder d;
    d.configure(o, b);
    d.push(sym.data(), sym.size(), r.bits, &r.soft);
    d.drain(r.bits, &r.soft);
    d.flush(r.bits);
    r.rotation = d.rotation(); r.conj = d.conj(); r.railShift = d.railShift(); r.ambiguityResolved = d.resolved() && !b.sync.empty();
    if (!b.sync.empty()) r.syncAt = findSync(r.bits, b.invert ? signallab::invert(b.sync) : b.sync, maxErrFor(b));
    r.syncHits = (int)r.syncAt.size();
    return r;
}

int BitDecoder::huntBest(int& bestR, bool& bestC, int& bestRail) const {
    const int M = orderPoints(o_);
    int bestHits = 0;
    for (int rail = 0; rail < (o_ == Order::Oqpsk ? 2 : 1); rail++)
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < M; r++) {
            BitOptions t = b_;
            t.autoAmbiguity = false; t.rotation = r; t.conj = c != 0; t.invert = false; t.railShift = rail;
            if (t.scr == Scrambler::Ccsds) t.scr = Scrambler::None;   // the sync word is not randomised
            BitDecoder d;
            d.configure(o_, t);
            Bits bb;
            d.push(hunt_.data(), hunt_.size(), bb);
            const int hits = (int)findSync(bb, b_.sync, maxErrFor(b_)).size();
            if (hits > bestHits) { bestHits = hits; bestR = r; bestC = c != 0; bestRail = rail; }
        }
    return bestHits;
}

void BitDecoder::drain(Bits& out, std::vector<float>* soft) {
    // end of a block: decide on what is held even if it is short
    if (resolved_ || hunt_.empty()) return;
    int r = rot_, rail = rail_; bool c = conj_;
    if (huntBest(r, c, rail) > 0) { rot_ = r; conj_ = c; resolved_ = true; if (rail != rail_) { rail_ = rail; haveRail_ = false; } }
    std::vector<cf> held;
    held.swap(hunt_);
    cf z;
    for (const cf& h : held) if (pre(h, z)) emit(z, out, soft);
}

} // namespace dect2::pskd
