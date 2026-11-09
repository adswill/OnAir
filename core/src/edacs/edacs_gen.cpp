// EDACS whole-system test signal (see edacs_gen.h).
#include "dect2/edacs/edacs_gen.h"
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/provoice.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

void edacsPvInfo(uint32_t seed, int frame, uint8_t info[88]) {
    std::mt19937 r(seed * 977u + (uint32_t)frame);
    for (int i = 0; i < 88; i++) info[i] = (uint8_t)(r() & 1);
    // a fundamental that gives a K well inside the table (the speech parameters of a real call do this)
    for (int i = 1; i <= 6; i++) info[i] = (uint8_t)((60 >> (6 - i)) & 1);
    info[86] = 1; info[87] = 0;
    info[0] = 0;
}

std::vector<cf32> edacsSystemSignal(const EdacsGenConfig& c) {
    const double pi = 3.14159265358979323846;
    const size_t total = (size_t)std::llround(c.durSec * c.rate);
    // ---- control channel: random lead-in bits (the slicer thresholds settle on them), then a frame every 30 ms
    std::vector<uint8_t> bits;
    std::mt19937 rng(c.seed);
    for (int i = 0; i < 400; i++) bits.push_back((uint8_t)(rng() & 1));
    const double leadSec = 400.0 / edacs::kBaud, frameSec = (double)edacs::kFrameBits / edacs::kBaud;
    const int nFrames = (int)((c.durSec - leadSec) / frameSec) - 1;
    for (int k = 0; k < nFrames; k++) {
        const double t = leadSec + k * frameSec;
        uint32_t a, b;
        edacs::Msg s;
        if (k % 10 == 0) {
            s.type = edacs::MsgType::SiteId; s.site = c.siteId; s.area = c.layout.extended ? c.systemId : -1; s.lcn = c.controlLcn; s.priority = 1;
            edacs::build(s, c.layout, a, b);
            if (!c.layout.extended) { b = (uint32_t)(rng() & 0x0FFFFFF); }
        } else {
            // idle filler words, different every frame (valid check bits; a reserved command, so they announce nothing)
            const uint32_t top = c.layout.extended ? 0xF700000u : 0xFFE0000u;   // standard: MT-A 7, MT-B 7, MT-D 0x1F; EA: MT1 0x1F, MT2 0xE
            a = top | (uint32_t)(rng() & (c.layout.extended ? 0x7FFFF : 0x1FFFF)); b = (uint32_t)(rng() & 0x0FFFFFF);
        }
        for (const auto& call : c.calls) {
            if (!call.announce) continue;
            const double t0 = call.startSec - call.announceLeadSec;
            if (t >= t0 && t < call.endSec && k % 3 == 1) {   // the assignment repeats every third frame while the call lasts
                edacs::Msg m;
                m.type = call.group ? edacs::MsgType::GroupAssign : edacs::MsgType::IndividualAssign; m.lcn = call.lcn; m.emergency = call.emergency;
                m.digital = call.digital;
                m.gid = call.group ? call.gid & 0x7FF : call.gid & 0x3FFF; m.lid = call.lid & 0x3FFF; m.source = call.lid & 0x3FFF; m.individual = !call.group;
                edacs::build(m, c.layout, a, b);
            }
            if (call.sendEnd && t >= call.endSec && t < call.endSec + 3 * frameSec && k % 3 == 2) {
                edacs::Msg m; m.type = edacs::MsgType::CallEnd; m.lcn = call.lcn; m.lid = 0;
                edacs::build(m, c.layout, a, b);
            }
        }
        const auto f = edacs::encodeFrame(a, b);
        bits.insert(bits.end(), f.begin(), f.end());
    }
    for (int i = 0; i < 80; i++) bits.push_back((uint8_t)(rng() & 1));
    const double ctlOff = c.controlHz - c.centreHz;
    std::vector<cf32> sig = edacs::modulate(bits, c.rate, ctlOff, c.controlDevHz, c.controlAmp);
    sig.resize(total, cf32(0, 0));
    // ---- voice channels
    for (const auto& call : c.calls) {
        if (call.lcn < 1 || (size_t)call.lcn > c.lcnFreqHz.size()) continue;
        const double off = c.lcnFreqHz[(size_t)call.lcn - 1] - c.centreHz;
        const size_t i0 = (size_t)std::llround(call.startSec * c.rate), i1 = std::min(total, (size_t)std::llround(call.endSec * c.rate));
        if (call.digital) {
            // ProVoice: blocks of four generated IMBE frames; the bit stream runs for the whole call
            std::vector<uint8_t> vb;
            const int blocks = (int)((call.endSec - call.startSec) * edacs::kBaud / provoice::kBlockBits) + 2;
            int frame = 0;
            for (int b = 0; b < blocks; b++) {
                provoice::Frame7100 f[4];
                for (int k = 0; k < 4; k++) { uint8_t info[88]; edacsPvInfo(call.pvSeed, frame++, info); provoice::encode7100(info, f[k]); }
                const auto block = provoice::makeBlock(f, provoice::kSyncStd, (uint16_t)(call.lid & 0x3FFF));
                vb.insert(vb.end(), block.begin(), block.end());
            }
            std::vector<cf32> v = edacs::modulate(vb, c.rate, off, 2400.0, call.amp, true);   // payload bit 1 is the lower frequency
            for (size_t i = i0; i < i1 && i - i0 < v.size(); i++) sig[i] += v[i - i0];
            continue;
        }
        double ph = 0;
        for (size_t i = i0; i < i1; i++) {
            const double t = (double)i / c.rate;
            ph += 2 * pi * (off + call.devHz * std::sin(2 * pi * call.toneHz * t)) / c.rate;
            if (ph > pi) ph -= 2 * pi; else if (ph < -pi) ph += 2 * pi;
            const double ramp = std::min({1.0, (double)(i - i0) / (0.005 * c.rate), (double)(i1 - i) / (0.005 * c.rate)});   // a 5 ms key-up / key-down ramp
            const float a = call.amp * (float)(0.5 - 0.5 * std::cos(pi * ramp));
            sig[i] += cf32(a * (float)std::cos(ph), a * (float)std::sin(ph));
        }
    }
    // ---- carrier offset and noise
    std::mt19937_64 nr(c.seed * 31 + 7);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (size_t i = 0; i < total; i++) {
        std::complex<double> v(sig[i].real(), sig[i].imag());
        if (c.cfoHz != 0) v *= std::polar(1.0, 2 * pi * c.cfoHz * (double)i / c.rate);
        if (c.noise > 0) v += std::complex<double>(nd(nr) * c.noise, nd(nr) * c.noise);
        sig[i] = cf32((float)v.real(), (float)v.imag());
    }
    return sig;
}

} // namespace dect2
