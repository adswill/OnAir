// COSPAS-SARSAT receiver on the generated test signal: every beacon of a cycle must arrive with its exact hex ID, clean, in noise, with carrier offsets
// of a few kHz, a bit rate that is 0.8 % off, reversed polarity and at other sample rates. Noise alone must give nothing.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static SarsatTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false, bool wide = false) {
    SarsatReceiver rx;
    rx.configure(rate);
    rx.setWideSearch(wide);
    rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
    auto syn = makeSarsatSynth(cfg, rate);
    std::vector<cf32> buf(32768);
    SarsatTelemetry t;
    uint64_t seq = 0;
    unsigned s = 777;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        SarsatTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static int countFound(const SarsatTelemetry& t, const char* what) {
    int found = 0;
    for (const sarsat::Spec& w : sarsat::testBeacons()) {
        std::vector<uint8_t> bits = sarsat::encode(w);
        sarsat::Beacon ref;
        sarsat::decode(bits, ref);
        bool ok = false;
        for (const SarsatEntry& e : t.log)
            if (e.b.hexId == ref.hexId && e.b.hasPos == ref.hasPos && std::fabs(e.b.lat - ref.lat) < 1e-6 && std::fabs(e.b.lon - ref.lon) < 1e-6 && e.b.idText == ref.idText) { ok = true; break; }
        CHECK(ok, "%s: missing beacon %s", what, ref.hexId.c_str());
        found += ok;
    }
    std::set<std::string> known;
    for (const sarsat::Spec& w : sarsat::testBeacons()) { std::vector<uint8_t> b = sarsat::encode(w); sarsat::Beacon r; sarsat::decode(b, r); known.insert(r.hexId); }
    int stray = 0;
    for (const SarsatEntry& e : t.log) if (!known.count(e.b.hexId)) { stray++; printf("  stray %s corr %d t %.2f snr %.1f cfo %.0f long %d id %s\n", e.b.hexId.c_str(), e.b.corrected, e.timeSec, e.snrDb, e.cfoHz, (int)e.b.longMsg, e.b.idText.c_str()); }
    CHECK(stray == 0, "%s: %d messages that were not sent", what, stray);
    return found;
}

