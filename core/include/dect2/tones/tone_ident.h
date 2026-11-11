// Tone system identification (4.0): given the tones heard (frequency and length, in the order they came) on a voice channel, say which signalling
// system it is: DTMF (two simultaneous tones, one from each group), a five-tone sequence (ZVEI1 / ZVEI2 / CCIR / EEA / EIA / DZVEI ... by the tone
// plan), a CTCSS sub-tone (a steady tone below 300 Hz) or SELCAL (two pairs from the 16-tone plan). The user does not have to know what a channel uses
// before opening the decoder. Needs the tables of tones_proto.h (dect2core).
#pragma once
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dect2 {

struct HeardTone { double hz = 0; double lenMs = 0; double startMs = 0; };   // two simultaneous tones are two entries with the same startMs
struct ToneSystemGuess { std::string system, detail; double score = 0; };

inline std::vector<ToneSystemGuess> identifyToneSystem(const std::vector<HeardTone>& t, double tolHz = 8.0) {
    std::vector<ToneSystemGuess> v;
    if (t.empty()) return v;
    using namespace tones;
    // CTCSS: one steady tone below 300 Hz, long
    {
        double best = 0; int idx = -1;
        for (const auto& x : t) if (x.hz < 300 && x.lenMs >= 500) { const int i = ctcssIndex(x.hz); if (i >= 0 && x.lenMs > best) { best = x.lenMs; idx = i; } }
        if (idx >= 0) { char b[64]; std::snprintf(b, sizeof b, "CTCSS %.1f Hz", ctcssHz(idx)); v.push_back({"CTCSS", b, 0.95}); }
    }
    // DTMF: pairs that start together, one row and one column tone
    {
        int pairs = 0, total = 0;
        for (size_t i = 0; i < t.size(); i++) for (size_t j = i + 1; j < t.size(); j++) {
            if (std::fabs(t[i].startMs - t[j].startMs) > 30) continue;
            total++;
            int r = -1, c = -1;
            for (int k = 0; k < 4; k++) { if (std::fabs(t[i].hz - kDtmfRows[k]) < t[i].hz * 0.025) r = k; if (std::fabs(t[j].hz - kDtmfCols[k]) < t[j].hz * 0.025) c = k; }
            if (r < 0 || c < 0) { r = c = -1; for (int k = 0; k < 4; k++) { if (std::fabs(t[j].hz - kDtmfRows[k]) < t[j].hz * 0.025) r = k; if (std::fabs(t[i].hz - kDtmfCols[k]) < t[i].hz * 0.025) c = k; } }
            if (r >= 0 && c >= 0) pairs++;
        }
        if (pairs > 0) v.push_back({"DTMF", std::to_string(pairs) + " digit(s)", std::min(1.0, 0.6 + 0.1 * pairs)});
        (void)total;
    }
    // five-tone sequential: every serial tone matches a set, the set with the most matches and the tone length nearest the usual one wins
    {
        std::vector<HeardTone> s;
        for (const auto& x : t) if (x.hz >= 300 && x.hz <= 3000) s.push_back(x);
        std::sort(s.begin(), s.end(), [](const HeardTone& a, const HeardTone& b) { return a.startMs < b.startMs; });
        // no two at the same time
        bool serial = !s.empty();
        for (size_t i = 1; i < s.size(); i++) if (std::fabs(s[i].startMs - s[i - 1].startMs) < 30) serial = false;
        if (serial && s.size() >= 3) {
            for (int k = 0; k < kSeqSetN; k++) {
                int hit = 0; double lenErr = 0;
                for (const auto& x : s) {
                    bool m = std::fabs(x.hz - kSeqSets[k].rep) <= tolHz;
                    for (int d = 0; d < 10 && !m; d++) m = std::fabs(x.hz - kSeqSets[k].digit[d]) <= tolHz;
                    if (m) { hit++; lenErr += std::fabs(x.lenMs - kSeqSets[k].toneMs) / std::max(1.0, kSeqSets[k].toneMs); }
                }
                if (hit < (int)s.size()) continue;   // every tone must belong to the plan
                const double sc = 0.7 + 0.25 * std::max(0.0, 1.0 - lenErr / s.size());
                v.push_back({kSeqSets[k].id, std::string(kSeqSets[k].name) + ", " + std::to_string(s.size()) + " tones", sc});
            }
        }
        // SELCAL: four tones in two pairs 1 s long (a pair is simultaneous)
        int sel = 0;
        for (const auto& x : t) { bool m = false; for (int i = 0; i < kSelcalN; i++) if (std::fabs(x.hz - selcalHz(i)) <= 3.0) m = true; if (m && x.lenMs >= 700) sel++; }
        if (sel == 4) v.push_back({"SELCAL", "four tones of the SELCAL plan", 0.9});
    }
    std::stable_sort(v.begin(), v.end(), [](const ToneSystemGuess& a, const ToneSystemGuess& b) { return a.score > b.score; });
    return v;
}

} // namespace dect2
