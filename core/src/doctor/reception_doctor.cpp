// Reception Doctor analysis (docs/RECEPTION-DOCTOR.md). One averaged spectrum (Hann, 4096 bins, 50 % overlap) plus time-domain level statistics.
#include "dect2/doctor/reception_doctor.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/sig_ident.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::doctor {
namespace {

constexpr int kN = 4096;

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[320];
    snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

double db(double p) { return 10 * std::log10(std::max(p, 1e-20)); }

struct Spec {
    std::vector<double> p;    // power per bin (linear, full-scale-relative), DC-centred: bin k is (k - kN/2) * rate / kN
    int segs = 0;
};

Spec welch(const cf32* x, size_t n) {
    Spec s;
    s.p.assign(kN, 0.0);
    static const std::vector<float> w = [] { std::vector<float> v(kN); for (int i = 0; i < kN; i++) v[i] = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / kN); return v; }();
    double wp = 0;
    for (float v : w) wp += (double)v * v;
    Fft fft(kN);
    std::vector<cf32> buf(kN);
    const size_t maxSegs = 64;
    size_t hop = kN / 2, total = n >= (size_t)kN ? (n - kN) / hop + 1 : 0;
    size_t step = total > maxSegs ? total / maxSegs : 1;
    for (size_t sI = 0; sI < total; sI += step) {
        for (int i = 0; i < kN; i++) buf[i] = x[sI * hop + i] * w[i];
        fft.forward(buf.data());
        for (int i = 0; i < kN; i++) s.p[(i + kN / 2) % kN] += std::norm(buf[i]) / (wp * kN);
        s.segs++;
    }
    if (s.segs) for (auto& v : s.p) v /= s.segs;
    return s;
}

} // namespace