int main() {
    const double rate = 256000;
    const double seconds = 5 * 3.6 + 3;   // five bursts of at most 0.62 s, 3 s apart
    {
        SynthConfig cfg; cfg.snrDb = 40;
        countFound(run(cfg, rate, seconds), "clean");
    }
    {
        SynthConfig cfg; cfg.snrDb = 22; cfg.cfoHz = 4500;
        countFound(run(cfg, rate, seconds), "22 dB, +4.5 kHz");
    }
    {
        SynthConfig cfg; cfg.snrDb = 20; cfg.cfoHz = -7200;
        countFound(run(cfg, rate, seconds), "20 dB, -7.2 kHz");
    }
    {   // bit rate 0.8 % fast
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeVal[0] = 0.8; cfg.cfoHz = 1200;
        countFound(run(cfg, rate, seconds), "bit rate +0.8 %");
    }
    {
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeVal[0] = -0.8;
        countFound(run(cfg, rate, seconds), "bit rate -0.8 %");
    }
    {   // reversed phase polarity
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeOpt[1] = 1;
        countFound(run(cfg, rate, seconds), "reversed polarity");
    }
    {
        SynthConfig cfg; cfg.snrDb = 25; cfg.cfoHz = 2000;
        countFound(run(cfg, 1024000, seconds), "1.024 Msps");
        countFound(run(cfg, 2400000, seconds), "2.4 Msps");
    }
    {   // edges of the search range and weak signals
        SynthConfig cfg; cfg.snrDb = 25; cfg.cfoHz = 11500;
        countFound(run(cfg, rate, seconds), "+11.5 kHz");
        cfg.cfoHz = -11500;
        countFound(run(cfg, rate, seconds), "-11.5 kHz");
        cfg.cfoHz = 40000;
        countFound(run(cfg, rate, seconds), "+40 kHz (wide transponder)");
        cfg.cfoHz = -40000;
        countFound(run(cfg, rate, seconds), "-40 kHz (wide transponder)");
        cfg.cfoHz = 300; cfg.snrDb = 14;
        countFound(run(cfg, rate, seconds * 2), "14 dB");
    }
    {   // the whole-capture search (issue #214, the 1544 - 1545 MHz downlink): beacons far from the dial at 1 Msps, merged without duplicates
        SynthConfig cfg; cfg.snrDb = 25;
        for (double off : {330000.0, -260000.0, 2000.0}) {
            cfg.cfoHz = off;
            char what[64]; snprintf(what, sizeof what, "whole capture, %+.0f kHz", off / 1e3);
            const SarsatTelemetry t = run(cfg, 1e6, seconds, false, true);
            countFound(t, what);
            CHECK(t.wideChannels >= 9 && t.wideSpanHz >= 800e3, "%s: %d channels", what, t.wideChannels);
            if (!t.log.empty()) CHECK(std::fabs(t.log.front().cfoHz - off) < 3000, "%s: reported at %.0f Hz", what, t.log.front().cfoHz);
            CHECK((int)t.log.size() <= (int)sarsat::testBeacons().size() * 2, "%s: %zu entries (duplicates?)", what, t.log.size());
        }
        const SarsatTelemetry n = run(cfg, 1e6, 10, true, true);
        CHECK(n.log.empty(), "whole capture: noise gave %zu messages", n.log.size());
    }
    {   // a real recording (issue #223): I/Q wav at 37.5 kHz, carrier near +1.44 kHz, moved to the tuning offset at 256 kHz
        std::string path = __FILE__;
        for (char& ch : path) if (ch == '\\') ch = '/';   // MinGW gives backslashes
        const size_t cut = path.rfind("tests/");
        path = (cut == std::string::npos ? std::string() : path.substr(0, cut)) + "tests/data/sarsat/epirbsignal_223.wav";
        FILE* f = fopen(path.c_str(), "rb");
        CHECK(f != nullptr, "cannot open %s", path.c_str());
        if (f) {
            std::vector<uint8_t> raw;
            uint8_t tmp[4096];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) raw.insert(raw.end(), tmp, tmp + n);
            fclose(f);
            const size_t frames = (raw.size() - 44) / 4;
            std::vector<cf32> in(frames);
            for (size_t i = 0; i < frames; i++) {
                const int16_t* s = (const int16_t*)&raw[44 + i * 4];
                in[i] = cf32(s[0] / 32768.f, s[1] / 32768.f);
            }
            const double inRate = 37500, outRate = 256000, shift = 20000 - 1440;
            const size_t pad = (size_t)outRate, outN = (size_t)(frames * outRate / inRate) + 2 * pad;
            SarsatReceiver rx;
            rx.configure(outRate);
            rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
            std::vector<cf32> out(outN);
            for (size_t i = 0; i < outN; i++) {
                cf32 v(0, 0);
                if (i >= pad && i < outN - pad) {
                    const double p = (i - pad) * inRate / outRate;
                    const size_t k = (size_t)p;
                    const float fr = (float)(p - k);
                    v = in[k] * (1 - fr) + in[std::min(k + 1, frames - 1)] * fr;
                }
                const double ph = 2 * M_PI * shift * (double)i / outRate;
                out[i] = v * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            for (size_t i = 0; i < outN; i += 4096) rx.feed(out.data() + i, std::min<size_t>(4096, outN - i));
            SarsatTelemetry t;
            rx.telemetry(t, 0);
            CHECK(t.log.size() == 1, "real recording: %zu messages, expected 1", t.log.size());
            if (!t.log.empty()) CHECK(t.log[0].b.hexId == "4016196C3F81FE0" && t.log[0].b.valid, "real recording: wrong ID %s", t.log[0].b.hexId.c_str());
        }
    }
    {
        SynthConfig cfg;
        const SarsatTelemetry t = run(cfg, rate, 20, true);
        CHECK(t.log.empty(), "noise gave %zu messages", t.log.size());
    }
    {   // a radio with no antenna: noise with short blips, a wandering tone and a tone that comes and goes. None of it is a burst.
        SarsatReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
        const double f0 = sarsatTuning().tuneOffsetHz + 4000.0;
        std::vector<cf32> buf(32768);
        unsigned s = 4242;
        uint64_t n = 0, seq = 0;
        double ph = 0;
        SarsatTelemetry t;
        for (double done = 0; done < 30 * rate; done += (double)buf.size()) {
            for (auto& v : buf) {
                s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f;
                s = s * 1664525u + 1013904223u; const float b = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f;
                const double sec = (double)n / rate;
                const double cyc = std::fmod(sec, 3.0);
                double amp = 0, f = f0;
                if (cyc < 0.15) amp = 0.4;                                              // a 150 ms blip
                else if (cyc >= 1.0 && cyc < 1.6) { amp = 0.4; f = f0 + (cyc - 1.0) * 5000.0; }   // 0.6 s tone that wanders 3 kHz
                else if (cyc >= 2.0 && cyc < 2.5) { amp = (std::fmod(sec * 4, 1.0) < 0.5) ? 0.4 : 0.0; }   // keyed on and off
                ph += 2 * M_PI * f / rate;
                v = cf32(a * 0.3f + (float)(amp * std::cos(ph)), b * 0.3f + (float)(amp * std::sin(ph)));
                n++;
            }
            rx.feed(buf.data(), buf.size());
            SarsatTelemetry x;
            if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
        }
        CHECK(t.bursts == 0, "spurs counted as %llu bursts", (unsigned long long)t.bursts);
        CHECK(t.blocksBad == 0 && t.log.empty(), "spurs: %llu failed checks, %zu messages", (unsigned long long)t.blocksBad, t.log.size());
    }
    return testSummary("test_sarsat_rx");
}
