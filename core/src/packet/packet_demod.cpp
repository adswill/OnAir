// The FM front end of the packet receivers: see packet_demod.h.
#include "dect2/packet/packet_demod.h"
#include "dect2/dsp/baseband_filter.h"
#include <algorithm>
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace pktdemod {

namespace {

using dect2::math::kPi;
constexpr size_t kNoiseBlocks = 250;           // the noise floor is a low percentile of the last 5 s

int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kMidRate));
    const int lo = std::max(1, (int)std::floor(fs / 130000.0));
    auto largestPrime = [](int v) {
        int lp = 1;
        for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; }
        return lp;
    };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; bestPrime = lp; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) {
        if (d % p == 0) { primes.push_back(p); d /= p; }
        else p++;
    }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) {
        if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p;
        else stages.push_back(p);
    }
    return stages;
}

} // namespace

bool FmFront::configure(double rate, double minRate) {
    fs = rate;
    ready = false;
    stages.clear();
    if (fs < minRate - 1) return false;
    const int d = pickDecimation(fs);
    const std::vector<int> f = splitFactors(d);
    double r = fs;
    stages.assign(f.size(), dmr::Fir<cf32>());
    for (size_t i = 0; i < f.size(); i++) {
        const double out = r / f[i];
        const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
        stages[i].design(dmr::lowpassTaps(pass, std::min(stop, r * 0.5), r, 60, 9), f[i]);
        r = out;
    }
    rsPass = std::fabs(r - kMidRate) < 0.01;
    if (!rsPass && !rs.configure(r, kMidRate)) return false;
    chan.design(bbf::overrideTaps("packet", dmr::lowpassTaps(chanPass, chanStop, kMidRate, 60, 9), kMidRate, chanPass + chanStop), 1);
    if (chanPass2 > 0) chan2.design(dmr::lowpassTaps(chanPass2, chanStop2, kMidRate, 60, 9), 1);
    if (chanPass3 > 0) chan3.design(dmr::lowpassTaps(chanPass3, chanStop3, kMidRate, 60, 9), 1);
    ready = true;
    reset();
    return true;
}

void FmFront::reset() {
    for (auto& s : stages) s.reset();
    if (!rsPass) rs.reset();
    chan.reset(); chan2.reset(); prevY2 = cf32(1.f, 0.f); chan3.reset(); prevY3 = cf32(1.f, 0.f);
    offPhase = 0; prevY = cf32(1.f, 0.f);
    blkSum = blkDisc = 0; blkN = 0; blocks.clear(); nf = 0; nfKnown = false; carrier = false;
    lastCarrier = -(int64_t)1e12; carrierSince = 0; snrEma = cfoEma = 0; haveSnr = false; afcHz = afcEst = afcPhase = lastBlkCfo = 0;
    inPower = 0; inN = 0;
    n96 = 0; pending = 0;
}

void FmFront::blockEnd() {
    const double mean = blkSum / (double)blkN;
    blocks.push_back(mean);
    if (blocks.size() > kNoiseBlocks) blocks.pop_front();
    if (blocks.size() >= 3) {
        std::vector<double> s(blocks.begin(), blocks.end());
        std::sort(s.begin(), s.end());
        nf = std::max(s[s.size() / 10] * 1.05, 1e-12);
        nfKnown = true;
    }
    const bool was = carrier;
    carrier = nfKnown && mean > 2.0 * nf;
    if (carrier && !was) carrierSince = n96 - (int64_t)kBlock;
    if (carrier) {
        lastCarrier = n96;
        const double snr = 10 * std::log10((mean - nf) / nf);
        snrEma = haveSnr ? 0.9 * snrEma + 0.1 * snr : snr;
        const double cfo = blkDisc / (double)blkN;
        cfoEma = haveSnr ? 0.9 * cfoEma + 0.1 * cfo : cfo;
        // fast while far off (two blocks in a row agree), slowly once close, so FM clicks of a weak signal do not move it. Only an error
        // of more than 1.5 kHz is mixed out (released below 1 kHz): a small one does no harm to the demodulators
        const bool farBy = std::fabs(cfo) > 1500 && std::fabs(lastBlkCfo) > 1500 && cfo * lastBlkCfo > 0;
        afcEst = std::max(-afcMaxHz, std::min(afcMaxHz, afcEst + (farBy ? 0.6 : 0.03) * (afcHz + cfo - afcEst)));
        if (std::fabs(afcEst) > 1500) afcHz = afcEst; else if (std::fabs(afcEst) < 1000) afcHz = 0;
        lastBlkCfo = cfo;
        haveSnr = true;
    }
    blkSum = blkDisc = 0; blkN = 0;
}

