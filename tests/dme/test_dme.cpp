// DME receiver against ICAO Annex 10 DME/N timing: shaped 3.5 us pulses, X pairs 12 us apart and Y reply pairs 30 us apart (36 us for an aircraft interrogation), random
// replies and squitter, the ident as a regular 1350 pairs/s train keyed in Morse (dot 0.1 - 0.16 s, dash three dots, gaps one and three dots) in between random
// traffic, noise, other pulses, several sample rates, and the channel plan. Generated signal only: no 1 GHz recording was available.
#include "dect2/dme/dme_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Pulse { double t; float a; };

// Envelope generator: each pulse is a raised-cosine-edged pulse of 3.5 us at half amplitude (rise 2.5 us, fall 2.5 us)
struct Gen {
    double fs;
    std::vector<Pulse> pulses;   // start times (50% point), sorted before use
    std::vector<float> render(double durSec, float noise, unsigned seed) {
        std::sort(pulses.begin(), pulses.end(), [](const Pulse& a, const Pulse& b) { return a.t < b.t; });
        const size_t n = (size_t)(durSec * fs);
        std::vector<float> env(n, 0.f);
        const double rise = 2.5e-6, width = 3.5e-6;
        for (const Pulse& p : pulses) {
            const double t0 = p.t - rise / 2, t1 = p.t + width + rise / 2;
            const size_t a = (size_t)std::max(0.0, std::floor(t0 * fs)), b = std::min(n, (size_t)std::ceil(t1 * fs));
            for (size_t i = a; i < b; i++) {
                const double t = (double)i / fs;
                double v;
                if (t < p.t - rise / 2) v = 0;
                else if (t < p.t + rise / 2) v = 0.5 - 0.5 * std::cos(M_PI * (t - (p.t - rise / 2)) / rise);
                else if (t < p.t + width - rise / 2) v = 1;
                else if (t < p.t + width + rise / 2) v = 0.5 + 0.5 * std::cos(M_PI * (t - (p.t + width - rise / 2)) / rise);
                else v = 0;
                env[i] = std::max(env[i], (float)v * p.a);
            }
        }
        if (noise > 0) {   // |IQ| of a noisy carrier: Rayleigh-ish; the floor and the pulses add in power
            std::mt19937 g(seed);
            std::normal_distribution<float> nd(0.f, noise);
            for (size_t i = 0; i < n; i++) { const float re = env[i] + nd(g), im = nd(g); env[i] = std::sqrt(re * re + im * im); }
        }
        return env;
    }
    void pair(double t, double spacingUs, float amp = 1.f) { pulses.push_back({t, amp}); pulses.push_back({t + spacingUs * 1e-6, amp}); }
};

// random replies and squitter: Poisson pairs at `rate` per second with the amplitude varying like different aircraft
void randomPairs(Gen& g, double t0, double t1, double spacingUs, double rate, unsigned seed) {
    std::mt19937 r(seed);
    std::exponential_distribution<double> ex(rate);
    std::uniform_real_distribution<float> ua(0.6f, 1.0f);
    for (double t = t0 + ex(r); t < t1; t += std::max(ex(r), 70e-6)) g.pair(t, spacingUs, ua(r));
}

