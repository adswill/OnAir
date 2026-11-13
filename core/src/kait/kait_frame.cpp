// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT framing and transmit side (see kait_frame.h).
#include "dect2/kait/kait_frame.h"
#include <algorithm>
#include <cmath>

namespace dect2::kait {

using kris::LdpcRate;
using kris::Mod;

namespace {
constexpr double kPi = 3.14159265358979323846;

void putBits(std::vector<uint8_t>& b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back((uint8_t)((v >> i) & 1u));
}
uint64_t getBits(const uint8_t* b, size_t& pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (uint64_t)(b[pos++] & 1u);
    return v;
}
int charCode(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == '-') return 38;
    if (c == '.') return 39;
    return 37;   // '/' and anything else
}
char codeChar(int v) {
    if (v >= 1 && v <= 26) return (char)('A' + v - 1);
    if (v >= 27 && v <= 36) return (char)('0' + v - 27);
    if (v == 37) return '/';
    if (v == 38) return '-';
    if (v == 39) return '.';
    return 0;   // 0 = pad, others invalid
}
int modIndex(Mod m) {
    switch (m) {
        case Mod::Bpsk: return 0;
        case Mod::Qpsk: return 1;
        case Mod::Qam16: return 2;
        case Mod::Qam64: return 3;
        case Mod::Qam256: return 4;
        default: return 5;
    }
}
bool modFromIndex(int i, Mod& m) {
    static const Mod t[6] = {Mod::Bpsk, Mod::Qpsk, Mod::Qam16, Mod::Qam64, Mod::Qam256, Mod::Qam1024};
    if (i < 0 || i > 5) return false;
    m = t[i];
    return true;
}
}  // namespace

const std::array<Profile, kNumProfiles> kProfiles{{
    // needEsN0Db: Es/N0 where the n = 7680 block fails about 1 time in 100 on a clean AWGN channel (tests/kait/test_kait_phy.cpp, KAIT-MEASURED-v0.1.md);
    // a repetition of r divides it by r (-10 log10 r dB). The whole chain needs about 1 dB more.
    // 0 balanced: doubles the bytes about every 3 dB of SNR from -12 dB up to 10 dB
    {0, "Balanced", 8, {{
        {Mod::Bpsk, LdpcRate::R1_4, 8, 20, -11.8f}, {Mod::Bpsk, LdpcRate::R1_4, 4, 8, -8.8f}, {Mod::Bpsk, LdpcRate::R1_4, 2, 8, -5.8f},
        {Mod::Bpsk, LdpcRate::R1_4, 1, 6, -2.8f}, {Mod::Qpsk, LdpcRate::R1_4, 1, 7, 0.2f}, {Mod::Qpsk, LdpcRate::R2_3, 1, 8, 3.5f},
        {Mod::Qpsk, LdpcRate::R9_10, 1, 22, 6.8f}, {Mod::Qam16, LdpcRate::R3_4, 1, 21, 10.5f}}}},
    // 1 robust: nothing above QPSK 2/3 (works from 3 dB up), more time on the weak layers
    {1, "Robust", 6, {{
        {Mod::Bpsk, LdpcRate::R1_4, 8, 30, -11.8f}, {Mod::Bpsk, LdpcRate::R1_4, 4, 15, -8.8f}, {Mod::Bpsk, LdpcRate::R1_4, 2, 15, -5.8f},
        {Mod::Bpsk, LdpcRate::R1_4, 1, 12, -2.8f}, {Mod::Qpsk, LdpcRate::R1_4, 1, 12, 0.2f}, {Mod::Qpsk, LdpcRate::R2_3, 1, 16, 3.5f},
        {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f}, {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f}}}},
    // 2 quality: for a good link (above 6 dB): a small thumbnail, then big layers up to 64QAM 5/6
    {2, "Quality", 8, {{
        {Mod::Bpsk, LdpcRate::R1_4, 4, 6, -8.8f}, {Mod::Bpsk, LdpcRate::R1_4, 1, 5, -2.8f}, {Mod::Qpsk, LdpcRate::R1_4, 1, 5, 0.2f},
        {Mod::Qpsk, LdpcRate::R2_3, 1, 8, 3.5f}, {Mod::Qpsk, LdpcRate::R9_10, 1, 20, 6.8f}, {Mod::Qam16, LdpcRate::R3_4, 1, 22, 10.5f},
        {Mod::Qam16, LdpcRate::R5_6, 1, 14, 11.8f}, {Mod::Qam64, LdpcRate::R5_6, 1, 20, 17.2f}}}},
    // 3 direct: for a link that holds 8 dB (QPSK 9/10 passes from about 7 dB): a thumbnail and then nearly all the time on one big layer
    {3, "Direct", 4, {{
        {Mod::Bpsk, LdpcRate::R1_4, 8, 5, -11.8f}, {Mod::Bpsk, LdpcRate::R1_4, 2, 3, -5.8f}, {Mod::Qpsk, LdpcRate::R1_4, 1, 3, 0.2f},
        {Mod::Qpsk, LdpcRate::R9_10, 1, 89, 6.8f}, {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f}, {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f},
        {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f}, {Mod::Bpsk, LdpcRate::R1_4, 1, 0, 0.f}}}},
}};

