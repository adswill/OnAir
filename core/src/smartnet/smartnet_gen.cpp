// Motorola Type II test signal (see smartnet_gen.h).
#include "dect2/smartnet/smartnet_gen.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <random>

namespace dect2 {

using smartnet::Osw;

std::vector<Osw> smartnetGrantOsws(const SmartnetGenCall& c) {
    Osw a, b;
    a.addr = c.src; a.group = c.group; a.cmd = c.digital ? 0x321 : 0x308;
    b.addr = (c.tg & 0xFFF0) | (c.options & 0xF); b.group = c.group; b.cmd = c.chan;
    return {a, b};
}

std::vector<cf32> smartnetSystemSignal(const SmartnetGenConfig& c) {
    const size_t N = (size_t)(c.durSec * c.rate);
    std::vector<cf32> out(N, cf32(0, 0));
    std::mt19937 rng(c.seed);
    std::normal_distribution<float> gauss(0.f, 1.f);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double tb = 1.0 / (smartnet::kBaud * (1.0 + c.clockPpm * 1e-6));
    const size_t nFrames = (size_t)(c.durSec / (tb * smartnet::kFrameBits)) + 2;
    // the OSW schedule
    std::deque<Osw> pending;
    std::vector<double> nextUpdate(c.calls.size(), 0.0);
    std::vector<char> granted(c.calls.size(), 0);
    std::vector<uint8_t> bits;
    bits.reserve(nFrames * smartnet::kFrameBits);
    const double ccHz = c.controlHz;
    int period = 0;
    auto oswFor = [&](double t) {
        for (size_t i = 0; i < c.calls.size(); i++) {
            const auto& k = c.calls[i];
            if (!granted[i] && t >= k.startSec - 0.05 && t < k.endSec) {
                granted[i] = 1;
                nextUpdate[i] = t + 0.25;
                for (const Osw& o : smartnetGrantOsws(k)) pending.push_back(o);
            } else if (granted[i] && t < k.endSec && t >= nextUpdate[i]) {
                nextUpdate[i] = t + 0.25;
                Osw u; u.addr = (k.tg & 0xFFF0) | (k.options & 0xF); u.group = true; u.cmd = k.chan;
                pending.push_back(u);
            }
        }
        if (pending.empty()) {
            period++;
            if (period % 24 == 0) {   // the system ID and control channel
                Osw a; a.addr = c.system; a.group = true; a.cmd = 0x308;
                Osw b; b.addr = 0x1F00 | 0x12; b.group = false; b.cmd = c.controlChan;
                pending.push_back(a); pending.push_back(b);
            } else if (period % 24 == 12) {   // the site broadcast
                Osw a; a.addr = c.system; a.group = true; a.cmd = 0x308;
                Osw b; b.addr = ((c.site - 1) << 10) | ((c.siteBand & 7) << 7) | (c.features & 0x3F); b.group = true; b.cmd = 0x320;
                Osw d; d.addr = 0x6000 | c.controlChan; d.group = false; d.cmd = 0x30B;
                pending.push_back(a); pending.push_back(b);
                if (c.idleInsideMessages) { Osw i; i.addr = 0; i.group = false; i.cmd = 0x2F8; pending.push_back(i); }   // a lone idle in the middle of the message
                pending.push_back(d);
            } else if (period % 24 == 6) {
                Osw a; a.addr = (1 << 13) | 0x1000 | 0x25; a.group = false; a.cmd = 0x3C0;   // system status, opcode 1: Type II
                pending.push_back(a);
            } else {
                Osw i; i.addr = 0x0000; i.group = false; i.cmd = 0x2F8;
                pending.push_back(i);
            }
        }
    };
    for (size_t f = 0; f < nFrames; f++) {
        const double t = f * tb * smartnet::kFrameBits;
        if (pending.size() < 3) oswFor(t);
        Osw o = pending.front(); pending.pop_front();
        auto fb = smartnet::frameBits(o);
        if (c.oswErrorRate > 0 && uni(rng) < c.oswErrorRate) { fb[20] ^= 1; fb[33] ^= 1; fb[50] ^= 1; fb[71] ^= 1; }
        bits.insert(bits.end(), fb.begin(), fb.end());
    }
    // the control channel: FM, phase continuous, a small smoothing of the bit edges
    {
        const double omega0 = 2 * M_PI * (ccHz - c.centreHz + c.cfoHz) / c.rate;
        double ph = 0, smooth = 0;
        const double al = 1.0 - std::exp(-2 * M_PI * 3000.0 / c.rate);
        const double sign = c.invert ? -1.0 : 1.0;
        for (size_t i = 0; i < N; i++) {
            const double t = (double)i / c.rate;
            const size_t bi = std::min((size_t)(t / tb), bits.size() - 1);
            const double level = (bits[bi] ? 1.0 : -1.0) * sign * c.deviationHz;
            smooth += al * (level - smooth);
            ph += omega0 + 2 * M_PI * smooth / c.rate;
            if (ph > 2 * M_PI) ph -= 2 * M_PI; else if (ph < -2 * M_PI) ph += 2 * M_PI;
            out[i] += cf32((float)(c.controlAmp * std::cos(ph)), (float)(c.controlAmp * std::sin(ph)));
        }
    }
    // the voice channels
    for (const auto& k : c.calls) {
        if (!k.voiceCarrier) continue;
        const double f = smartnet::channelHz(c.band, c.custom, k.chan);
        if (f <= 0) continue;
        const double omega = 2 * M_PI * (f - c.centreHz + c.cfoHz) / c.rate;
        double ph = 0, toneP = 0;
        const double dt = 2 * M_PI * k.toneHz / c.rate;
        const size_t a = (size_t)(k.startSec * c.rate), b = std::min(N, (size_t)(k.endSec * c.rate));
        for (size_t i = a; i < b; i++) {
            toneP += dt;
            ph += omega + 2 * M_PI * k.devHz * std::sin(toneP) / c.rate;
            if (ph > 2 * M_PI) ph -= 2 * M_PI; else if (ph < -2 * M_PI) ph += 2 * M_PI;
            out[i] += cf32((float)(k.amp * std::cos(ph)), (float)(k.amp * std::sin(ph)));
        }
    }
    for (size_t i = 0; i < N; i++) out[i] += cf32((float)c.noise * gauss(rng), (float)c.noise * gauss(rng));
    return out;
}

} // namespace dect2
