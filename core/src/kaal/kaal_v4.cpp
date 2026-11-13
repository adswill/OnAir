// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4: four TDMA voice slots, chirp-pair sync every 40 ms, control channel, iterative receiver (kaal_v4.h, docs/KAAL-NEXT.md).
#include "dect2/kaal/kaal_v4.h"
#include "dect2/kaal/kaal_fec.h"
#include "dect2/kaal/kaal_phy.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace dect2::kaal::v4 {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTaps = 31, kHalf = 15;               // RRC 0.25, 3 samples per symbol, span 10 symbols
constexpr float kKnown = 60.f;                      // LLR of a shortened (known zero) bit

// ---------- small helpers ----------
const std::vector<uint32_t>& perm(size_t n) {       // the KAAL Fisher-Yates permutation of n positions, cached
    static std::mutex mu;
    static std::vector<std::pair<size_t, std::vector<uint32_t>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    for (auto& c : cache) if (c.first == n) return c.second;
    cache.emplace_back(n, interleavePerm(n));
    return cache.back().second;
}

std::vector<uint8_t> prbs(uint64_t seed, size_t n) {
    std::vector<uint8_t> b(n);
    uint64_t st = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((xorshift64(st) >> 23) & 1);
    return b;
}
const std::vector<uint8_t>& slotScrambler(int slot) {
    static const std::vector<uint8_t> s[kSlots] = {prbs(0x4B41414C34000001ULL, kSlotCoded16), prbs(0x4B41414C34000002ULL, kSlotCoded16),
                                                   prbs(0x4B41414C34000003ULL, kSlotCoded16), prbs(0x4B41414C34000004ULL, kSlotCoded16)};
    return s[slot & 3];
}
const std::vector<uint8_t>& ctrlScrambler() {
    static const std::vector<uint8_t> s = prbs(0x4B41414C3443544CULL, kLdpcN);
    return s;
}

void putBits(std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((x >> i) & 1)); }
uint32_t getBits(const uint8_t* b, int n) { uint32_t x = 0; for (int i = 0; i < n; i++) x = (x << 1) | (b[i] & 1); return x; }

std::vector<cf> qpskMap(const uint8_t* b, size_t nbits) {
    const float s = (float)(1.0 / std::sqrt(2.0));
    std::vector<cf> o(nbits / 2);
    for (size_t i = 0; i < o.size(); i++) o[i] = cf(b[2 * i] ? -s : s, b[2 * i + 1] ? -s : s);
    return o;
}
void qpskDemap(const cf* z, size_t n, float nv, float* llr) {
    const float k = (float)(2.0 * std::sqrt(2.0)) / std::max(nv, 1e-6f);
    for (size_t i = 0; i < n; i++) { llr[2 * i] = k * z[i].real(); llr[2 * i + 1] = k * z[i].imag(); }
}
cf qpskDecide(cf z) { const float s = (float)(1.0 / std::sqrt(2.0)); return cf(z.real() < 0 ? -s : s, z.imag() < 0 ? -s : s); }
cf qamDecide(cf z) {
    const float s = (float)(1.0 / std::sqrt(10.0));
    auto lv = [&](float v) { const float u = v / s; return (u < -2 ? -3.f : u < 0 ? -1.f : u < 2 ? 1.f : 3.f) * s; };
    return cf(lv(z.real()), lv(z.imag()));
}
// soft symbols from LLRs (positive = bit 0)
float pZero(float l) { return 1.f / (1.f + std::exp(-std::max(-30.f, std::min(30.f, l)))); }
cf qamSoft(const float* l) {
    const float s = (float)(1.0 / std::sqrt(10.0));
    float c[2];
    for (int comp = 0; comp < 2; comp++) {
        const float p0 = pZero(l[2 * comp]), p1 = pZero(l[2 * comp + 1]);   // P(b0 = 0), P(b1 = 0)
        // levels: (b0,b1) 00 -3, 01 -1, 11 +1, 10 +3
        c[comp] = s * (p0 * p1 * -3.f + p0 * (1 - p1) * -1.f + (1 - p0) * (1 - p1) * 1.f + (1 - p0) * p1 * 3.f);
    }
    return cf(c[0], c[1]);
}
cf qpskSoft(const float* l) { const float s = (float)(1.0 / std::sqrt(2.0)); return cf(s * (2 * pZero(l[0]) - 1), s * (2 * pZero(l[1]) - 1)); }

// slot word: rate matching between the 640-bit mother codeword and n coded bits (circular buffer)
inline int rmIndex(int j, int k) { return j < k ? j : kLdpcK + (j - k) % kLdpcM; }

// interpolation: windowed sinc, 12 taps, 256 phases
struct Interp {
    static constexpr int kN = 12, kP = 256;
    float t[kP + 1][kN];
    Interp() {
        for (int p = 0; p <= kP; p++) {
            const double mu = (double)p / kP;
            double s = 0;
            for (int k = 0; k < kN; k++) {
                const double x = (k - 5) - mu;          // taps at offsets -5 .. +6
                const double sc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double w = 0.42 + 0.5 * std::cos(kPi * x / 6.5) + 0.08 * std::cos(2 * kPi * x / 6.5);   // Blackman over +-6.5
                t[p][k] = (float)(sc * w);
                s += sc * w;
            }
            for (int k = 0; k < kN; k++) t[p][k] = (float)(t[p][k] / s);
        }
    }
    cf at(const cf* y, size_t n, double pos) const {   // y[pos], pos fractional
        const double fl = std::floor(pos);
        const int64_t i = (int64_t)fl;
        const int p = (int)std::lround((pos - fl) * kP);
        cf acc(0, 0);
        for (int k = 0; k < kN; k++) {
            const int64_t j = i + k - 5;
            if (j >= 0 && j < (int64_t)n) acc += y[j] * t[p][k];
        }
        return acc;
    }
};
const Interp& interp() { static const Interp i; return i; }

}  // namespace

// ---------- building blocks ----------
std::vector<cf> upChirp() {
    std::vector<cf> u(kChirp);
    for (int n = 0; n < kChirp; n++) {
        const double x = n - kChirp / 2;
        const double ph = kPi * x * x / kChirp;
        u[n] = cf((float)std::cos(ph), (float)std::sin(ph));
    }
    return u;
}

cf pilotAt(int slot, int i) {
    static const auto tab = [] {
        std::array<std::array<cf, kSlotPilots>, kSlots> t{};
        uint64_t st = 0x50494C4F54533456ULL;
        const float s = (float)(1.0 / std::sqrt(2.0));
        for (auto& row : t) for (auto& v : row) { const uint64_t r = xorshift64(st); v = cf((r & 1) ? -s : s, (r & 2) ? -s : s); }
        return t;
    }();
    return tab[slot & 3][std::min(kSlotPilots - 1, i / kPilotStep)];
}

std::vector<double> rrcTaps3() {
    const double beta = 0.25;
    std::vector<double> h(kTaps);
    double e = 0;
    for (int n = -kHalf; n <= kHalf; n++) {
        const double t = (double)n / kSps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - beta + 4 * beta / kPi;
        else if (std::fabs(std::fabs(t) - 1 / (4 * beta)) < 1e-9)
            v = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
        else v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - (4 * beta * t) * (4 * beta * t)));
        h[n + kHalf] = v;
        e += v * v;
    }
    for (double& v : h) v /= std::sqrt(e);
    return h;
}