Report diagnose(const cf32* x, size_t n, const Context& c) {
    Report r;
    if (!x || n < (size_t)kN * 4 || c.rateHz <= 0) return r;
    r.valid = true;
    Measure& m = r.m;

    // ---- time domain: level, peak, clipping (a sample with I or Q within 2 % of full scale)
    double sum = 0, pk = 0;
    size_t clip = 0;
    for (size_t i = 0; i < n; i++) {
        const float a = std::fabs(x[i].real()), b = std::fabs(x[i].imag());
        sum += (double)a * a + (double)b * b;
        pk = std::max<double>(pk, std::max(a, b));
        if (a >= 0.98f || b >= 0.98f) clip++;
    }
    m.rmsDbfs = db(sum / (double)n);
    m.peak = pk;
    m.clipPct = 100.0 * (double)clip / (double)n;

    // ---- spectrum
    Spec sp = welch(x, n);
    const double binHz = c.rateHz / kN;
    std::vector<double> d(kN);
    for (int i = 0; i < kN; i++) d[i] = db(sp.p[i]);
    std::vector<double> sorted(d);
    std::sort(sorted.begin(), sorted.end());
    const double floorDb = sorted[kN / 2];                       // median bin: the noise when the capture is wider than the signals
    m.floorDbfs = floorDb;
    // 9-bin smoothing to find signals
    std::vector<double> sm(kN, 0);
    {
        double acc = 0;
        for (int i = 0; i < kN; i++) { acc += sp.p[i]; if (i >= 9) acc -= sp.p[i - 9]; sm[i] = db(acc / std::min(i + 1, 9)); }
        std::vector<double> t(kN, -200);
        for (int i = 4; i < kN; i++) t[i - 4] = sm[i];
        sm = t;
    }
    // strongest smoothed bin (ignore the 5 bins around DC: the spike is judged separately)
    int pkBin = -1;
    double pkDb = -200;
    for (int i = 0; i < kN; i++) {
        if (std::abs(i - kN / 2) <= 5) continue;
        if (sm[i] > pkDb) { pkDb = sm[i]; pkBin = i; }
    }
    m.strongestOverFloorDb = pkDb - floorDb;
    const bool haveSignal = pkBin >= 0 && m.strongestOverFloorDb > 8;
    double sigLo = 0, sigHi = 0;
    if (haveSignal) {
        const double thr = floorDb + std::max(3.0, m.strongestOverFloorDb - 10);   // -10 dB of the peak, at least 3 dB over the floor
        int lo = pkBin, hi = pkBin;
        int gap = 0;
        for (int i = pkBin; i >= 0; i--) { if (sm[i] >= thr) { lo = i; gap = 0; } else if (++gap > 6) break; }
        gap = 0;
        for (int i = pkBin; i < kN; i++) { if (sm[i] >= thr) { hi = i; gap = 0; } else if (++gap > 6) break; }
        sigLo = (lo - kN / 2) * binHz; sigHi = (hi - kN / 2) * binHz;
        m.strongestHz = 0.5 * (sigLo + sigHi);
        m.strongestBwHz = sigHi - sigLo + binHz;
    }

    auto push = [&](Finding f) { r.findings.push_back(std::move(f)); };

    // ---- 1. overload: clipping, or a peak at full scale
    const bool clipping = m.clipPct > 0.1 || pk >= 0.995;
    if (clipping && (c.live || c.gainMaxDb > 0)) {
        Finding f; f.code = Code::Clipping; f.severity = SevBad; f.a = m.clipPct;
        f.text = fmt("The radio's converter is overloaded: %.2f %% of the samples are at full scale. Lower the gain.", m.clipPct);
        f.fix = Fix::LowerGain; f.value = m.clipPct > 5 ? 20 : m.clipPct > 1 ? 12 : 6;
        push(f);
    } else if (clipping) {
        Finding f; f.code = Code::Clipping; f.severity = SevWarn; f.a = m.clipPct;
        f.text = fmt("The recording itself is clipped (%.2f %% of the samples at full scale); the decoder will see distortion.", m.clipPct);
        push(f);
    }

    // ---- 2. too little signal: the level is near the converter's own noise, or the spectrum has nothing standing out
    if (!clipping && !c.locked) {
        const bool lowLevel = m.rmsDbfs < -48;
        if (lowLevel && c.live) {
            Finding f; f.code = Code::WeakSignal; f.severity = SevWarn; f.a = m.rmsDbfs;
            f.text = c.gainMaxDb > 0 && c.gainDb >= c.gainMaxDb - 1
                ? fmt("The level is only %.0f dBFS with the gain already at maximum: check the antenna and cable, or add an LNA at the antenna.", m.rmsDbfs)
                : fmt("The level is only %.0f dBFS, close to the converter's own noise: raise the gain.", m.rmsDbfs);
            if (c.gainMaxDb > 0 && c.gainDb < c.gainMaxDb - 1) { f.fix = Fix::RaiseGain; f.value = std::min(c.gainMaxDb - c.gainDb, std::min(20.0, -30.0 - m.rmsDbfs)); }
            push(f);
        }
        if (!haveSignal) {
            // flat spectrum: the strongest smoothed bin is within 8 dB of the median
            Finding f; f.code = Code::NoSignal; f.severity = SevWarn; f.a = m.strongestOverFloorDb;
            f.text = fmt("Nothing stands out of the noise (strongest %.0f dB over the floor): the spectrum is flat. No antenna, a wrong frequency or a dead band.", m.strongestOverFloorDb);
            push(f);
        }
    }

    // ---- 3. the mode's own channel: width, offset, interferers
    if (c.modeBwHz > 0) {
        if (c.modeBwHz > 0.85 * c.rateHz) {
            Finding f; f.code = Code::RateTooNarrow; f.severity = SevBad; f.a = c.modeBwHz; f.b = c.rateHz;
            f.text = fmt("The mode needs about %.0f kHz of bandwidth but the sample rate is only %.0f kHz: widen the sample rate.", c.modeBwHz / 1e3, c.rateHz / 1e3);
            f.fix = Fix::WidenRate; f.value = std::ceil(c.modeBwHz * 1.25 / 1e5) * 1e5;
            push(f);
        }
        if (haveSignal && !c.locked) {
            // a wanted-looking signal: its width is within a factor 3 of the mode's; off-centre when its centre is far from the expected place
            const double ratio = m.strongestBwHz / c.modeBwHz;
            const double delta = m.strongestHz - c.modeOffsetHz;
            if (ratio > 0.33 && ratio < 3.0 && std::fabs(delta) > std::max(0.1 * c.modeBwHz, 3 * binHz)) {
                Finding f; f.code = Code::OffCentre; f.severity = SevWarn; f.a = delta; f.b = m.strongestBwHz;
                f.text = fmt("A signal %.0f kHz wide sits %+.1f kHz from where this mode listens: move the capture onto it.", m.strongestBwHz / 1e3, delta / 1e3);
                f.fix = Fix::ApplyOffset; f.value = delta;
                push(f);
            }
        }
        // strong adjacent interferer: the strongest bin outside the channel (with a skirt of one channel width) against the strongest inside
        {
            const double lo = c.modeOffsetHz - c.modeBwHz * 0.75, hi = c.modeOffsetHz + c.modeBwHz * 0.75;
            double inPk = -200, outPk = -200, outHz = 0;
            for (int i = 0; i < kN; i++) {
                if (std::abs(i - kN / 2) <= 5) continue;
                const double hz = (i - kN / 2) * binHz;
                if (hz >= c.modeOffsetHz - c.modeBwHz / 2 && hz <= c.modeOffsetHz + c.modeBwHz / 2) inPk = std::max(inPk, sm[i]);
                else if (hz < lo || hz > hi) { if (sm[i] > outPk) { outPk = sm[i]; outHz = hz; } }
            }
            if (outPk > floorDb + 25 && outPk > inPk + 15 && c.modeBwHz < 0.85 * c.rateHz) {
                Finding f; f.code = Code::Interferer; f.severity = clipping ? SevBad : SevWarn; f.a = outPk - std::max(inPk, floorDb); f.b = outHz;
                f.text = fmt("A strong signal %+.0f kHz away is %.0f dB above the wanted channel and can swamp the receiver: lower the gain or add a filter.", (outHz - c.modeOffsetHz) / 1e3, f.a);
                if (c.live && c.gainMaxDb > 0 && c.gainDb > 0) { f.fix = Fix::LowerGain; f.value = 10; }
                if (!clipping && !c.locked) push(f);
            }
        }
    }

    // ---- 4. drops
    if (c.live) {
        if (c.radioDrops || (c.deliveredPct >= 0 && c.deliveredPct < 97)) {
            Finding f; f.code = Code::UsbDrops; f.severity = SevBad; f.a = c.deliveredPct;
            f.text = c.deliveredPct >= 0
                ? fmt("The radio delivers only %.0f %% of its sample rate (USB or driver losses): use another port or cable, no hub, or a lower sample rate.", c.deliveredPct)
                : std::string("The radio is losing samples (USB or driver): use another port or cable, no hub, or a lower sample rate.");
            push(f);
        }
        if (c.cpuDrops) {
            Finding f; f.code = Code::CpuDrops; f.severity = SevBad;
            f.text = "OnAir cannot keep up with the sample rate and drops samples: close other programs or lower the sample rate.";
            push(f);
        }
    }

    // ---- 5. DC spike and I/Q imbalance (live radios; a recording may have them too but there is nothing to fix on the radio)
    {
        double nb = 0;
        int cnt = 0;
        for (int k = 20; k <= 60; k++) { nb += sp.p[kN / 2 + k] + sp.p[kN / 2 - k]; cnt += 2; }
        const double ref = db(nb / cnt);
        const double dc = db(sp.p[kN / 2] + sp.p[kN / 2 - 1] + sp.p[kN / 2 + 1]) - 4.77 - ref;   // three bins summed: remove the 4.8 dB of the sum
        m.dcOverDb = dc;
        const bool inChan = c.modeBwHz > 0 && std::fabs(c.modeOffsetHz) < c.modeBwHz / 2 + 2 * binHz;
        if (dc > 15 && c.live && !c.locked) {
            Finding f; f.code = Code::DcSpike; f.severity = inChan ? SevWarn : SevInfo; f.a = dc;
            f.text = inChan
                ? fmt("A DC spike %.0f dB high sits on the middle of the channel: tune the radio a little off the signal (offset tuning) or switch on Remove DC spike.", dc)
                : fmt("There is a DC spike %.0f dB above its surroundings; it is outside the wanted channel, so it should not matter.", dc);
            push(f);
        }
    }
    if (pkBin >= 0 && haveSignal && m.strongestBwHz < 40 * binHz && !clipping) {
        // narrow tone: compare with its mirror
        const int mir = kN - pkBin;
        double img = -200;
        for (int i = std::max(0, mir - 2); i <= std::min(kN - 1, mir + 2); i++) img = std::max(img, d[i]);
        double tone = -200;
        for (int i = std::max(0, pkBin - 2); i <= std::min(kN - 1, pkBin + 2); i++) tone = std::max(tone, d[i]);
        m.imageRejDb = tone - img;
        // the mirror of a tone must not itself be a signal: it is a mirror only when the mirror bin is a narrow peak too and the tone is clean otherwise
        double surround = -200;
        for (int i = std::max(0, mir - 30); i <= std::min(kN - 1, mir + 30); i++) if (std::abs(i - mir) > 3) surround = std::max(surround, d[i]);
        if (m.imageRejDb < 30 && m.imageRejDb > 0 && img > surround + 6 && tone - floorDb > 30 && c.live && !c.locked) {
            Finding f; f.code = Code::IqImbalance; f.severity = SevWarn; f.a = m.imageRejDb;
            f.text = fmt("A mirror image only %.0f dB below the strongest tone: the radio's I/Q branches are unbalanced. Switch on IQ correction in the radio settings.", m.imageRejDb);
            push(f);
        }
    }

    // ---- 6. wrong mode: only when nothing above explains it and the decoder is not locked
    if (c.identify && !c.locked && haveSignal && r.findings.empty()) {
        sigid::Options o;
        o.rfHz = c.rfHz; o.loHz = sigLo; o.hiHz = sigHi; o.verify = true;
        const sigid::Result id = sigid::identify(x, std::min<size_t>(n, 1u << 20), c.rateHz, o);
        if (id.verdict == sigid::VerdictLikely && !id.cands.empty()) {
            const sigid::Candidate& k = id.cands[0];
            if (!k.modeId.empty() && k.modeId != c.modeId) {
                Finding f; f.code = Code::WrongMode; f.severity = SevWarn; f.a = k.confidence * 100;
                f.text = fmt("The signal on the spectrum looks like ", 0) + k.name + fmt(" (%.0f %% sure), not this mode: switch mode.", k.confidence * 100);
                f.fix = Fix::SwitchMode; f.modeId = k.modeId; f.subId = k.subId;
                push(f);
            }
        }
    }

    if (r.findings.empty()) {
        Finding f; f.code = Code::LooksFine; f.severity = SevInfo;
        f.text = c.locked ? "The decoder is locked and the front end looks healthy." : "The front end looks healthy: no overload, a usable level, the signal in place.";
        r.findings.push_back(f);
    }
    std::stable_sort(r.findings.begin(), r.findings.end(), [](const Finding& a, const Finding& b) { return a.severity > b.severity; });
    return r;
}

} // namespace dect2::doctor