const Profile* profileById(int id) { return id >= 0 && id < kNumProfiles ? &kProfiles[(size_t)id] : nullptr; }

int infoBitsPerBlock(LdpcRate r) { return kris::ldpcK(kLdpcN, r); }
int payloadBytesPerBlock(LdpcRate r) {
    const int k = infoBitsPerBlock(r);
    return k > kBlockCrcBits ? (k - kBlockCrcBits) / 8 : 0;
}
int symbolsPerBlock(const LayerSpec& l) { return kLdpcN / (int)l.mod * std::max(1, l.rep); }
long layerDataSymbols(const LayerSpec& l) { return (long)l.blocks * symbolsPerBlock(l); }

long Header::dataSymbols() const {
    long d = kHeaderDataSyms;
    for (int i = 0; i < nLayers; i++) d += layerDataSymbols(layers[(size_t)i]);
    return d;
}
size_t Header::streamBytes() const {
    size_t n = 0;
    for (int i = 0; i < nLayers; i++) n += (size_t)layers[(size_t)i].blocks * (size_t)payloadBytesPerBlock(layers[(size_t)i].rate);
    return n;
}
long Header::slots() const {
    const long d = dataSymbols();
    return d + d / kPilotGroup;
}
long Header::burstSamples() const { return endSweepOffset(*this) + 2L * kSweepSamples; }
long endSweepOffset(const Header& h) { return 2L * kSweepSamples + kGuardSamples + (long)kSps * h.slots() + kGuardSamples; }

std::vector<uint8_t> headerInfoBits(const Header& h) {
    std::vector<uint8_t> b;
    b.reserve(kHeaderInfoBits);
    putBits(b, 1, 4);
    for (int i = 0; i < kMaxCallsign; i++) putBits(b, i < (int)h.callsign.size() ? (uint64_t)charCode(h.callsign[(size_t)i]) : 0u, 6);
    putBits(b, (uint64_t)h.width, 13);
    putBits(b, (uint64_t)h.height, 13);
    putBits(b, (uint64_t)h.levels, 3);
    putBits(b, (uint64_t)(h.topPlane + 1), 4);
    putBits(b, (uint64_t)h.profileId, 3);
    putBits(b, (uint64_t)h.nLayers, 4);
    for (int i = 0; i < kMaxLayers; i++) {
        const LayerSpec& l = h.layers[(size_t)i];
        putBits(b, (uint64_t)modIndex(l.mod), 3);
        putBits(b, (uint64_t)l.rate, 3);
        putBits(b, (uint64_t)(std::max(1, l.rep) - 1), 3);
        putBits(b, (uint64_t)l.blocks, 9);
    }
    putBits(b, h.decisions, 32);
    const std::vector<uint8_t> by = [&] {
        std::vector<uint8_t> o((b.size() + 7) / 8, 0);
        for (size_t i = 0; i < b.size(); i++) o[i / 8] = (uint8_t)(o[i / 8] | (b[i] << (7 - i % 8)));
        return o;
    }();
    putBits(b, kris::crc16(by.data(), by.size()), 16);
    return b;
}