const std::vector<float>& FmFront::process(const cf32* x, size_t n) {
    disc.clear();
    pending = 0;
    if (!ready || n == 0) return disc;
    bool bad = false;
    for (size_t i = 0; i < n; i++) {
        const float re = x[i].real(), im = x[i].imag();
        if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
    }
    if (bad || afInput) {
        clean.assign(x, x + n);
        for (auto& v : clean) {
            if (!(std::fabs(v.real()) < 1e4f && std::fabs(v.imag()) < 1e4f)) v = cf32(0, 0);
            if (afInput) v = cf32(v.real(), 0.f);
        }
        x = clean.data();
    }
    for (size_t i = 0; i < n; i++) inPower += (double)std::norm(x[i]);
    inN += (int64_t)n;
    const cf32* cur = x;
    size_t cn = n;
    if (offsetHz != 0 && !afInput) {
        mixBuf.resize(n);
        const double w = -2 * kPi * offsetHz / fs;
        const cf32 stp((float)std::cos(w), (float)std::sin(w));
        cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
        for (size_t i = 0; i < n; i++) {
            mixBuf[i] = x[i] * ph;
            ph *= stp;
            if ((i & 1023) == 1023) ph /= std::abs(ph);
        }
        offPhase = std::fmod(offPhase + w * (double)n, 2 * kPi);
        cur = mixBuf.data();
    }
    for (size_t s = 0; s < stages.size(); s++) {
        std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
        dst.clear();
        stages[s].process(cur, cn, dst);
        cur = dst.data(); cn = dst.size();
    }
    if (!rsPass) {
        o2.clear();
        rs.process(cur, cn, o2);
        cur = o2.data(); cn = o2.size();
    }
    if (afcHz != 0 && !afInput) {
        afcBuf.resize(cn);
        const double w = -2 * kPi * afcHz / kMidRate;
        for (size_t i = 0; i < cn; i++) {
            afcBuf[i] = cur[i] * cf32((float)std::cos(afcPhase), (float)std::sin(afcPhase));
            afcPhase += w;
            if (afcPhase > kPi) afcPhase -= 2 * kPi; else if (afcPhase < -kPi) afcPhase += 2 * kPi;
        }
        cur = afcBuf.data();
    }
    ch.clear();
    chan.process(cur, cn, ch);
    disc.resize(ch.size());
    const float k = (float)(kMidRate / (2 * kPi));
    const float wide = (float)std::max(14000.0, afcMaxHz + 2000.0);
    for (size_t i = 0; i < ch.size(); i++) {
        const cf32 y = ch[i];
        const cf32 z = y * std::conj(prevY);
        float d = afInput ? y.real() * 4000.f : (z.real() == 0.f && z.imag() == 0.f) ? 0.f : std::atan2(z.imag(), z.real()) * k;
        blkDisc += std::max(-wide, std::min(wide, d));   // the carrier error from a wider clip: clipHz would hide most of a large one
        d = std::max(-clipHz, std::min(clipHz, d));
        prevY = y;
        disc[i] = d;
        blkSum += (double)std::norm(y);
        if (++blkN == kBlock) blockEnd();
    }
    if (chanPass2 > 0) {
        ch2.clear();
        chan2.process(cur, cn, ch2);
        disc2.resize(ch2.size());
        for (size_t i = 0; i < ch2.size(); i++) {
            const cf32 y = ch2[i];
            const cf32 z = y * std::conj(prevY2);
            float d = afInput ? y.real() * 4000.f : (z.real() == 0.f && z.imag() == 0.f) ? 0.f : std::atan2(z.imag(), z.real()) * k;
            disc2[i] = std::max(-clipHz, std::min(clipHz, d));
            prevY2 = y;
        }
    }
    if (chanPass3 > 0) {
        ch3.clear();
        chan3.process(cur, cn, ch3);
        disc3.resize(ch3.size());
        for (size_t i = 0; i < ch3.size(); i++) {
            const cf32 y = ch3[i];
            const cf32 z = y * std::conj(prevY3);
            float d = afInput ? y.real() * 4000.f : (z.real() == 0.f && z.imag() == 0.f) ? 0.f : std::atan2(z.imag(), z.real()) * k;
            disc3[i] = std::max(-clipHz, std::min(clipHz, d));
            prevY3 = y;
        }
    }
    pending = ch.size();
    return disc;
}

} // namespace pktdemod
} // namespace dect2