uint16_t crc16(const uint8_t* bits, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        const bool fb = ((c >> 15) & 1) ^ (bits[i] & 1);
        c = (uint16_t)(c << 1);
        if (fb) c ^= 0x1021;
    }
    return c;
}

std::vector<uint8_t> encodeSlot(int slot, const SlotPayload& p) {
    const int k = profileK(p.profile);
    const int n = profileQpsk(p.profile) ? kSlotCodedQ : kSlotCoded16;
    std::vector<uint8_t> info;
    info.reserve(kLdpcK);
    if (profileVoice(p.profile)) {
        for (int i = 0; i < k; i++) info.push_back((uint8_t)((p.bits[i >> 3] >> (7 - (i & 7))) & 1));
    } else {
        putBits(info, p.profile == kIdle ? 0u : (uint32_t)(p.dataType & 15), 4);
        putBits(info, p.stream & 15, 4);
        putBits(info, p.seq, 8);
        const int nb = dataBytes(p.profile);
        for (int i = 0; i < nb; i++) putBits(info, p.profile == kIdle ? 0u : p.bits[i], 8);
        putBits(info, crc16(info.data(), info.size()), 16);
    }
    uint8_t full[kLdpcK] = {}, cw[kLdpcN];
    for (int i = 0; i < k; i++) full[i] = info[i];
    ldpcEncode(full, cw);
    const std::vector<uint8_t>& sc = slotScrambler(slot);
    std::vector<uint8_t> tx(n);
    for (int j = 0; j < n; j++) tx[j] = cw[rmIndex(j, k)] ^ sc[j];
    const std::vector<uint32_t>& pm = perm(n);
    std::vector<uint8_t> out(n);
    for (int i = 0; i < n; i++) out[i] = tx[pm[i]];
    return out;
}