bool parseHeaderBits(const uint8_t* bits, Header& h) {
    const size_t body = kHeaderInfoBits - 16;
    std::vector<uint8_t> by((body + 7) / 8, 0);
    for (size_t i = 0; i < body; i++) by[i / 8] = (uint8_t)(by[i / 8] | ((bits[i] & 1u) << (7 - i % 8)));
    size_t pos = body;
    if (getBits(bits, pos, 16) != kris::crc16(by.data(), by.size())) return false;
    pos = 0;
    if (getBits(bits, pos, 4) != 1) return false;
    Header o;
    for (int i = 0; i < kMaxCallsign; i++) {
        const int v = (int)getBits(bits, pos, 6);
        if (v == 0) continue;
        const char c = codeChar(v);
        if (!c) return false;
        o.callsign.push_back(c);
    }
    o.width = (int)getBits(bits, pos, 13);
    o.height = (int)getBits(bits, pos, 13);
    o.levels = (int)getBits(bits, pos, 3);
    o.topPlane = (int)getBits(bits, pos, 4) - 1;
    o.profileId = (int)getBits(bits, pos, 3);
    o.nLayers = (int)getBits(bits, pos, 4);
    if (o.nLayers > kMaxLayers || o.width < 1 || o.height < 1 || o.levels < 1) return false;
    for (int i = 0; i < kMaxLayers; i++) {
        LayerSpec& l = o.layers[(size_t)i];
        if (!modFromIndex((int)getBits(bits, pos, 3), l.mod)) return false;
        const int r = (int)getBits(bits, pos, 3);
        if (r > 6) return false;
        l.rate = static_cast<LdpcRate>(r);
        l.rep = (int)getBits(bits, pos, 3) + 1;
        l.blocks = (int)getBits(bits, pos, 9);
        if (i < o.nLayers && (infoBitsPerBlock(l.rate) <= kBlockCrcBits || (l.rep > 1 && l.mod != Mod::Bpsk))) return false;
    }
    o.decisions = (uint32_t)getBits(bits, pos, 32);
    h = o;
    return true;
}

std::vector<uint8_t> scramble(uint32_t seed, int nBits) {
    kris::XorShift64 g{0x9E3779B97F4A7C15ull * ((uint64_t)seed + 1) ^ 0x4B414954534352ull};
    std::vector<uint8_t> out((size_t)nBits);
    uint64_t w = 0;
    for (int i = 0; i < nBits; ++i) {
        if ((i & 63) == 0) w = g.next();
        out[(size_t)i] = (uint8_t)((w >> (i & 63)) & 1u);
    }
    return out;
}

float pilotSign(long i) {
    uint64_t z = (uint64_t)i * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (z & 1u) ? -1.f : 1.f;
}

