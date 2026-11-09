// Shortwave station identification: schedule parser, language scoring, the interval signal detector (a repeating melody yes, noise and speech-like audio no), and matching a taught melody.
#include "dect2/general/sw_ident.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<float> melody(const std::vector<double>& notes, double noteSec, double totalSec, int rate, double noiseAmp, unsigned seed) {
    std::mt19937 r(seed); std::normal_distribution<float> g(0, 1);
    const size_t n = (size_t)(totalSec * rate);
    std::vector<float> x(n);
    const size_t noteLen = (size_t)(noteSec * rate);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const size_t idx = (i / noteLen) % notes.size();
        ph += 2 * M_PI * notes[idx] / rate;
        const double e = std::min(1.0, 8.0 * (double)(i % noteLen) / (double)noteLen);
        x[i] = (float)(0.4 * e * std::sin(ph) + 0.15 * e * std::sin(2 * ph)) + (float)noiseAmp * g(r);
    }
    return x;
}

static IntervalDetector run(const std::vector<float>& x, int rate) { IntervalDetector d; d.configure(rate); for (size_t i = 0; i < x.size(); i += 4800) d.push(x.data() + i, std::min<size_t>(4800, x.size() - i)); return d; }

int main() {
    const int rate = 48000;
    const std::vector<double> m1 = {523, 659, 784, 1047, 784, 659, 587, 523}, m2 = {440, 349, 262, 330, 415, 494, 392, 294};
    // detection over noise levels and tempos
    for (double noise : {0.0, 0.05, 0.15}) for (double noteSec : {0.35, 0.5}) {
        auto x = melody(m1, noteSec, 40, rate, noise, 11);
        IntervalDetector d = run(x, rate);
        printf("melody noise %.2f note %.2f s: repeating %d strength %.2f period %.2f s (true %.2f)\n", noise, noteSec, d.repeating(), d.strength(), d.periodSec(), noteSec * 8);
        CHECK(d.repeating(), "melody not found at noise %.2f", noise);
        CHECK(std::fabs(d.periodSec() - noteSec * 8) < 0.5, "period %.2f, wanted %.2f", d.periodSec(), noteSec * 8);
    }
    {   // noise alone and speech-like audio are not interval signals
        std::mt19937 r(5); std::normal_distribution<float> g(0, 1);
        std::vector<float> noise((size_t)(40 * rate));
        float lp = 0; for (auto& v : noise) { lp += (g(r) - lp) * 0.2f; v = lp; }
        IntervalDetector d = run(noise, rate);
        printf("noise: strength %.2f\n", d.strength());
        CHECK(!d.repeating(), "noise reads as an interval signal (%.2f)", d.strength());
        std::vector<float> speech((size_t)(40 * rate));
        double ph = 0; double f = 180; lp = 0; int hold = 0;
        for (size_t i = 0; i < speech.size(); i++) {
            if (--hold <= 0) { hold = 4000 + (int)(r() % 12000); f = 120 + (r() % 1000); }
            ph += 2 * M_PI * f / rate;
            speech[i] = (float)(0.3 * std::sin(ph) * (0.5 + 0.5 * std::sin(2 * M_PI * 4.0 * (double)i / rate))) + 0.05f * g(r);
        }
        IntervalDetector e = run(speech, rate);
        printf("speech-like: strength %.2f\n", e.strength());
        CHECK(!e.repeating(), "speech-like audio reads as an interval signal (%.2f)", e.strength());
    }
    // teach and recognise; another melody does not match
    {
        auto a = melody(m1, 0.5, 40, rate, 0.05, 1), b = melody(m1, 0.5, 40, rate, 0.15, 2), c = melody(m2, 0.5, 40, rate, 0.05, 3);
        IntervalDetector da = run(a, rate), db = run(b, rate), dc = run(c, rate);
        const float same = IntervalDetector::match(da.fingerprint(), db.fingerprint()), other = IntervalDetector::match(da.fingerprint(), dc.fingerprint());
        printf("match: same melody %.2f, another melody %.2f\n", same, other);
        CHECK(same > 0.85f, "same melody %.2f", same);
        CHECK(other < 0.75f, "another melody %.2f", other);
        SwIdent id;
        id.teachInterval("Example Radio", da.fingerprint());
        SwIdentInput in; in.khz = 9650; in.utcMinute = 600; in.intervalSignal = true; in.intervalPrint = db.fingerprint();
        auto g = id.guess(in);
        CHECK(!g.empty() && g[0].station == "Example Radio" && g[0].estimated, "taught signal");
    }
    {   // the schedule file and the language
        const std::string f = "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\n"
                              "9650;0900-1000;;CHN;Radio One;E;Eu;;;;;\n9650;0930-1030;Sa,Su;KOR;Radio Two;K;As;;;;;\n9650;2200-0100;;USA;Night Radio;E;Am;;;;;\n";
        auto e = parseEibi(f);
        CHECK(e.size() == 3, "parsed %zu", e.size());
        CHECK(e[1].days == 0x60, "days %02X", e[1].days);
        CHECK(e[0].lang == "en", "lang %s", e[0].lang.c_str());
        SwIdent id; id.addSchedule(e);
        SwIdentInput in; in.khz = 9650; in.utcMinute = 9 * 60 + 40; in.weekdayMon0 = 5; in.language = "ko"; in.languageProb = 0.9f;
        auto g = id.guess(in);
        CHECK(g.size() >= 2 && g[0].station == "Radio Two" && g[0].score > 0.7f, "language decides between two scheduled stations ('%s')", g.empty() ? "" : g[0].station.c_str());
        in.language = ""; in.utcMinute = 23 * 60 + 30;
        g = id.guess(in);
        CHECK(g.size() == 1 && g[0].station == "Night Radio", "past midnight");
        SwIdent none; SwIdentInput q; q.khz = 7000; q.language = "de";
        g = none.guess(q);
        CHECK(g.size() == 1 && g[0].score < 0.5f, "language alone");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