bool decodeSlot(int slot, const float* llr, Profile prof, SlotPayload& out, int maxIter, std::vector<float>* post) {
    const int k = profileK(prof);
    const int n = profileQpsk(prof) ? kSlotCodedQ : kSlotCoded16;
    const std::vector<uint32_t>& pm = perm(n);
    const std::vector<uint8_t>& sc = slotScrambler(slot);
    float l[kSlotCoded16];
    for (int i = 0; i < n; i++) l[pm[i]] = llr[i];
    for (int j = 0; j < n; j++) if (sc[j]) l[j] = -l[j];
    float full[kLdpcN];
    for (int j = 0; j < kLdpcN; j++) full[j] = 0.f;
    for (int j = k; j < kLdpcK; j++) full[j] = kKnown;
    for (int j = 0; j < n; j++) full[rmIndex(j, k)] += l[j];
    uint8_t info[kLdpcK];
    float pf[kLdpcN];
    bool ok = ldpcDecodeLayered(full, info, maxIter, 0.9f, pf, nullptr);
    for (int j = k; j < kLdpcK && ok; j++) if (info[j]) ok = false;
    if (post) {
        post->assign(n, 0.f);
        float v[kSlotCoded16];
        for (int j = 0; j < n; j++) v[j] = sc[j] ? -pf[rmIndex(j, k)] : pf[rmIndex(j, k)];
        for (int i = 0; i < n; i++) (*post)[i] = v[pm[i]];
    }
    out = SlotPayload{};
    out.profile = prof;
    out.nbits = (uint16_t)payloadBits(prof);
    if (!ok) return false;
    if (profileVoice(prof)) {
        for (int i = 0; i < k; i++) if (info[i]) out.bits[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        out.ok = true;
        return true;
    }
    const int nb = (prof == kD26R) ? 13 : 34;
    const int dataEnd = 16 + nb * 8;
    if (crc16(info, dataEnd) != getBits(info + dataEnd, 16)) return false;
    out.dataType = (uint8_t)getBits(info, 4);
    out.stream = (uint8_t)getBits(info + 4, 4);
    out.seq = (uint8_t)getBits(info + 8, 8);
    for (int i = 0; i < nb; i++) out.bits[i] = (uint8_t)getBits(info + 16 + 8 * i, 8);
    if (prof != kD26R && out.dataType == 0) out.profile = kIdle;
    out.ok = true;
    return true;
}

std::vector<uint8_t> encodeControl(const ControlWord& w) {
    std::vector<uint8_t> info;
    info.reserve(kLdpcK);
    putBits(info, (uint32_t)w.version & 0xFF, 8);
    for (int s = 0; s < kSlots; s++) putBits(info, w.map[s] & 15, 4);
    putBits(info, w.msgType & 15, 4);
    putBits(info, w.part & 15, 4);
    putBits(info, (uint32_t)std::max(0, w.parts - 1) & 15, 4);
    putBits(info, 0, 4);
    for (int i = 0; i < kCtrlMsgBytes; i++) putBits(info, w.msg[i], 8);
    putBits(info, crc16(info.data(), info.size()), 16);
    uint8_t cw[kLdpcN];
    ldpcEncode(info.data(), cw);
    const std::vector<uint8_t>& sc = ctrlScrambler();
    const std::vector<uint32_t>& pm = perm(kLdpcN);
    std::vector<uint8_t> out(kLdpcN);
    for (int i = 0; i < kLdpcN; i++) out[i] = cw[pm[i]] ^ sc[pm[i]];
    return out;
}

bool decodeControl(const float* llr, ControlWord& w, int maxIter) {
    const std::vector<uint8_t>& sc = ctrlScrambler();
    const std::vector<uint32_t>& pm = perm(kLdpcN);
    float l[kLdpcN];
    for (int i = 0; i < kLdpcN; i++) l[pm[i]] = sc[pm[i]] ? -llr[i] : llr[i];
    uint8_t info[kLdpcK];
    if (!ldpcDecodeLayered(l, info, maxIter, 0.9f, nullptr, nullptr)) return false;
    const int end = 40 + kCtrlMsgBytes * 8;
    if (crc16(info, end) != getBits(info + end, 16)) return false;
    w = ControlWord{};
    w.version = (int)getBits(info, 8);
    for (int s = 0; s < kSlots; s++) { const uint32_t v = getBits(info + 8 + 4 * s, 4); w.map[s] = v < kProfiles ? (Profile)v : kIdle; }
    w.msgType = (uint8_t)getBits(info + 24, 4);
    w.part = (uint8_t)getBits(info + 28, 4);
    w.parts = (uint8_t)(getBits(info + 32, 4) + 1);
    for (int i = 0; i < kCtrlMsgBytes; i++) w.msg[i] = (uint8_t)getBits(info + 40 + 8 * i, 8);
    return true;
}

namespace {
// beacon fields in a control message: the KAAL beacon header (170 bits incl. its own CRC-16) packed into 22 bytes
bool beaconToMsg(const BeaconFields& f, uint8_t* msg) {
    BeaconFields g = f;
    g.frameType = 0;
    const std::vector<uint8_t> b = encodeBeaconBits(g, "");
    if (b.size() != (size_t)kBeaconHeaderBits) return false;
    std::memset(msg, 0, kCtrlMsgBytes);
    for (size_t i = 0; i < b.size(); i++) if (b[i]) msg[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    return true;
}
bool msgToBeacon(const uint8_t* msg, BeaconFields& f) {
    std::vector<uint8_t> b(kBeaconHeaderBits);
    for (int i = 0; i < kBeaconHeaderBits; i++) b[i] = (msg[i >> 3] >> (7 - (i & 7))) & 1;
    std::string t;
    return decodeBeaconBits(b, f, t);
}
}  // namespace

// ---------- transmitter ----------
struct FrameTx::Impl {
    double outRate = kFs;
    bool resample = false;
    RationalResampler rs;
    std::vector<double> h = rrcTaps3();
    std::vector<cf> carry;                       // overlap-add tail
    uint64_t sf = 0;
    Profile prof[kSlots] = {kIdle, kIdle, kIdle, kIdle};
    std::deque<std::array<uint8_t, 40>> voice[kSlots];
    bool robust = false, ctrlText = true;
    // text for the data slots: whole messages framed as [len hi][len lo][bytes]
    std::deque<std::vector<uint8_t>> msgs;
    size_t msgPos = 0;                           // bytes of msgs.front() already sent in slots
    uint8_t seq = 0;
    // control channel
    bool haveBeacon = false;
    uint8_t beaconMsg[kCtrlMsgBytes] = {};
    std::string call[kSlots];
    std::vector<uint8_t> ctrlBits;
    uint64_t ctrlWords = 0;
    std::vector<uint8_t> ctrlMsgText;            // the message being sent in control words
    int ctrlPart = 0, ctrlParts = 0;
    int callRot = 0;
    bool allBusyLast = false;
    uint64_t sent[kProfiles] = {};
    uint64_t textBytes = 0;

    void resetAll() {
        carry.assign(kTaps, cf(0, 0));
        sf = 0;
        for (auto& q : voice) q.clear();
        msgs.clear(); msgPos = 0; seq = 0;
        ctrlBits.clear(); ctrlWords = 0; ctrlMsgText.clear(); ctrlPart = ctrlParts = 0; callRot = 0; allBusyLast = false;
        std::memset(sent, 0, sizeof(sent)); textBytes = 0;
        if (resample) rs.reset();
    }

    ControlWord buildControl() {
        ControlWord w;
        for (int s = 0; s < kSlots; s++) w.map[s] = profileVoice(prof[s]) && !voice[s].empty() ? prof[s] : (robust ? kD26R : kD68);
        const uint64_t c = ctrlWords++;
        // text in the control channel only while every slot is busy with voice
        if (ctrlParts == 0 && ctrlText && allBusyLast && !msgs.empty() && msgPos == 0) {
            const std::vector<uint8_t>& m = msgs.front();
            ctrlMsgText.assign(m.begin() + 2, m.end());
            if (ctrlMsgText.size() > 16 * (size_t)kCtrlMsgBytes) ctrlMsgText.resize(16 * kCtrlMsgBytes);
            ctrlParts = (int)((ctrlMsgText.size() + kCtrlMsgBytes - 1) / kCtrlMsgBytes);
            if (ctrlParts == 0) ctrlParts = 1;
            ctrlPart = 0;
            msgs.pop_front();
        }
        const bool beaconTurn = haveBeacon && (c % 4 == 0 || (ctrlParts == 0 && c % 4 != 2));
        int callSlot = -1;
        for (int i = 0; i < kSlots; i++) { const int s = (callRot + i) % kSlots; if (!call[s].empty()) { callSlot = s; break; } }
        if (ctrlParts > 0 && !(haveBeacon && c % 4 == 0)) {
            w.msgType = kMsgText;
            w.part = (uint8_t)ctrlPart;
            w.parts = (uint8_t)ctrlParts;
            const size_t off = (size_t)ctrlPart * kCtrlMsgBytes;
            const size_t nb = std::min((size_t)kCtrlMsgBytes, ctrlMsgText.size() - off);
            std::memcpy(w.msg, ctrlMsgText.data() + off, nb);
            textBytes += nb;
            if (++ctrlPart >= ctrlParts) { ctrlParts = 0; ctrlMsgText.clear(); }
        } else if (callSlot >= 0 && (c % 4 == 2 || !haveBeacon)) {
            w.msgType = kMsgSlotCall;
            w.msg[0] = (uint8_t)callSlot;
            const std::string& cs = call[callSlot];
            std::memcpy(w.msg + 1, cs.data(), std::min(cs.size(), (size_t)kCtrlMsgBytes - 1));
            callRot = (callSlot + 1) % kSlots;
        } else if (beaconTurn || haveBeacon) {
            w.msgType = kMsgBeacon;
            std::memcpy(w.msg, beaconMsg, kCtrlMsgBytes);
        } else {
            w.msgType = kMsgEmpty;
        }
        return w;
    }

    SlotPayload nextData() {
        SlotPayload p;
        p.profile = robust ? kD26R : kD68;
        const int nb = dataBytes(p.profile);
        p.nbits = (uint16_t)(nb * 8);
        if (msgs.empty()) { if (!robust) p.profile = kIdle; return p; }
        std::vector<uint8_t>& m = msgs.front();
        p.dataType = msgPos == 0 ? 1 : 4;
        p.stream = 0;
        p.seq = seq++;
        const size_t take = std::min((size_t)nb, m.size() - msgPos);
        std::memcpy(p.bits, m.data() + msgPos, take);
        msgPos += take;
        textBytes += take;
        if (msgPos >= m.size()) { msgs.pop_front(); msgPos = 0; }
        return p;
    }

    void superframe(std::vector<cf>& out) {
        const int cyc = (int)(sf % kCtrlCycle);
        if (cyc == 0) ctrlBits = encodeControl(buildControl());
        std::vector<cf> sym(kSfSyms);
        const std::vector<cf> u = upChirp();
        for (int n = 0; n < kChirp; n++) { sym[n] = u[n]; sym[kChirp + n] = std::conj(u[n]) * (cyc == 0 ? -1.f : 1.f); }
        const std::vector<cf> cs = qpskMap(ctrlBits.data() + cyc * 2 * kCtrlSyms, 2 * kCtrlSyms);
        for (int i = 0; i < kCtrlSyms; i++) sym[kCtrlStart + i] = cs[i];
        int busy = 0;
        for (int s = 0; s < kSlots; s++) {
            SlotPayload p;
            if (profileVoice(prof[s]) && !voice[s].empty()) {
                p.profile = prof[s];
                p.nbits = (uint16_t)payloadBits(prof[s]);
                std::memcpy(p.bits, voice[s].front().data(), 40);
                voice[s].pop_front();
                busy++;
            } else {
                p = nextData();
            }
            sent[p.profile]++;
            const std::vector<uint8_t> bits = encodeSlot(s, p);
            const std::vector<cf> d = profileQpsk(p.profile) ? qpskMap(bits.data(), bits.size()) : qam16Map(bits.data(), bits.size());
            int di = 0;
            for (int i = 0; i < kSlotSyms; i++) sym[slotStart(s) + i] = isPilot(i) ? pilotAt(s, i) : d[di++];
        }
        allBusyLast = busy == kSlots;
        // pulse shaping (overlap-add), scaled to kTxRms
        const float g = kTxRms * (float)std::sqrt((double)kSps);
        std::vector<cf> acc(kSfSamples + kTaps, cf(0, 0));
        for (int i = 0; i < kTaps; i++) acc[i] = carry[i];
        for (int m = 0; m < kSfSyms; m++) {
            const cf v = sym[m] * g;
            cf* a = acc.data() + m * kSps;
            for (int j = 0; j < kTaps; j++) a[j] += v * (float)h[j];
        }
        for (int i = 0; i < kTaps; i++) carry[i] = acc[kSfSamples + i];
        if (resample) rs.process(acc.data(), kSfSamples, out);
        else out.insert(out.end(), acc.begin(), acc.begin() + kSfSamples);
        sf++;
    }
};

FrameTx::FrameTx() : p_(new Impl) { p_->resetAll(); }
FrameTx::~FrameTx() = default;
void FrameTx::configure(double outRate) {
    p_->outRate = outRate;
    p_->resample = std::fabs(outRate - kFs) > 1e-6;
    if (p_->resample) p_->rs.configure(kFs, outRate);
    p_->resetAll();
}
void FrameTx::reset() { p_->resetAll(); }
void FrameTx::setSlotProfile(int slot, Profile p) { if (slot >= 0 && slot < kSlots) p_->prof[slot] = p; }
void FrameTx::pushVoice(int slot, const uint8_t* bits) {
    if (slot < 0 || slot >= kSlots) return;
    std::array<uint8_t, 40> a{};
    std::memcpy(a.data(), bits, (payloadBits(p_->prof[slot]) + 7) / 8);
    p_->voice[slot].push_back(a);
}
size_t FrameTx::voiceQueued(int slot) const { return (slot >= 0 && slot < kSlots) ? p_->voice[slot].size() : 0; }
void FrameTx::setDataRobust(bool on) { p_->robust = on; }
void FrameTx::queueText(const std::string& text) {
    if (text.empty()) return;
    const size_t n = std::min(text.size(), (size_t)65535);
    std::vector<uint8_t> m;
    m.push_back((uint8_t)(n >> 8)); m.push_back((uint8_t)(n & 255));
    m.insert(m.end(), text.begin(), text.begin() + (long)n);
    p_->msgs.push_back(std::move(m));
}
size_t FrameTx::textQueuedBytes() const {
    size_t n = 0;
    for (auto& m : p_->msgs) n += m.size();
    return n - p_->msgPos + p_->ctrlMsgText.size() - std::min(p_->ctrlMsgText.size(), (size_t)p_->ctrlPart * kCtrlMsgBytes);
}
void FrameTx::setBeacon(const BeaconFields& f) { p_->haveBeacon = beaconToMsg(f, p_->beaconMsg); }
void FrameTx::setSlotCallsign(int slot, const std::string& c) { if (slot >= 0 && slot < kSlots) p_->call[slot] = c.substr(0, kCtrlMsgBytes - 1); }
void FrameTx::setControlText(bool on) { p_->ctrlText = on; }
void FrameTx::superframe(std::vector<cf>& out) { p_->superframe(out); }
uint64_t FrameTx::superframes() const { return p_->sf; }
uint64_t FrameTx::sentSlots(Profile p) const { return (p < kProfiles) ? p_->sent[p] : 0; }
uint64_t FrameTx::textBytesSent() const { return p_->textBytes; }

// ---------- receiver ----------
namespace {
constexpr int kAcqSf = 3;                    // superframes gathered for acquisition (120 ms)
constexpr int kAcqShift = 13;                // +- samples of chirp peak shift searched (+-4.3 kHz)
constexpr float kAcqThr = 0.30f;             // mean normalised chirp correlation needed to lock
constexpr float kLossThr = 0.12f;            // ... below this for kLossSf superframes = lost
constexpr int kLossSf = 8;
constexpr int kMargin = 64;                  // samples around a superframe segment
}  // namespace

struct FrameRx::Impl {
    double inRate = kFs;
    bool resampling = false;
    RationalResampler rs;
    std::vector<cf> rsOut;
    std::vector<float> h;
    std::vector<cf> u;                       // up-chirp
    std::vector<cf> x;                       // raw 48 kHz input
    int64_t xBase = 0;                       // absolute index of x[0]
    uint64_t samplesIn = 0;
    bool locked = false;
    double T = 0;                            // absolute sample index of symbol 0 of the next superframe
    double freq = 0;                         // carrier offset (Hz) taken out
    double clk = kSfSamples;                 // samples per superframe (sample-clock offset)
    uint64_t sfSinceLock = 0;
    int lossCount = 0;
    float nv = 0.1f;                         // smoothed noise variance (unit symbol power)
    bool iterative = true, ddTrack = true;
    // control
    int ctrlPos = -1;
    float ctrlLlr[kLdpcN];
    uint32_t ctrlHave = 0;                   // bit i: block i of the cycle present
    ControlWord lastCtrl;
    bool ctrlValid = false;
    uint64_t ctrlAge = 0;
    std::vector<uint8_t> ctrlText;           // control-channel text being collected
    int ctrlTextNext = 0;
    // slots
    Profile lastGood[kSlots] = {kIdle, kIdle, kIdle, kIdle};
    // data text reassembly (stream 0)
    std::vector<uint8_t> dmsg;
    size_t dlen = 0;
    bool dactive = false;
    uint8_t dseq = 0;
    // outputs
    std::deque<RxSuperframe> outSf;
    std::deque<std::string> texts;
    std::deque<BeaconFields> beacons;
    std::deque<std::pair<int, std::string>> calls;
    RxStatsV4 st;
    std::function<void(const std::string&)> log;

    Impl() {
        const std::vector<double> hd = rrcTaps3();
        h.assign(hd.begin(), hd.end());
        u = upChirp();
    }

    void resetAll() {
        x.clear(); xBase = 0; samplesIn = 0;
        locked = false; T = 0; freq = 0; clk = kSfSamples; sfSinceLock = 0; lossCount = 0; nv = 0.1f;
        ctrlPos = -1; ctrlHave = 0; ctrlValid = false; ctrlAge = 0; ctrlText.clear(); ctrlTextNext = 0;
        for (auto& p : lastGood) p = kIdle;
        dmsg.clear(); dlen = 0; dactive = false;
        hInit = false;
        outSf.clear(); texts.clear(); beacons.clear(); calls.clear();
        st = RxStatsV4{};
        if (resampling) rs.reset();
    }

    // matched filter of x[a .. b) (relative indices, kHalf margin used on both sides when present), derotated by freq
    void mf(int64_t a, int64_t b, std::vector<cf>& y, bool derotate) const {
        const int64_t n = b - a;
        y.assign((size_t)std::max<int64_t>(0, n), cf(0, 0));
        const int64_t lo = std::max<int64_t>(0, a - kHalf), hi = std::min<int64_t>((int64_t)x.size(), b + kHalf);
        std::vector<cf> d((size_t)std::max<int64_t>(0, hi - lo));
        if (derotate) {
            const double w = -2 * kPi * freq / kFs;
            const double ph0 = std::fmod(w * (double)(xBase + lo), 2 * kPi);
            cf rot((float)std::cos(ph0), (float)std::sin(ph0));
            const cf step((float)std::cos(w), (float)std::sin(w));
            for (int64_t i = lo; i < hi; i++) {
                d[(size_t)(i - lo)] = x[(size_t)i] * rot;
                rot *= step;
                if (((i - lo) & 255) == 255) rot /= std::abs(rot);
            }
        } else {
            for (int64_t i = lo; i < hi; i++) d[(size_t)(i - lo)] = x[(size_t)i];
        }
        for (int64_t i = 0; i < n; i++) {
            const int64_t c = a + i;   // output at x index c: sum h[j] x[c + j - kHalf]
            cf acc(0, 0);
            for (int j = 0; j < kTaps; j++) {
                const int64_t k = c + j - kHalf;
                if (k >= lo && k < hi) acc += d[(size_t)(k - lo)] * h[j];
            }
            y[(size_t)i] = acc;
        }
    }

    // ---- acquisition ----
    bool acquire() {
        const int64_t need = (int64_t)kAcqSf * kSfSamples + 2 * kChirp * kSps + 2 * kAcqShift + 2 * kMargin;
        if ((int64_t)x.size() < need) return false;
        std::vector<cf> y;
        mf(0, need, y, false);
        const int64_t nc = need - kChirp * kSps;
        std::vector<float> ru((size_t)nc), rd((size_t)nc);
        for (int64_t t = 0; t < nc; t++) {
            cf cu(0, 0), cd(0, 0);
            float e = 0;
            for (int n = 0; n < kChirp; n++) {
                const cf v = y[(size_t)(t + kSps * n)];
                cu += v * std::conj(u[n]);
                cd += v * u[n];          // conj(down) = up
                e += std::norm(v);
            }
            const float den = kChirp * e + 1e-20f;
            ru[(size_t)t] = std::norm(cu) / den;
            rd[(size_t)t] = std::norm(cd) / den;
        }
        // candidate t0 (superframe start) in [kAcqShift, kAcqShift + 1920); up peak at t0 - j, down peak at t0 + 48 + j
        float best = -1;
        int bt = 0, bj = 0;
        for (int t0 = kAcqShift; t0 < kAcqShift + kSfSamples; t0++) {
            for (int j = -kAcqShift; j <= kAcqShift; j++) {
                float m = 0;
                for (int r = 0; r < kAcqSf; r++) {
                    const int64_t pu = t0 + (int64_t)r * kSfSamples - j, pd = t0 + (int64_t)r * kSfSamples + kChirp * kSps + j;
                    if (pd >= nc) { m = -1; break; }
                    m += ru[(size_t)pu] + rd[(size_t)pd];
                }
                if (m > best) { best = m; bt = t0; bj = j; }
            }
        }
        const float score = best / (2.f * kAcqSf);
        if (score < kAcqThr) {
            // keep the last superframe or so, drop the rest
            const int64_t drop = (int64_t)x.size() - (int64_t)kSfSamples - 2 * kMargin;
            if (drop > 0) { x.erase(x.begin(), x.begin() + drop); xBase += drop; }
            return false;
        }
        // fractional peaks from the sums over the superframes
        auto sumAt = [&](const std::vector<float>& r, int64_t p) {
            float s = 0;
            for (int q = 0; q < kAcqSf; q++) { const int64_t i = p + (int64_t)q * kSfSamples; if (i >= 0 && i < nc) s += r[(size_t)i]; }
            return s;
        };
        auto parab = [](float a, float b, float c) { const float d = a - 2 * b + c; return std::fabs(d) < 1e-12f ? 0.f : 0.5f * (a - c) / d; };
        const int64_t pu = bt - bj, pd = bt + kChirp * kSps + bj;
        const double fu = pu + parab(std::sqrt(sumAt(ru, pu - 1)), std::sqrt(sumAt(ru, pu)), std::sqrt(sumAt(ru, pu + 1)));
        const double fd = pd + parab(std::sqrt(sumAt(rd, pd - 1)), std::sqrt(sumAt(rd, pd)), std::sqrt(sumAt(rd, pd + 1)));
        const double m = (fd - fu - kChirp * kSps) / (2.0 * kSps);          // symbols of chirp shift
        freq = m * kRs / kChirp;
        double t0 = 0.5 * (fu + fd - kChirp * kSps);
        while (t0 < kMargin) t0 += kSfSamples;
        T = (double)xBase + t0;
        clk = kSfSamples;
        locked = true;
        sfSinceLock = 0;
        lossCount = 0;
        ctrlPos = -1; ctrlHave = 0;
        st.locks++;
        if (log) { char b[160]; snprintf(b, sizeof b, "KAAL v4: lock, chirp score %.2f, carrier %+.0f Hz", score, freq); log(b); }
        return true;
    }

    // ---- one superframe ----
    bool processSf() {
        const double rel = T - (double)xBase;
        const int64_t a = (int64_t)std::floor(rel) - kMargin;
        const int64_t b = (int64_t)std::floor(rel) + kSfSamples + kChirp * kSps + kMargin;
        if (b + kHalf > (int64_t)x.size()) return false;
        if (a < 0) { T += clk; return true; }   // (only right after a lock) not enough history
        std::vector<cf> y;
        mf(a, b, y, true);
        const double off = rel - (double)a;          // position of symbol 0 in y
        const Interp& ip = interp();
        constexpr int kZ = kSfSyms + kChirp;         // the next superframe's up-chirp too
        cf z[kZ];
        // timing: chirp correlations at -d, 0, +d samples
        auto chirpCorr = [&](double dt, cf& cu, cf& cd) {
            cu = cf(0, 0); cd = cf(0, 0);
            for (int n = 0; n < kChirp; n++) {
                cu += ip.at(y.data(), y.size(), off + dt + kSps * n) * std::conj(u[n]);
                cd += ip.at(y.data(), y.size(), off + dt + kSps * (kChirp + n)) * u[n];
            }
        };
        cf cu0, cd0, cuE, cdE, cuL, cdL;
        const double dd = 1.0;
        chirpCorr(0, cu0, cd0); chirpCorr(-dd, cuE, cdE); chirpCorr(dd, cuL, cdL);
        auto parab = [](double e, double c, double l) { const double d = e - 2 * c + l; return std::fabs(d) < 1e-12 ? 0.0 : 0.5 * (e - l) / d; };
        double du = dd * parab(std::abs(cuE), std::abs(cu0), std::abs(cuL));
        double dn = dd * parab(std::abs(cdE), std::abs(cd0), std::abs(cdL));
        du = std::max(-1.5, std::min(1.5, du)); dn = std::max(-1.5, std::min(1.5, dn));
        const double terr = 0.5 * (du + dn);
        for (int k = 0; k < kZ; k++) z[k] = ip.at(y.data(), y.size(), off + terr + kSps * k);
        // sync quality and the control marker
        float e = 0;
        cf su(0, 0), sd(0, 0);
        for (int n = 0; n < kChirp; n++) { su += z[n] * std::conj(u[n]); sd += z[kChirp + n] * u[n]; e += std::norm(z[n]) + std::norm(z[kChirp + n]); }
        const float rho = (std::norm(su) + std::norm(sd)) / (kChirp * e + 1e-20f);
        // known symbols: chirps, the pilot grid (64 + 18 q, q = 0..32: the last one is the next up-chirp's first symbol), next up-chirp
        // frequency: lag-18 products on the pilot grid
        cf ref[kZ];
        bool known[kZ];
        for (int k = 0; k < kZ; k++) known[k] = false;
        for (int n = 0; n < kChirp; n++) { ref[n] = u[n]; known[n] = true; ref[kSfSyms + n] = u[n]; known[kSfSyms + n] = true; }
        // marker decision: phase between the up and down halves (residual frequency is small once tracking; use the grid estimate below)
        for (int s = 0; s < kSlots; s++)
            for (int i = 0; i < kSlotSyms; i += kPilotStep) if (isPilot(i)) { ref[slotStart(s) + i] = pilotAt(s, i); known[slotStart(s) + i] = true; }
        cf lag(0, 0);
        for (int q = 0; q < 32; q++) {
            const int k0 = kSlot0 + kPilotStep * q, k1 = k0 + kPilotStep;
            lag += z[k1] * std::conj(ref[k1]) * std::conj(z[k0] * std::conj(ref[k0]));
        }
        const double b0 = std::arg(lag) / kPilotStep;   // rad per symbol
        // marker: down-chirp sign
        const cf rot16 = std::polar(1.f, (float)(-b0 * kChirp));
        const bool marker = (sd * std::conj(su) * rot16).real() < 0;
        const float dsign = marker ? -1.f : 1.f;
        for (int n = 0; n < kChirp; n++) { ref[kChirp + n] = std::conj(u[n]) * dsign; known[kChirp + n] = true; }
        // phase fit a + b k: derotate by b0, then weighted LS on group phases (unwrapped)
        struct G { double k; cf v; double w; };
        std::vector<G> grp;
        auto groupOf = [&](int k0, int n) {
            cf s(0, 0);
            for (int k = k0; k < k0 + n; k++) s += z[k] * std::conj(ref[k]) * std::polar(1.f, (float)(-b0 * k));
            grp.push_back({k0 + (n - 1) / 2.0, s, (double)n});
        };
        groupOf(0, kChirp); groupOf(kChirp, kChirp);
        for (int q = 0; q < 32; q++) groupOf(kSlot0 + kPilotStep * q, 1);
        groupOf(kSfSyms, kChirp);
        std::vector<double> ph(grp.size());
        double prev = std::arg(grp[0].v);
        ph[0] = prev;
        for (size_t i = 1; i < grp.size(); i++) {
            double p = std::arg(grp[i].v);
            while (p - prev > kPi) p -= 2 * kPi;
            while (p - prev < -kPi) p += 2 * kPi;
            ph[i] = p; prev = p;
        }
        double sw = 0, sk = 0, sp = 0, skk = 0, skp = 0;
        for (size_t i = 0; i < grp.size(); i++) {
            const double w = grp[i].w * std::abs(grp[i].v) / std::max(1e-9, grp[i].w);
            sw += w; sk += w * grp[i].k; sp += w * ph[i]; skk += w * grp[i].k * grp[i].k; skp += w * grp[i].k * ph[i];
        }
        const double det = sw * skk - sk * sk;
        const double b1 = det > 1e-12 ? (sw * skp - sk * sp) / det : 0;
        const double a0 = (sp - b1 * sk) / std::max(1e-12, sw);
        const double bb = b0 + b1;
        // gain and noise from the known symbols
        cf zn[kZ];
        double gnum = 0, gden = 0;
        for (int k = 0; k < kZ; k++) {
            zn[k] = z[k] * std::polar(1.f, (float)(-(a0 + bb * k)));
            if (known[k]) { gnum += (zn[k] * std::conj(ref[k])).real(); gden += std::norm(ref[k]); }
        }
        const float g = (float)std::max(1e-9, gnum / std::max(1e-12, gden));
        double nsum = 0;
        int ncount = 0;
        for (int k = 0; k < kZ; k++) {
            zn[k] /= g;
            if (known[k]) { nsum += std::norm(zn[k] - ref[k]); ncount++; }
        }
        if (equalise(zn, ref, nsum / std::max(1, ncount))) {
            nsum = 0; ncount = 0;
            for (int k = 0; k < kZ; k++) if (known[k]) { nsum += std::norm(zn[k] - ref[k]); ncount++; }
        }
        const float nvNow = (float)std::max(1e-5, nsum / std::max(1, ncount));
        nv = sfSinceLock < 2 ? nvNow : 0.75f * nv + 0.25f * nvNow;
        // loops: carrier, timing, clock
        const double fErr = bb * kRs / (2 * kPi);
        const bool early = sfSinceLock < 3;
        freq += (early ? 1.0 : 0.35) * fErr;
        const double tGain = early ? 1.0 : 0.5;
        clk += (early ? 0.0 : 0.02) * terr;
        clk = std::max(kSfSamples * 0.999, std::min(kSfSamples * 1.001, clk));
        T += clk + tGain * terr;
        // lock loss
        if (rho < kLossThr) { if (++lossCount >= kLossSf) { locked = false; if (log) log("KAAL v4: lost"); } }
        else lossCount = 0;
        // decision-directed phase smoothing (complex average of z * conj(decision) over +-W symbols), known symbols weigh 4x
        if (ddTrack) ddSmooth(zn, ref, known);
        RxSuperframe r;
        r.index = sfSinceLock;
        r.esn0Db = (float)(-10 * std::log10(nv));
        r.cfoHz = freq;
        // control channel
        if (marker) { ctrlPos = 0; ctrlHave = 0; }
        else if (ctrlPos >= 0) ctrlPos = (ctrlPos + 1) % kCtrlCycle;
        r.ctrlPos = ctrlPos;
        if (ctrlPos >= 0) {
            qpskDemap(zn + kCtrlStart, kCtrlSyms, nv, ctrlLlr + ctrlPos * 2 * kCtrlSyms);
            ctrlHave |= 1u << ctrlPos;
            if (ctrlPos == kCtrlCycle - 1) {
                if (ctrlHave == (1u << kCtrlCycle) - 1) {
                    ControlWord w;
                    if (decodeControl(ctrlLlr, w)) { st.ctrlOk++; onControl(w); } else st.ctrlBad++;
                }
                ctrlHave = 0;
            }
        }
        if (ctrlValid) ctrlAge++;
        // slots
        for (int s = 0; s < kSlots; s++) decodeSlotSyms(s, zn + slotStart(s), r.slot[s]);
        st.superframes++;
        st.esn0Db = r.esn0Db;
        st.cfoHz = freq;
        st.clockPpm = (clk / kSfSamples - 1.0) * 1e6;
        outSf.push_back(r);
        if (outSf.size() > 64) outSf.pop_front();
        sfSinceLock++;
        return true;
    }

    // Multipath: symbol-spaced channel h[-2..2] by least squares on the two chirps (contiguous known symbols), averaged over superframes;
    // when the off-centre taps hold more than 1/50 of the energy, a 13-tap MMSE linear equaliser built from it is applied to the superframe.
    static constexpr int kHL = 2, kEqM = 6;
    cf hAvg[2 * kHL + 1] = {};
    bool hInit = false;
    bool eqOn = true;
    bool equalise(cf* zn, const cf* ref, double nvNow) {
        if (!eqOn) return false;
        constexpr int L = 2 * kHL + 1;
        cf A[L][L] = {}, bvec[L] = {};
        auto addRow = [&](int k) {
            cf s[L];
            for (int l = 0; l < L; l++) s[l] = ref[k - (l - kHL)];
            for (int i = 0; i < L; i++) {
                bvec[i] += std::conj(s[i]) * zn[k];
                for (int j = 0; j < L; j++) A[i][j] += std::conj(s[i]) * s[j];
            }
        };
        for (int k = kHL; k < 2 * kChirp - kHL; k++) addRow(k);
        for (int k = kSfSyms + kHL; k < kSfSyms + kChirp - kHL; k++) addRow(k);
        cf h[L];
        if (!solve<L>(A, bvec, h)) return false;
        if (!hInit) { for (int l = 0; l < L; l++) hAvg[l] = h[l]; hInit = true; }
        else for (int l = 0; l < L; l++) hAvg[l] = 0.85f * hAvg[l] + 0.15f * h[l];
        float eo = 0;
        for (int l = 0; l < L; l++) if (l != kHL) eo += std::norm(hAvg[l]);
        if (eo < 0.02f * std::norm(hAvg[kHL])) return false;
        // MMSE: w = (H H^H + nv I)^-1 H e, H the (2M+1) x (2M+1+2kHL) convolution matrix of hAvg
        constexpr int N = 2 * kEqM + 1, S = N + 2 * kHL;
        cf H[N][S] = {};
        for (int r = 0; r < N; r++) for (int l = 0; l < L; l++) H[r][r + l] = hAvg[2 * kHL - l];   // z[k+r-M] = sum_l h[l] s[k+r-M-l]
        cf R[N][N], c[N], w[N];
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                cf acc(0, 0);
                for (int q = 0; q < S; q++) acc += H[i][q] * std::conj(H[j][q]);
                R[i][j] = acc + (i == j ? cf((float)std::max(1e-4, nvNow), 0) : cf(0, 0));
            }
            c[i] = H[i][kEqM + kHL];
        }
        if (!solve<N>(R, c, w)) return false;
        cf out[kSfSyms + kChirp];
        constexpr int nz = kSfSyms + kChirp;
        float g = 0;
        for (int i = 0; i < N; i++) g += (std::conj(w[i]) * c[i]).real();
        for (int k = 0; k < nz; k++) {
            cf acc(0, 0);
            for (int i = 0; i < N; i++) { const int j = k + i - kEqM; if (j >= 0 && j < nz) acc += std::conj(w[i]) * zn[j]; }
            out[k] = g > 1e-6f ? acc / g : acc;
        }
        std::copy(out, out + nz, zn);
        st.equalised++;
        return true;
    }
    template <int N> static bool solve(cf (&A)[N][N], cf (&b)[N], cf (&x)[N]) {   // Gaussian elimination with partial pivoting (A, b are destroyed)
        for (int c = 0; c < N; c++) {
            int p = c;
            for (int r = c + 1; r < N; r++) if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
            if (std::abs(A[p][c]) < 1e-9f) return false;
            if (p != c) { for (int j = 0; j < N; j++) std::swap(A[p][j], A[c][j]); std::swap(b[p], b[c]); }
            for (int r = c + 1; r < N; r++) {
                const cf f = A[r][c] / A[c][c];
                for (int j = c; j < N; j++) A[r][j] -= f * A[c][j];
                b[r] -= f * b[c];
            }
        }
        for (int r = N - 1; r >= 0; r--) {
            cf acc = b[r];
            for (int j = r + 1; j < N; j++) acc -= A[r][j] * x[j];
            x[r] = acc / A[r][r];
        }
        return true;
    }

    void ddSmooth(cf* zn, const cf* ref, const bool* known) {
        constexpr int W = 10;
        const float esn0 = 1.f / std::max(nv, 1e-6f);
        cf w[kSfSyms + kChirp];
        for (int k = 0; k < kSfSyms + kChirp; k++) {
            if (known[k]) { w[k] = zn[k] * std::conj(ref[k]) * 4.f; continue; }
            bool q = k >= kCtrlStart && k < kCtrlStart + kCtrlSyms;
            if (!q) {
                const int s = (k - kSlot0) / kSlotSyms;
                if (s >= 0 && s < kSlots) q = profileQpsk(slotHint(s));
            }
            const float need = q ? 2.f : 8.f;   // Es/N0 (linear dB-ish gate) where decisions are mostly right
            if (10 * std::log10(esn0) < need) { w[k] = cf(0, 0); continue; }
            const cf d = q ? qpskDecide(zn[k]) : qamDecide(zn[k]);
            w[k] = zn[k] * std::conj(d) / std::max(1e-6f, std::norm(d)) * std::norm(d);
        }
        cf corr[kSfSyms + kChirp];
        cf acc(0, 0);
        const int N = kSfSyms + kChirp;
        for (int k = 0; k < std::min(W, N); k++) acc += w[k];
        for (int k = 0; k < N; k++) {
            if (k + W < N) acc += w[k + W];
            if (k - W - 1 >= 0) acc -= w[k - W - 1];
            corr[k] = acc;
        }
        for (int k = 0; k < N; k++) if (std::abs(corr[k]) > 1e-9f) zn[k] *= std::conj(corr[k]) / std::abs(corr[k]);
    }

    Profile slotHint(int s) const {
        if (ctrlValid && ctrlAge < 30) return lastCtrl.map[s];
        return lastGood[s];
    }

    void onControl(const ControlWord& w) {
        lastCtrl = w; ctrlValid = true; ctrlAge = 0;
        if (w.msgType == kMsgBeacon) {
            BeaconFields f;
            if (msgToBeacon(w.msg, f)) { beacons.push_back(f); if (beacons.size() > 8) beacons.pop_front(); }
        } else if (w.msgType == kMsgSlotCall) {
            std::string c;
            for (int i = 1; i < kCtrlMsgBytes && w.msg[i]; i++) c.push_back((char)w.msg[i]);
            calls.push_back({w.msg[0] & 3, c});
            if (calls.size() > 16) calls.pop_front();
        } else if (w.msgType == kMsgText) {
            if (w.part == 0) { ctrlText.clear(); ctrlTextNext = 0; }
            if (w.part == ctrlTextNext) {
                ctrlText.insert(ctrlText.end(), w.msg, w.msg + kCtrlMsgBytes);
                ctrlTextNext++;
                if (ctrlTextNext >= w.parts) {
                    std::string t(ctrlText.begin(), ctrlText.end());
                    while (!t.empty() && t.back() == 0) t.pop_back();
                    texts.push_back(t);
                    ctrlText.clear(); ctrlTextNext = 0;
                }
            } else { ctrlText.clear(); ctrlTextNext = 0; }
        }
    }

    void onData(const SlotPayload& p) {
        if (p.profile == kIdle || p.stream != 0) return;
        const int nb = dataBytes(p.profile);
        if (p.dataType == 1) {
            dmsg.assign(p.bits, p.bits + nb);
            dlen = ((size_t)p.bits[0] << 8 | p.bits[1]) + 2;
            dactive = true;
            dseq = p.seq;
        } else if (p.dataType == 4 && dactive && p.seq == (uint8_t)(dseq + 1)) {
            dmsg.insert(dmsg.end(), p.bits, p.bits + nb);
            dseq = p.seq;
        } else { dactive = false; return; }
        if (dactive && dmsg.size() >= dlen) {
            texts.push_back(std::string(dmsg.begin() + 2, dmsg.begin() + (long)dlen));
            if (texts.size() > 64) texts.pop_front();
            dactive = false;
        }
    }

    void decodeSlotSyms(int s, const cf* zs, SlotPayload& out) {
        cf d[kSlotData];
        int di = 0;
        for (int i = 0; i < kSlotSyms; i++) if (!isPilot(i)) d[di++] = zs[i];
        // hypotheses, the expected one first
        Profile order[6];
        int no = 0;
        auto add = [&](Profile p) {
            if (p == kIdle) p = kD68;
            for (int i = 0; i < no; i++) if (order[i] == p) return;
            order[no++] = p;
        };
        add(slotHint(s)); add(lastGood[s]);
        add(kD68); add(kV80); add(kV60); add(kV48); add(kV32R); add(kD26R);
        float l16[kSlotCoded16], lq[kSlotCodedQ];
        const std::vector<float> m16 = qam16DemapSoft(d, kSlotData, nv);
        std::copy(m16.begin(), m16.end(), l16);
        qpskDemap(d, kSlotData, nv, lq);
        std::vector<float> post;
        for (int i = 0; i < no; i++) {
            const bool q = profileQpsk(order[i]);
            SlotPayload p;
            if (decodeSlot(s, q ? lq : l16, order[i], p, i == 0 ? 40 : 20, i == 0 ? &post : nullptr)) {
                accept(s, p, out);
                return;
            }
            if (i == 0 && iterative && no > 0) {
                // decision-directed re-estimation from the decoder's soft output: per-symbol phase/gain from a sliding average of z * conj(s^)
                for (int round = 0; round < 2; round++) {
                    cf sh[kSlotData];
                    for (int k = 0; k < kSlotData; k++) sh[k] = q ? qpskSoft(&post[2 * k]) : qamSoft(&post[4 * k]);
                    cf d2[kSlotData];
                    constexpr int W = 12;
                    double num = 0, den = 0;
                    for (int k = 0; k < kSlotData; k++) {
                        cf acc(0, 0);
                        float en = 0;
                        for (int j = std::max(0, k - W); j <= std::min(kSlotData - 1, k + W); j++) { acc += d[j] * std::conj(sh[j]); en += std::norm(sh[j]); }
                        const cf hk = en > 1e-6f ? acc / en : cf(1, 0);
                        d2[k] = std::abs(hk) > 0.2f ? d[k] / hk : d[k];
                        num += std::norm(d2[k] - sh[k]) ; den += 1;
                    }
                    const float nv2 = (float)std::max(1e-5, std::min(1.0, num / den));
                    const float nvUse = std::max(nv2, 0.5f * nv);
                    float l2[kSlotCoded16];
                    if (q) qpskDemap(d2, kSlotData, nvUse, l2);
                    else { const std::vector<float> m2 = qam16DemapSoft(d2, kSlotData, nvUse); std::copy(m2.begin(), m2.end(), l2); }
                    if (decodeSlot(s, l2, order[0], p, 40, &post)) { st.iterativeSaves++; accept(s, p, out); return; }
                    std::copy(d2, d2 + kSlotData, d);
                }
                // the demapped LLRs of the other hypotheses stay the first-pass ones
            }
        }
        out = SlotPayload{};
        out.profile = slotHint(s);
        out.ok = false;
        st.slotsBad++;
    }

    void accept(int s, const SlotPayload& p, SlotPayload& out) {
        out = p;
        lastGood[s] = p.profile == kIdle ? kD68 : p.profile;
        st.lastProfile[s] = p.profile;
        st.slotsOk++;
        if (st.lockTimeS < 0) st.lockTimeS = (double)samplesIn / kFs;
        if (!profileVoice(p.profile)) onData(p);
    }

    void feed48(const cf* in, size_t n) {
        x.insert(x.end(), in, in + n);
        samplesIn += n;
        for (;;) {
            if (!locked) { if (!acquire()) break; continue; }
            if (!processSf()) break;
            if (!locked) {   // lost: restart the search from the current position
                const int64_t keep = (int64_t)std::floor(T - (double)xBase) - kSfSamples;
                if (keep > 0 && keep < (int64_t)x.size()) { x.erase(x.begin(), x.begin() + keep); xBase += keep; }
                continue;
            }
            const int64_t drop = (int64_t)std::floor(T - (double)xBase) - 4 * kMargin;
            if (drop > 4096) { x.erase(x.begin(), x.begin() + drop); xBase += drop; }
        }
        st.locked = locked;
        st.samplesIn = samplesIn;
    }
};