std::vector<cf> headerSymbols(const Header& h) {
    const std::vector<uint8_t> info = headerInfoBits(h);
    const std::vector<uint8_t> coded = kris::convEncode(1, info, true);
    std::vector<cf> out;
    out.reserve((size_t)kHeaderDataSyms);
    for (int c = 0; c < kHeaderRepeat; c++) {
        const std::vector<uint8_t> pn = scramble(0x4000u + (uint32_t)c, (int)coded.size());
        std::vector<uint8_t> bits(coded.size());
        for (size_t i = 0; i < coded.size(); i++) bits[i] = (uint8_t)((coded[i] ^ pn[i]) & 1u);
        const std::vector<cf> s = kris::qamMap(bits.data(), bits.size(), Mod::Bpsk);
        out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

long slotsForSeconds(double seconds) {
    return (long)std::floor((seconds * kInternalRate - 4.0 * kSweepSamples - 2.0 * kGuardSamples) / kSps);
}
long maxDataSymbols(double seconds) { return slotsForSeconds(seconds) * kPilotGroup / kPilotPeriod; }

std::vector<LayerSpec> planLayers(int profileId, double seconds) {
    const Profile* p = profileById(profileId);
    std::vector<LayerSpec> out;
    if (!p) return out;
    const long budget = maxDataSymbols(seconds) - kHeaderDataSyms;
    long used = 0;
    for (int i = 0; i < p->nLayers; i++) {
        const LayerShape& s = p->layers[(size_t)i];
        if (s.sharePercent <= 0) continue;
        LayerSpec l;
        l.mod = s.mod; l.rate = s.rate; l.rep = s.rep;
        const long spb = symbolsPerBlock(l);
        l.blocks = (int)std::min<long>(511, std::max<long>(1, budget * s.sharePercent / 100 / spb));
        used += l.blocks * spb;
        out.push_back(l);
    }
    // what rounding left over goes to the last layer
    while (!out.empty()) {
        LayerSpec& l = out.back();
        const long spb = symbolsPerBlock(l);
        if (l.blocks >= 511 || used + spb > budget) break;
        l.blocks++;
        used += spb;
    }
    return out;
}

static BurstPlan finishPlan(const Encoded& enc, const std::string& callsign, int profileId, double seconds) {
    BurstPlan plan;
    const std::vector<LayerSpec> ls = planLayers(profileId, seconds);
    if (ls.empty()) return plan;
    Header& h = plan.hdr;
    h.callsign = callsign.substr(0, (size_t)kMaxCallsign);
    h.width = enc.info.w;
    h.height = enc.info.h;
    h.levels = enc.info.levels;
    h.topPlane = enc.info.topPlane;
    h.profileId = profileId;
    h.nLayers = (int)ls.size();
    for (size_t i = 0; i < ls.size(); i++) h.layers[i] = ls[i];
    h.decisions = enc.info.decisions;
    plan.stream = enc.bytes;
    plan.stream.resize(h.streamBytes(), 0);
    plan.ok = true;
    return plan;
}

BurstPlan planBurstFromStream(const Encoded& enc, const std::string& callsign, int profileId, double seconds) {
    BurstPlan p = finishPlan(enc, callsign, profileId, seconds);
    if (p.ok && enc.bytes.size() > p.hdr.streamBytes()) p.ok = false;
    return p;
}

BurstPlan planBurst(const Image& img, const std::string& callsign, int profileId, double seconds) {
    const std::vector<LayerSpec> ls = planLayers(profileId, seconds);
    if (ls.empty()) return BurstPlan{};
    size_t cap = 0;
    for (const LayerSpec& l : ls) cap += (size_t)l.blocks * (size_t)payloadBytesPerBlock(l.rate);
    return finishPlan(encodeImage(img, cap), callsign, profileId, seconds);
}

const std::vector<float>& rrcShape() {
    static const std::vector<float> h = [] {
        std::vector<float> t = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
        for (float& v : t) v *= (float)std::sqrt((double)kSps);
        return t;
    }();
    return h;
}

static std::vector<cf> chirp(bool up) {
    std::vector<cf> out((size_t)kSweepSamples);
    const double T = (double)kSweepSamples / kInternalRate, B = 2.0 * kSweepHalfBandHz, k = B / T;
    const int taper = 1000;
    for (int n = 0; n < kSweepSamples; n++) {
        const double t = (double)n / kInternalRate;
        const double ph = kPi * k * t * t - kPi * B * t;
        double a = 1.0;
        if (n < taper) a = 0.5 - 0.5 * std::cos(kPi * n / taper);
        else if (n >= kSweepSamples - taper) a = 0.5 - 0.5 * std::cos(kPi * (kSweepSamples - 1 - n) / taper);
        const cf v{(float)(a * std::cos(ph)), (float)(a * std::sin(ph))};
        out[(size_t)n] = up ? v : std::conj(v);
    }
    return out;
}
std::vector<cf> sweepUp() {
    static const std::vector<cf> s = chirp(true);
    return s;
}
std::vector<cf> sweepDown() {
    static const std::vector<cf> s = chirp(false);
    return s;
}

std::vector<cf> buildSlots(const BurstPlan& plan) {
    std::vector<cf> out;
    if (!plan.ok) return out;
    const Header& h = plan.hdr;
    std::vector<cf> data = headerSymbols(h);
    data.reserve((size_t)h.dataSymbols());
    size_t off = 0;
    uint32_t gb = 0;
    for (int li = 0; li < h.nLayers; li++) {
        const LayerSpec& l = h.layers[(size_t)li];
        const int K = infoBitsPerBlock(l.rate), pb = payloadBytesPerBlock(l.rate);
        std::vector<uint8_t> info((size_t)K), cw((size_t)kLdpcN);
        for (int b = 0; b < l.blocks; b++, gb++) {
            const uint8_t* pay = plan.stream.data() + off;
            off += (size_t)pb;
            for (int i = 0; i < pb * 8; i++) info[(size_t)i] = (uint8_t)((pay[i / 8] >> (7 - i % 8)) & 1u);
            const uint16_t crc = kris::crc16(pay, (size_t)pb);
            for (int i = 0; i < 16; i++) info[(size_t)(pb * 8 + i)] = (uint8_t)((crc >> (15 - i)) & 1u);
            kris::ldpcEncode(kLdpcN, l.rate, info.data(), cw.data());
            for (int c = 0; c < l.rep; c++) {
                const std::vector<uint8_t> pn = scramble(0x10000u + gb * 8u + (uint32_t)c, kLdpcN);
                std::vector<uint8_t> bits(cw);
                for (size_t i = 0; i < bits.size(); i++) bits[i] = (uint8_t)((bits[i] ^ pn[i]) & 1u);
                const std::vector<cf> s = kris::qamMap(bits.data(), bits.size(), l.mod);
                data.insert(data.end(), s.begin(), s.end());
            }
        }
    }
    out.reserve((size_t)h.slots());
    long pi = 0;
    for (size_t i = 0; i < data.size(); i++) {
        out.push_back(data[i]);
        if ((i + 1) % (size_t)kPilotGroup == 0) out.push_back(cf{pilotSign(pi++), 0.f});
    }
    return out;
}

std::vector<cf> buildBurst(const BurstPlan& plan) {
    if (!plan.ok) return {};
    const std::vector<cf> slots = buildSlots(plan);
    std::vector<cf> out((size_t)plan.hdr.burstSamples(), cf{0.f, 0.f});
    const std::vector<cf> up = sweepUp(), dn = sweepDown();
    const long e0 = endSweepOffset(plan.hdr);
    std::copy(up.begin(), up.end(), out.begin());
    std::copy(dn.begin(), dn.end(), out.begin() + kSweepSamples);
    std::copy(up.begin(), up.end(), out.begin() + e0);
    std::copy(dn.begin(), dn.end(), out.begin() + e0 + kSweepSamples);
    const std::vector<float>& h = rrcShape();
    const long half = (long)(h.size() - 1) / 2;
    for (size_t k = 0; k < slots.size(); k++) {
        const long c = 2L * kSweepSamples + kGuardSamples + (long)k * kSps;
        const cf s = slots[k];
        for (size_t t = 0; t < h.size(); t++) out[(size_t)(c - half + (long)t)] += s * h[t];
    }
    return out;
}

}  // namespace dect2::kait
