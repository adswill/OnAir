// RTTY auto-lock (hfdig_rtty.h, rttyAutoAnalyse): the two spectral lines give the pair of tones and the shift, then the decoder itself is run over
// the audio for each speed and both polarities, and the guess that gives the best text wins; the others are kept as runner-ups.
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/autoparam/autoparam.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

namespace {

constexpr double kFs = kHfdigAudioRate;
// score of the string of text: letters, digits, spaces and the usual marks count, controls a little, the rest half
float textScore(const std::string& t) {
    float g = 0;
    for (char c : t) {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') g += 1.0f;
        else if (c == '\r' || c == '\n') g += 0.3f;
        else g += 0.5f;
    }
    return g;
}

}   // namespace

std::vector<RttyAutoHyp> rttyAutoAnalyse(const float* audio, size_t n, int maxHyps) {
    std::vector<RttyAutoHyp> res;
    if (n < (size_t)(3 * kFs)) return res;
    // the pairs of tones (autoparam.h); with them the keying gives the speed, so only the likely ones go through the decoder
    const auto pairs = autoparam::findFskPairs(audio, n, kFs, kRttyShiftTable, kRttyShifts, 4, 10.0);
    if (pairs.empty()) return res;
    struct Pick { autoparam::FskPair p; int bauds[2]; int nb; };
    std::vector<Pick> pick;
    for (const auto& p : pairs) {
        Pick k; k.p = p; k.nb = 0;
        const auto fb = autoparam::measureFskBaud(audio, n, kFs, p.lowHz, p.shiftHz, kRttyBaudTable, kRttyBauds);
        if (fb.best >= 0 && fb.confidence >= 0.2) { k.bauds[k.nb++] = fb.best; if (fb.second >= 0) k.bauds[k.nb++] = fb.second; }
        pick.push_back(k);
    }
    // the decoder over the audio for each guess; the last 20 s of it at most
    const size_t take = std::min(n, (size_t)(20 * kFs));
    const float* x = audio + (n - take);
    std::vector<RttyAutoHyp> all;
    for (const auto& k : pick) {
        const auto& p = k.p;
        const int nb = k.nb ? k.nb : kRttyBauds;
        for (int bq = 0; bq < nb; bq++)
            for (int rv = 0; rv < 2; rv++) {
                const int bi = k.nb ? k.bauds[bq] : bq;
                auto d = makeRttyDecoder();
                d->setBaudIndex(bi); d->setShiftIndex(p.shiftIdx); d->setReverse(rv != 0); d->setFixedPair(p.lowHz);
                d->reset();
                for (size_t i = 0; i < take; i += 4096) d->feedAudio(x + i, std::min<size_t>(4096, take - i));
                HfdigRttyTelemetry t;
                d->telemetry(t);
                RttyAutoHyp h;
                h.lowHz = p.lowHz; h.shiftIdx = p.shiftIdx; h.shiftHz = kRttyShiftTable[p.shiftIdx]; h.baudIdx = bi; h.baud = kRttyBaudTable[bi];
                h.reverse = rv != 0; h.chars = (uint32_t)t.chars; h.errors = (uint32_t)t.framingErrors;
                h.score = textScore(t.text) - 2.5f * (float)t.framingErrors;
                all.push_back(h);
            }
    }
    std::sort(all.begin(), all.end(), [](const RttyAutoHyp& a, const RttyAutoHyp& b) { return a.score > b.score; });
    for (const auto& h : all) {
        if ((int)res.size() >= maxHyps) break;
        if (h.score < 8.0f || h.chars < 8) break;
        res.push_back(h);
    }
    return res;
}

}   // namespace dect2
