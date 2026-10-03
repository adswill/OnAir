#include "dect2/channel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace dect2 {

const char* multipathName(MultipathLevel l) {
    switch (l) {
    case MultipathLevel::Unknown: return "unknown";
    case MultipathLevel::None: return "none";
    case MultipathLevel::Mild: return "mild";
    case MultipathLevel::Likely: return "likely";
    case MultipathLevel::Severe: return "severe";
    }
    return "";
}

void MultipathDetector::reset() { *this = MultipathDetector(); }

void MultipathDetector::analyseChannel(const RxTelemetry& rx) {
    rep_.echoes.clear();
    rep_.notchDepthDb = 0;
    rep_.notches = 0;
    echoPts_ = notchPts_ = 0;
    chOk_ = rx.chValid && !rx.irDb.empty() && rx.nativeRate > 0;
    if (!chOk_) return;
    const double usPerSample = 1e6 / rx.nativeRate;
    rep_.guardUs = rx.guard * usPerSample;

    // ---- echoes: local maxima of the power-delay profile away from the strongest path
    const auto& ir = rx.irDb;
    const int n = (int)ir.size();
    int main = 0;
    for (int i = 1; i < n; i++) if (ir[i] > ir[main]) main = i;
    const float mainDb = ir[main];
    const int sep = 3; // closer than this is the main lobe itself, not a separate path
    std::vector<EchoInfo> found;
    for (int i = 2; i < n - 2; i++) {
        if (std::abs(i - main) <= sep) continue;
        const float v = ir[i] - mainDb;
        if (v < -22.f) continue;
        if (ir[i] < ir[i - 1] || ir[i] < ir[i + 1] || ir[i] < ir[i - 2] || ir[i] < ir[i + 2]) continue;
        EchoInfo e;
        e.delayUs = (i - main) * usPerSample;
        e.levelDb = v;
        e.insideGuard = e.delayUs >= -rep_.guardUs * 0.0 && e.delayUs <= rep_.guardUs; // post-echoes must fit in the guard interval
        if (e.delayUs < 0) e.insideGuard = -e.delayUs <= rep_.guardUs;
        found.push_back(e);
    }
    std::sort(found.begin(), found.end(), [](const EchoInfo& a, const EchoInfo& b) { return a.levelDb > b.levelDb; });
    if (found.size() > 4) found.resize(4);
    rep_.echoes = found;
    if (!found.empty()) {
        const double L = found[0].levelDb;
        echoPts_ = L > -6 ? 3 : L > -12 ? 2 : 1;
        if (!found[0].insideGuard && echoPts_ < 3) echoPts_++; // an echo beyond the guard interval also causes inter-symbol interference
    }

    // ---- frequency-selective fading: dips in |H(f)|
    const auto& m = rx.chMagDb;
    if (m.size() >= 64) {
        const int w = std::max(3, (int)m.size() / 200); // smooth over about 0.5% of the channel
        std::vector<float> sm(m.size());
        double acc = 0; int cnt = 0;
        for (size_t i = 0; i < m.size(); i++) {
            acc += m[i]; cnt++;
            if (cnt > 2 * w + 1) { acc -= m[i - (2 * w + 1)]; cnt--; }
            sm[i] = (float)(acc / cnt);
        }
        // ignore the channel edges (roll-off)
        const size_t a = m.size() / 20, b = m.size() - m.size() / 20;
        std::vector<float> mid(sm.begin() + a, sm.begin() + b);
        std::vector<float> srt = mid;
        std::sort(srt.begin(), srt.end());
        const float med = srt[srt.size() / 2];
        const float low = srt[std::max<size_t>(0, srt.size() / 50)]; // 2nd percentile
        rep_.notchDepthDb = std::max(0.f, med - low);
        bool inDip = false;
        for (float v : mid) {
            const bool d = v < med - 6.f;
            if (d && !inDip) rep_.notches++;
            inDip = d;
        }
        notchPts_ = rep_.notchDepthDb > 18 ? 3 : rep_.notchDepthDb > 12 ? 2 : rep_.notchDepthDb > 6 ? 1 : 0;
    }
}

