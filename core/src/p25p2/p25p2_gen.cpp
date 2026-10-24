// P25 Phase 2 test signal (see p25p2_gen.h).
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace dect2 {

namespace {
uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// What a logical channel sends in its p-th burst of the superframe during a call (as heard on the NSW PSN): channel 1 (slots 0, 2, 4, 6, 8, 11)
// 4V 4V 4V 4V 2V SACCH, channel 2 (slots 1, 3, 5, 7, 9, 10) 4V 4V 4V 2V 4V SACCH
enum Kind { k4V, k2V, kSig };
Kind kindOf(int channel, int p) {
    if (p == 5) return kSig;
    if (channel == 1) return p == 4 ? k2V : k4V;
    return p == 3 ? k2V : k4V;
}
// the p-th burst of logical channel 1 / 2 in the superframe is in slot ...
const int kSlotOf[2][6] = {{0, 2, 4, 6, 8, 11}, {1, 3, 5, 7, 9, 10}};

const P25p2GenCall* callAt(const P25p2GenConfig& c, int channel, int sf) {
    for (const auto& k : c.calls)
        if (k.channel == channel && sf >= k.startSf && sf < k.startSf + k.nSf) return &k;
    return nullptr;
}

// a burst of the call that carries voice: neither the PTT (first burst of the first superframe) nor the END_PTT (last burst of the last)
bool carriesVoice(const P25p2GenCall& k, int sf, int p) {
    if (sf == k.startSf && p == 0) return false;
    if (sf == k.startSf + k.nSf - 1 && p == 5) return false;
    return p != 5;
}
} // namespace

void p25p2VoiceBits(int channel, int index, uint8_t bits[72]) {
    uint64_t x = mix(((uint64_t)channel << 32) ^ (uint64_t)index);
    for (int i = 0; i < 72; i++) {
        if (i % 64 == 0 && i) x = mix(x);
        bits[i] = (uint8_t)((x >> (i % 64)) & 1);
    }
}

int p25p2VoiceFrames(const P25p2GenConfig& c, int channel) {
    int n = 0;
    for (const auto& k : c.calls) {
        if (k.channel != channel) continue;
        for (int sf = k.startSf; sf < k.startSf + k.nSf && sf < c.superframes; sf++)
            for (int p = 0; p < 6; p++)
                if (carriesVoice(k, sf, p)) n += kindOf(channel, p) == k4V ? 4 : 2;
    }
    return n;
}

std::vector<uint8_t> p25p2Dibits(const P25p2GenConfig& c) {
    std::vector<uint8_t> out;
    uint64_t rs = mix(c.seed);
    auto rnd = [&]() { rs = mix(rs); return (uint8_t)(rs & 3); };
    for (int i = 0; i < c.leadDibits; i++) out.push_back(rnd());
    uint8_t seq[p25p2::kScrambleDibits];
    p25p2::scrambleSeq(c.scramble ? c.wacn : 0, c.scramble ? c.sysId : 0, c.scramble ? c.colorCode : 0, seq);
    int vidx[3] = {0, 0, 0};
    int essPart[3] = {0, 0, 0};
    size_t sysIdx = 0;
    for (int sf = 0; sf < c.superframes; sf++) {
        uint8_t burst[p25p2::kSlotsPerSuperframe][p25p2::kBurstDibits];
        for (int ch = 1; ch <= 2; ch++)
            for (int p = 0; p < 6; p++) {
                const int s = kSlotOf[ch - 1][p];
                uint8_t* b = burst[s];
                for (int i = 0; i < p25p2::kBurstDibits; i++) b[i] = rnd();
                const P25p2GenCall* call = callAt(c, ch, sf);
                auto sig = [&](bool facch, const std::vector<uint8_t>& mac) {
                    uint8_t cw[p25p2::kFacchDibits];
                    p25p2::macEncode(facch, mac.data(), cw);
                    p25p2::payloadPut(cw, facch ? p25p2::kFacchDibits : p25p2::kSacchDibits, b);
                    p25p2::duidPut(facch ? p25p2::kDuidFacch : p25p2::kDuidSacch, b);
                };
                if (!call) {
                    const bool facch = p != 5;
                    const int n = facch ? p25p2::kFacchMacBytes : p25p2::kSacchMacBytes;
                    sig(facch, c.sysMsgs.empty() ? p25p2::macIdle(n) : p25p2::macSignal(n, {c.sysMsgs[sysIdx++ % c.sysMsgs.size()]}));
                    continue;
                }
                const bool first = sf == call->startSf, last = sf == call->startSf + call->nSf - 1;
                if (first && p == 0) { sig(true, p25p2::macPtt(p25p2::kFacchMacBytes, call->src, call->group, call->algId)); continue; }
                if (last && p == 5) { sig(true, p25p2::macEndPtt(p25p2::kFacchMacBytes, call->src, call->group, c.colorCode)); continue; }
                if (p == 5) {
                    sig(false, call->unitToUnit ? p25p2::macActiveUu(p25p2::kSacchMacBytes, call->svcOpts, call->group, call->src)
                                                : p25p2::macActive(p25p2::kSacchMacBytes, call->svcOpts, call->group, call->src));
                    continue;
                }
                // voice: the frames, and the call's encryption sync (ESS-B a quarter at a time in the 4V bursts, ESS-A in the 2V)
                uint8_t essB[16], essA[28], mi[9] = {};
                for (int i = 0; i < 9; i++) mi[i] = (uint8_t)(call->algId == p25p2::kAlgClear ? 0 : 0x11 * (i + 1));
                p25p2::essEncode(call->algId, call->algId == p25p2::kAlgClear ? 0 : 0x1234, mi, essB, essA);
                const bool v4 = kindOf(ch, p) == k4V;
                for (int k = 0; k < (v4 ? 4 : 2); k++) {
                    uint8_t bits[72];
                    char fr[4][24];
                    p25p2VoiceBits(ch, vidx[ch]++, bits);
                    p25p2::ambeUnflatten(bits, fr);
                    p25p2::ambePut(fr, p25p2::kVoiceOff[k], b);
                }
                if (v4) {
                    if (essPart[ch] >= 4) essPart[ch] = 0;
                    p25p2::hexbitsPut(essB + 4 * essPart[ch]++, 4, b + p25p2::kEssBPos);
                } else {
                    p25p2::hexbitsPut(essA, 16, b + p25p2::kEssA1Pos);
                    p25p2::hexbitsPut(essA + 16, 12, b + p25p2::kEssA2Pos);
                    essPart[ch] = 0;
                }
                p25p2::duidPut(v4 ? p25p2::kDuid4V : p25p2::kDuid2V, b);
            }
        for (int s = 0; s < p25p2::kSlotsPerSuperframe; s++) {
            uint8_t* b = burst[s];
            // scrambled: everything but the ISCH and the DUID
            for (int i = p25p2::kIschDibits; i < p25p2::kBurstDibits; i++)
                if (i != p25p2::kDuidPos[0] && i != p25p2::kDuidPos[1] && i != p25p2::kDuidPos[2] && i != p25p2::kDuidPos[3]) b[i] ^= seq[s * p25p2::kSlotDibits + i];
            // the ISCH: the sync word in slots 2, 3, 6, 7, 10, 11, an I-ISCH (location, channel, ultraframe count) in the others
            if ((s & 2) != 0) p25p2::ischSync(b);
            else p25p2::ischInfo(p25p2::ischInfoOf(s & 1, s / 4, 0, sf & 3), b);
            out.insert(out.end(), b, b + p25p2::kBurstDibits);
        }
    }
    return out;
}

std::vector<cf32> p25p2Modulate(const std::vector<uint8_t>& dibits, const P25p2GenConfig& c) {
    const double pi = 3.14159265358979323846;
    const int sps = std::max(2, (int)std::lround(c.rate / p25p2::kSymbolRate));
    const double fs = sps * p25p2::kSymbolRate;
    const int span = 6;
    std::vector<double> g((size_t)(2 * span * sps + 1));
    for (size_t i = 0; i < g.size(); i++) g[i] = p25p2::rrcImpulse(((double)i - span * sps) / sps, p25p2::kRollOff);
    const size_t nSym = dibits.size();
    std::vector<std::complex<double>> sym(nSym);
    int phase8 = 0;   // multiples of pi/4
    for (size_t k = 0; k < nSym; k++) {
        phase8 += p25p2::dibitStep(dibits[k]);
        sym[k] = std::polar(1.0, phase8 * pi / 4);
    }
    const size_t nOut = (nSym + 2 * span) * (size_t)sps;
    std::vector<std::complex<double>> y(nOut);
    for (size_t k = 0; k < nSym; k++) {
        const size_t centre = (k + span) * (size_t)sps;
        for (int j = -span * sps; j <= span * sps; j++) y[centre + (size_t)(j + 0)] += sym[k] * g[(size_t)(j + span * sps)];
    }
    double pw = 0;
    for (const auto& v : y) pw += std::norm(v);
    const double scale = pw > 0 ? 1.0 / std::sqrt(pw / (double)nOut) : 1.0;
    std::mt19937_64 rng(c.seed * 977 + 13);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double nsig = std::sqrt(std::pow(10.0, -c.snrDb / 10.0) * (fs / p25p2::kSymbolRate) / 2.0);
    std::vector<cf32> out(nOut);
    for (size_t n = 0; n < nOut; n++) {
        std::complex<double> v = y[n] * scale * std::polar(1.0, 2 * pi * c.cfoHz * (double)n / fs);
        if (c.snrDb < 200) v += std::complex<double>(nd(rng) * nsig, nd(rng) * nsig);
        out[n] = cf32((float)v.real(), (float)v.imag());
    }
    return out;
}

} // namespace dect2