const char* kMorse(char c) {
    static const std::map<char, const char*> m = {{'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'F', "..-."}, {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"},
        {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"},
        {'W', ".--"}, {'X', "-..-"}, {'Y', "-.--"}, {'Z', "--.."}};
    return m.at(c);
}

// The ident: key-down = pairs at exactly 1/1350 s (with a little jitter), dot `dot` seconds, dash 3 dots, one dot between elements, three between letters.
// Between the key-downs the beacon replies as usual (random pairs at `rate`).
double addIdent(Gen& g, double t0, const std::string& ident, double dot, double spacingUs, double rate, unsigned seed, double jitterUs = 2.0) {
    std::mt19937 r(seed);
    std::normal_distribution<double> jit(0.0, jitterUs * 1e-6);
    double t = t0;
    for (size_t li = 0; li < ident.size(); li++) {
        const std::string code = kMorse(ident[li]);
        for (size_t j = 0; j < code.size(); j++) {
            const double d = code[j] == '-' ? 3 * dot : dot;
            for (double k = 0;; k++) { const double ts = t + k / dme::kIdentRatePps; if (ts >= t + d) break; g.pair(ts + jit(r), spacingUs); }
            t += d;
            const double gap = (j + 1 < code.size()) ? dot : (li + 1 < ident.size() ? 3 * dot : 0);
            if (gap > 0) { randomPairs(g, t, t + gap, spacingUs, rate, seed + (unsigned)(t * 1000)); t += gap; }
        }
    }
    return t;
}

void feedAll(dme::Receiver& rx, const std::vector<float>& env) {
    for (size_t i = 0; i < env.size(); i += 50000) rx.feed(env.data() + i, std::min<size_t>(50000, env.size() - i));
}

} // namespace

int main() {
    CHECK(dme::morseLetter("---") == 'O' && dme::morseLetter(".-.") == 'R' && dme::morseLetter("-..") == 'D' && dme::morseLetter("-.-.-.-") == '?', "Morse table");

    // ---- the channel plan: 252 channels, every reply frequency once, reply = interrogation +- 63 MHz
    {
        std::set<long> replies;
        int bad = 0;
        for (int y = 0; y < 2; y++)
            for (int n = 1; n <= 126; n++) {
                // the reply frequency from the rule: X 1-16 and 60-69: int-63; X 17-59 and 70-126: int+63; Y the opposite
                const bool low = (n <= 16 || (n >= 60 && n <= 69));
                const double reply = dme::interrogationMHz(n) + ((low != (y == 1)) ? -63 : 63);
                bool isY = false;
                const int back = dme::channelFromReplyMHz(reply, isY);
                if (back != n || isY != (y == 1)) bad++;
                replies.insert(std::lround(reply));
            }
        CHECK(bad == 0, "channel plan round trip: %d channels differ", bad);
        CHECK(replies.size() == 252 && *replies.begin() == 962 && *replies.rbegin() == 1213, "reply frequencies: %zu, %ld .. %ld", replies.size(), *replies.begin(), *replies.rbegin());
        bool y = false;
        CHECK(dme::channelFromReplyMHz(1104, y) == 17 && !y, "1104 MHz is 17X");
        CHECK(dme::channelFromReplyMHz(962, y) == 1 && !y, "962 MHz is 1X");
        CHECK(dme::channelFromReplyMHz(1088, y) == 1 && y, "1088 MHz is 1Y");
        CHECK(dme::channelFromReplyMHz(1213, y) == 126 && !y, "1213 MHz is 126X");
        CHECK(dme::channelFromReplyMHz(961, y) == 0 && dme::channelFromReplyMHz(1214, y) == 0 && dme::channelFromReplyMHz(1050.5, y) == 0, "outside the plan");
    }

    // ---- channel, pair rate and counts: X, Y reply (30 us), Y interrogation (36 us), with noise
    for (double fs : {2.0e6, 2.4e6, 3.2e6, 4.0e6, 5.0e6, 10.0e6}) {
        for (int mode = 0; mode < 3; mode++) {   // X reply, Y reply, Y interrogation
            const double sp = mode == 0 ? 12.0 : mode == 1 ? 30.0 : 36.0;
            Gen g; g.fs = fs;
            randomPairs(g, 0.01, 1.4, sp, 2700, 7 + (unsigned)mode);
            const std::vector<float> env = g.render(1.5, 0.04f, 3);
            dme::Receiver rx;
            rx.configure(fs);
            feedAll(rx, env);
            const dme::Telemetry& t = rx.telemetry();
            const dme::Channel want = mode == 0 ? dme::Channel::X : dme::Channel::Y;
            CHECK(t.channel == want, "fs %.1f MHz spacing %.0f: channel %s", fs / 1e6, sp, dme::channelName(t.channel));
            CHECK(t.pairRatePps > 2000 && t.pairRatePps < 3000, "fs %.1f MHz spacing %.0f: pair rate %.0f", fs / 1e6, sp, t.pairRatePps);
            if (mode == 2) CHECK(t.pairsYInterrogation > 0 && t.pairsYInterrogation == t.pairsY, "interrogation count %d of %d", t.pairsYInterrogation, t.pairsY);
            else CHECK(t.pairsYInterrogation == 0, "a reply counted as an interrogation");
            CHECK(!t.identKeyed && t.ident.empty(), "ident '%s' from random pairs", t.ident.c_str());
        }
    }

    // ---- the ident, X and Y, normal and slow dots, several words, at several sample rates; random replies in between and before / after
    {
        struct C { const char* id; double dot; double sp; double rate; double fs; };
        const C cases[] = {
            {"MUC", 0.10, 12.0, 2700, 2.4e6}, {"ORD", 0.10, 30.0, 1400, 2.0e6}, {"DLH", 0.16, 12.0, 800, 4.0e6}, {"SYD", 0.12, 30.0, 2700, 5.0e6},
            {"LSZ", 0.10, 12.0, 1350, 2.4e6}, {"ZRH", 0.14, 30.0, 700, 2.0e6}, {"KLM", 0.10, 12.0, 2000, 10.0e6}, {"EEE", 0.10, 12.0, 2700, 2.4e6},
        };
        for (const C& c : cases) {
            Gen g; g.fs = c.fs;
            randomPairs(g, 0.01, 1.0, c.sp, c.rate, 21);
            double t = addIdent(g, 1.0, c.id, c.dot, c.sp, c.rate, 5);
            randomPairs(g, t, t + 1.5, c.sp, c.rate, 22);
            t = addIdent(g, t + 1.5, c.id, c.dot, c.sp, c.rate, 6);   // the repeat
            randomPairs(g, t, t + 1.0, c.sp, c.rate, 23);
            const std::vector<float> env = g.render(t + 1.1, 0.03f, 11);
            dme::Receiver rx;
            rx.configure(c.fs);
            feedAll(rx, env);
            const dme::Telemetry& r = rx.telemetry();
            const std::string want = std::string(c.id) + " " + c.id;   // the group twice, a word gap between
            std::string got = r.ident;
            while (!got.empty() && got.back() == ' ') got.pop_back();
            CHECK(got.find(std::string(c.id)) != std::string::npos && (got == want || got == std::string(c.id)), "ident %s dot %.2f rate %.0f fs %.1f: read '%s'", c.id, c.dot, c.rate, c.fs / 1e6, r.ident.c_str());
            CHECK(r.identIntervalUs > 735 && r.identIntervalUs < 746, "ident interval %.1f us", r.identIntervalUs);
        }
    }

    // ---- things that are not pairs of the channel: other spacings, single pulses, pairs 24 us apart; regular pairs at another rate are not an ident
    {
        Gen g; g.fs = 2.4e6;
        randomPairs(g, 0.01, 1.0, 24.0, 2700, 4);
        for (double tp = 0.02; tp < 1.0; tp += 1.0 / 1350.0) g.pair(tp, 63.0);   // TACAN-like 63 us pairs in a regular train
        std::mt19937 r(9);
        std::uniform_real_distribution<double> ut(0.01, 1.0);
        for (int i = 0; i < 1500; i++) g.pulses.push_back({ut(r), 0.9f});   // single pulses
        const std::vector<float> env = g.render(1.1, 0.03f, 5);
        dme::Receiver rx;
        rx.configure(2.4e6);
        feedAll(rx, env);
        const dme::Telemetry& t = rx.telemetry();
        CHECK(t.pairsX + t.pairsY < 500, "wrong spacings counted: X %d Y %d", t.pairsX, t.pairsY);
        CHECK(t.ident.empty(), "ident '%s' from a regular train of wrong pairs", t.ident.c_str());
    }
    {   // a regular train at 1350 pairs/s of X pairs that never stops is key-down forever, not letters
        Gen g; g.fs = 2.4e6;
        for (double tp = 0.02; tp < 2.0; tp += 1.0 / 1350.0) g.pair(tp, 12.0);
        dme::Receiver rx;
        rx.configure(2.4e6);
        feedAll(rx, g.render(2.1, 0.02f, 8));
        CHECK(rx.telemetry().ident.empty(), "a steady key-down read as '%s'", rx.telemetry().ident.c_str());
    }
    return testSummary("test_dme");
}