void MultipathDetector::update(const RxTelemetry& rx) {
    // per-frame history (new frame = dataFrames advanced)
    if (rx.dataValid && rx.dataFrames != lastFrames_) {
        if (lastFrames_ != ~0ull) {
            const uint64_t ok = rx.blocksOk - lastOk_, bad = rx.blocksBad - lastBad_;
            FrameRec r;
            r.snr = rx.dataSnrDb;
            r.loss = (ok + bad) ? (double)bad / (double)(ok + bad) : 0.0;
            hist_.push_back(r);
            if (hist_.size() > 40) hist_.pop_front();
        }
        lastFrames_ = rx.dataFrames; lastOk_ = rx.blocksOk; lastBad_ = rx.blocksBad;
        analyseChannel(rx);
        finish();
    } else if (!rx.dataValid && rep_.level == MultipathLevel::Unknown && rx.chValid) {
        analyseChannel(rx);
        finish();
    }
}

void MultipathDetector::finish() {
    rep_.framesSeen = (int)hist_.size();
    double mean = 0, var = 0;
    int fades = 0;
    for (auto& r : hist_) { mean += r.snr; if (r.loss > 0.05) fades++; }
    if (!hist_.empty()) {
        mean /= hist_.size();
        for (auto& r : hist_) var += (r.snr - mean) * (r.snr - mean);
        var /= hist_.size();
    }
    rep_.snrStdDb = std::sqrt(var);
    rep_.fadeFrames = fades;
    int timePts = 0;
    if (hist_.size() >= 12) timePts = (fades >= 10 || rep_.snrStdDb > 3.5) ? 3 : (fades >= 4 || rep_.snrStdDb > 2.0) ? 2 : (fades >= 1 || rep_.snrStdDb > 1.2) ? 1 : 0;

    rep_.reasons.clear();
    char b[200];
    if (!chOk_ && hist_.empty()) { rep_.level = MultipathLevel::Unknown; rep_.headline = "waiting for a channel estimate"; return; }
    if (!rep_.echoes.empty()) {
        const EchoInfo& e = rep_.echoes[0];
        snprintf(b, sizeof b, "echo %.0f dB below the main path, %+.2f us away%s", -e.levelDb, e.delayUs, e.insideGuard ? "" : " (outside the guard interval: inter-symbol interference)");
        rep_.reasons.push_back(b);
        if (rep_.echoes.size() > 1) { snprintf(b, sizeof b, "%zu echoes above -22 dB in total", rep_.echoes.size()); rep_.reasons.push_back(b); }
    }
    if (notchPts_ > 0) {
        snprintf(b, sizeof b, "frequency-selective fading: %d dip%s, deepest %.0f dB below the median", rep_.notches, rep_.notches == 1 ? "" : "s", rep_.notchDepthDb);
        rep_.reasons.push_back(b);
    }
    if (timePts > 0) {
        snprintf(b, sizeof b, "signal quality varies over time (SNR std %.1f dB, %d of the last %d frames lost >5%% of their FEC blocks): moving objects or changing reflections", rep_.snrStdDb, fades, (int)hist_.size());
        rep_.reasons.push_back(b);
    }
    const int pts = echoPts_ + notchPts_ + timePts;
    rep_.level = pts == 0 ? MultipathLevel::None : pts <= 1 ? MultipathLevel::Mild : pts <= 6 ? MultipathLevel::Likely : MultipathLevel::Severe;
    if (getenv("DECT2_MPDEBUG")) fprintf(stderr, "[mp] pts echo %d notch %d time %d (fades %d std %.2f depth %.1f)\n", echoPts_, notchPts_, timePts, fades, rep_.snrStdDb, rep_.notchDepthDb);
    switch (rep_.level) {
    case MultipathLevel::None: rep_.headline = "no sign of multipath or fading"; break;
    case MultipathLevel::Mild: rep_.headline = "slight multipath"; break;
    case MultipathLevel::Likely: rep_.headline = "multipath / fading likely"; break;
    case MultipathLevel::Severe: rep_.headline = "strong multipath / fading"; break;
    default: break;
    }
    if (rep_.level >= MultipathLevel::Likely) rep_.reasons.push_back("try moving or re-aiming the antenna (see the direction finder), or use a directional antenna");
}

} // namespace dect2
