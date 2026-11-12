// The sensor-family entries (families 564 .. 681, sensors_rx.h: sensorsViews): views onto the Sub-GHz sensors receiver that report one family only.
// The real pulse trains of tests/sensors/families.txt (one recording per family of rtl_433's test set) are rebuilt as IQ signals, with a carrier offset,
// a pulse-timing error of +-4 % (a sensor's clock) and noise, and run through every view:
//   - a view reports a message exactly when the receiver without a view decodes it and the model belongs to the view's family (no leak to another family's view);
//   - the receiver without a view and the view of the right family decode the same number of messages;
//   - a sweep of the carrier-to-noise ratio prints how many of the recordings the view of their own family still decodes;
//   - noise alone and a plain carrier give no message in any view, and a view silences the unknown signal analyser.
// The four families without a recording here (Neptune R900, Renault, Toyota, Citroen TPMS) are checked for their model patterns only (every decoder
// of the family exists and its model name is matched by exactly one view). Simulated signals built from recordings of the sensors' pulses, not an over-the-air test.
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string>
#include <fstream>
#include <random>
#include <sstream>
using namespace dect2;
#include "../common/test_util.h"

struct Entry { std::string fam, model, id, type; double f1 = 0, f2 = 0; std::vector<int> p; };

static std::vector<Entry> load() {
    std::vector<Entry> v;
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/tests/sensors/families.txt");
    std::string l;
    while (std::getline(in, l)) {
        std::vector<std::string> f;
        std::stringstream ss(l);
        std::string t;
        while (std::getline(ss, t, '|')) f.push_back(t);
        if (f.size() < 7) continue;
        Entry e;
        e.fam = f[0]; e.model = f[1]; e.id = f[2]; e.type = f[3]; e.f1 = atof(f[4].c_str()); e.f2 = atof(f[5].c_str());
        std::stringstream ps(f[6]);
        while (std::getline(ps, t, ',')) e.p.push_back(atoi(t.c_str()));
        v.push_back(std::move(e));
    }
    return v;
}

// the recording as IQ: widths scaled by `scale` (a clock error), the carrier (OOK) or the two tones (FSK) moved by offsetHz, complex noise of power noisePow
static std::vector<cf32> synth(const Entry& e, double rate, double scale, double offsetHz, double noisePow, unsigned seed) {
    std::vector<cf32> x(size_t(0.05 * rate), cf32(0, 0));
    const double sps = rate / 1e6;
    double ph = 0;
    for (size_t i = 0; i + 1 < e.p.size(); i += 2) {
        for (int k = 0; k < 2; k++) {
            const size_t n = (size_t)std::lround(e.p[i + k] * sps * scale);
            const bool high = k == 0;
            const double hz = (e.type == "fsk" ? (high ? e.f1 : e.f2) : 0) + offsetHz;
            for (size_t j = 0; j < n; j++) {
                ph += 2 * math::kPi * hz / rate;
                const float a = e.type == "fsk" ? 0.5f : (high ? 0.5f : 0.f);
                x.push_back(cf32(a * (float)std::cos(ph), a * (float)std::sin(ph)));
            }
        }
    }
    x.insert(x.end(), size_t(0.08 * rate), cf32(0, 0));
    if (noisePow > 0) {
        std::mt19937 g(seed); std::normal_distribution<float> nd(0.f, 1.f); const float s = (float)std::sqrt(noisePow / 2);
        for (auto& v : x) v += cf32(s * nd(g), s * nd(g));
    }
    return x;
}