FrameRx::FrameRx() : p_(new Impl) {}
FrameRx::~FrameRx() = default;
void FrameRx::configure(double inRate) {
    p_->inRate = inRate;
    p_->resampling = std::fabs(inRate - kFs) > 1e-6;
    if (p_->resampling) p_->rs.configure(inRate, kFs);
    p_->resetAll();
}
void FrameRx::reset() { p_->resetAll(); }
void FrameRx::setIterative(bool on) { p_->iterative = on; }
void FrameRx::setDdTracking(bool on) { p_->ddTrack = on; }
void FrameRx::setEqualiser(bool on) { p_->eqOn = on; }
void FrameRx::feed(const cf* x, size_t n) {
    if (p_->resampling) {
        p_->rsOut.clear();
        p_->rs.process(x, n, p_->rsOut);
        p_->feed48(p_->rsOut.data(), p_->rsOut.size());
    } else p_->feed48(x, n);
}
bool FrameRx::popSuperframe(RxSuperframe& out) {
    if (p_->outSf.empty()) return false;
    out = p_->outSf.front(); p_->outSf.pop_front();
    return true;
}
bool FrameRx::pollText(std::string& t) { if (p_->texts.empty()) return false; t = p_->texts.front(); p_->texts.pop_front(); return true; }
bool FrameRx::pollBeacon(BeaconFields& f) { if (p_->beacons.empty()) return false; f = p_->beacons.front(); p_->beacons.pop_front(); return true; }
bool FrameRx::pollSlotCall(int& s, std::string& c) {
    if (p_->calls.empty()) return false;
    s = p_->calls.front().first; c = p_->calls.front().second; p_->calls.pop_front();
    return true;
}
ControlWord FrameRx::lastControl() const { return p_->lastCtrl; }
RxStatsV4 FrameRx::stats() const { return p_->st; }
void FrameRx::setLogCallback(std::function<void(const std::string&)> cb) { p_->log = std::move(cb); }

}  // namespace dect2::kaal::v4

namespace dect2::kaal::v4 {
namespace {
std::mutex& codecMu() { static std::mutex m; return m; }
SlotCodec& codecSlot() { static SlotCodec c; return c; }
}  // namespace
void registerSlotCodec(const SlotCodec& c) { std::lock_guard<std::mutex> lk(codecMu()); codecSlot() = c; }
const SlotCodec* slotCodec() {
    std::lock_guard<std::mutex> lk(codecMu());
    return codecSlot().makeEncoder && codecSlot().makeDecoder ? &codecSlot() : &kv4SlotCodec();
}
}  // namespace dect2::kaal::v4