static SensorsTelemetry run(const std::vector<cf32>& x, double rate, const std::string& view) {
    SensorsReceiver rx;
    rx.setAutoLevel(true);
    rx.setView(view);
    rx.configure(rate);
    const size_t blk = 8192;
    for (size_t o = 0; o < x.size(); o += blk) rx.feed(x.data() + o, std::min(blk, x.size() - o));
    rx.flush();
    SensorsTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static bool matches(const std::string& model, const std::string& patterns, const std::string& kind = "") {
    std::string m = model; for (auto& c : m) c = (char)std::tolower((unsigned char)c);
    std::string cur;
    for (char c : patterns + "|") {
        if (c == '|') { if (!cur.empty()) { if (cur.rfind("kind:", 0) == 0) { if (cur.substr(5) == kind) return true; } else if (m.find(cur) != std::string::npos) return true; } cur.clear(); }
        else cur += c;
    }
    return false;
}

int main() {
    int nv = 0;
    const SensorsView* views = sensorsViews(nv);
    CHECK(nv == 22, "%d views", nv);
    for (int i = 0; i < nv; i++) {   // numbers: family, active standard, engine standard, tuning
        CHECK(sensorsViewForActive(views[i].family + 1) == &views[i] && sensorsViewForStd(views[i].family + 2) == &views[i], "view %d lookup", i);
        const ModeTuning t = sensorsViewTuning(i);
        CHECK(t.stdMode == views[i].family + 2 && std::string(t.id) == views[i].id, "view %d tuning", i);
        for (int j = 0; j < i; j++) CHECK(views[j].family != views[i].family && std::strcmp(views[j].id, views[i].id) != 0, "views %d and %d clash", j, i);
    }
    CHECK(sensorsViewForActive(242) == nullptr && sensorsViewForStd(243) == nullptr, "the Sub-GHz sensors mode is not a view");

    // every decoder of rtl_433 that belongs to a family is reached by exactly one view (the four families without a recording: their decoders exist)
    {
        const char* want[][2] = {{"Neptune R900", "neptune"}, {"Renault TPMS", "renault"}, {"Toyota TPMS", "toyota"}, {"Citroen TPMS", "citroen"}, {"Schrader TPMS", "schrader"}, {"Oregon Scientific Weather Sensor", "oregon"}, {"HIDEKI TS04", "hideki"}, {"Esperanza EWS", "esperanza"}, {"X10 RF", "x10"}};
        const auto protos = SensorsReceiver::protocols();
        for (auto& w : want) {
            bool have = false;
            for (const auto& p : protos) { std::string a = p.name, b = w[0]; for (auto& c : a) c = (char)std::tolower((unsigned char)c); for (auto& c : b) c = (char)std::tolower((unsigned char)c); if (a.rfind(b, 0) == 0) have = true; }
            CHECK(have, "no decoder named %s", w[0]);
            int views_hit = 0; for (int i = 0; i < nv; i++) if (matches(w[1], views[i].patterns)) views_hit++;
            CHECK(views_hit >= 1, "no view takes %s", w[1]);
        }
    }

    const auto es = load();
    CHECK(es.size() >= 20, "families table has %zu packages", es.size());
    int own = 0, ownOk = 0, leaks = 0, lost = 0;
    const double offs[] = {0, 18000, -27000};
    const double scales[] = {1.0, 1.04, 0.96};
    unsigned seed = 1;
    for (const auto& e : es) {
        int minW = 1 << 30;
        for (int w : e.p) if (w > 0) minW = std::min(minW, w);
        const double rate = minW < 80 ? 1e6 : 250e3;
        for (int k = 0; k < 3; k++) {
            const double off = e.type == "fsk" ? 0 : offs[k];   // an FSK recording carries its own tone offsets
            const double fo = e.type == "fsk" ? offs[k] * 0.4 : 0;
            Entry ee = e; ee.f1 += fo; ee.f2 += fo;
            const auto x = synth(ee, rate, scales[k], off, 0.0, seed++);
            const SensorsTelemetry base = run(x, rate, "");
            bool baseHit = false; std::string kind; for (const auto& d : base.devices) if (d.model == e.model) { baseHit = true; kind = d.kind; }
            for (int i = 0; i < nv; i++) {
                const SensorsTelemetry t = run(x, rate, views[i].patterns);
                const bool mine = matches(e.model, views[i].patterns, kind);
                size_t foreign = 0, got = 0;
                for (const auto& d : t.devices) { if (matches(d.model, views[i].patterns, d.kind)) got++; else foreign++; }
                if (foreign) { leaks++; printf("  LEAK %s view %s: %zu foreign devices (offset %.0f, scale %.2f)\n", e.model.c_str(), views[i].id, foreign, off, scales[k]); }
                CHECK(t.unknown.empty(), "view %s: the unknown signal analyser is not silent", views[i].id);
                if (mine) { own++; if (baseHit) { if (got > 0) ownOk++; else { lost++; printf("  LOST %s in its own view %s (offset %.0f, scale %.2f)\n", e.model.c_str(), views[i].id, off, scales[k]); } } }
                else CHECK(got == 0, "%s reached view %s", e.model.c_str(), views[i].id);
            }
        }
    }
    printf("recordings x impairments in their own view: %d, decoded as without a view: %d, lost: %d, leaks into another view: %d\n", own, ownOk, lost, leaks);
    CHECK(leaks == 0 && lost == 0, "leaks %d lost %d", leaks, lost);

    // carrier to noise sweep (carrier amplitude 0.5, noise power per sample): the views of the first nine families with a recording
    printf("noise sweep (carrier to noise in the sample bandwidth): recordings decoded in their own view\n");
    for (double cn : {30.0, 20.0, 14.0, 10.0, 6.0, 3.0, 0.0}) {
        const double np = 0.25 / std::pow(10.0, cn / 10.0);
        int tot = 0, ok = 0;
        for (const auto& e : es) {
            if (e.type == "fsk") continue;
            int minW = 1 << 30; for (int w : e.p) if (w > 0) minW = std::min(minW, w);
            const double rate = minW < 80 ? 1e6 : 250e3;
            const auto x = synth(e, rate, 1.0, 0, np, seed++);
            for (int i = 0; i < nv; i++) if (matches(e.model, views[i].patterns)) {
                const auto t = run(x, rate, views[i].patterns); tot++; if (!t.devices.empty()) ok++;
            }
        }
        printf("  %.0f dB: %d of %d\n", cn, ok, tot);
        if (cn >= 30) CHECK(ok >= tot - 2, "30 dB: %d of %d", ok, tot);
    }

    // noise alone and a plain carrier: no message in any view
    for (int i = 0; i < nv; i++) {
        std::mt19937 g(77); std::normal_distribution<float> nd(0.f, 0.2f);
        std::vector<cf32> n(size_t(250e3 * 20)); for (auto& v : n) v = cf32(nd(g), nd(g));
        auto t = run(n, 250e3, views[i].patterns);
        CHECK(t.devices.empty() && t.unknown.empty(), "view %s: noise gave %zu devices", views[i].id, t.devices.size());
        std::vector<cf32> c(size_t(250e3 * 5), cf32(0.5f, 0.f)); t = run(c, 250e3, views[i].patterns);
        CHECK(t.devices.empty(), "view %s: a carrier gave %zu devices", views[i].id, t.devices.size());
    }
    return testSummary("sensors views");
}
