// SuperSID monitor on simulated levels and signals: a sudden rise is found, a slow terminator-like drift and noise are not, the receiver front end measures
// carrier levels, several stations at once. Simulated only: no VLF recording was available. A real flare also needs a check against GOES X-ray data.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

static int idx(const SidMonitor& m, const char* call) {
    for (int i = 0; i < m.nStations(); i++) if (m.state().stations[(size_t)i].call == call) return i;
    return -1;
}

int main() {
    const double dt = 5.0;
    // 1. levels: three stations for 3 hours, a +3 dB step on NAA at 2 h that decays over 40 min
    {
        SidMonitor m;
        TRng r(5);
        const int n = m.nStations();
        const int iN = idx(m, "NAA"), iL = idx(m, "NLK"), iP = idx(m, "NPM");
        double eventAt = -1;
        for (int k = 0; k < 3 * 720; k++) {
            const double t = k * dt;
            std::vector<float> lv((size_t)n, -200.f);
            double naa = 25.0 + 0.15 * r.gauss();
            if (t >= 7200) naa += 3.0 * std::exp(-(t - 7200) / 1500.0);
            lv[(size_t)iN] = (float)naa;
            lv[(size_t)iL] = (float)(18.0 + 0.15 * r.gauss());
            lv[(size_t)iP] = (float)(14.0 + 0.15 * r.gauss() + 0.5 * std::sin(t / 3000.0));   // slow drift
            m.pushLevels(lv, dt);
            if (eventAt < 0 && m.state().stations[(size_t)iN].event) eventAt = t;
            if (t < 7200) CHECK(!m.state().stations[(size_t)iN].event && !m.state().stations[(size_t)iL].event && !m.state().stations[(size_t)iP].event, "false event at %.0f s", t);
        }
        std::printf("SID: flare step at 7200 s found at %.0f s\n", eventAt);
        CHECK(eventAt >= 7200 && eventAt < 7200 + 300, "event found at %.0f s (step at 7200)", eventAt);
        CHECK(m.state().eventsTotal == 1 && m.state().lastEventCall == "NAA", "events %d call %s", m.state().eventsTotal, m.state().lastEventCall.c_str());
        CHECK(!m.state().stations[(size_t)iL].event && !m.state().stations[(size_t)iP].event, "other stations flagged");
        CHECK(!m.state().stations[(size_t)iN].event, "event still on 3 h in (it decays over 25 min)");
        CHECK(m.state().stations[(size_t)iN].history.size() >= 170, "minute history %zu", m.state().stations[(size_t)iN].history.size());
    }
    // 2. a sunset terminator: 4 dB fall over 90 minutes is not a sudden change
    {
        SidMonitor m;
        TRng r(7);
        const int n = m.nStations(), iN = idx(m, "NAA");
        for (int k = 0; k < 4 * 720; k++) {
            std::vector<float> lv((size_t)n, -200.f);
            const double t = k * dt;
            lv[(size_t)iN] = (float)(25.0 + 0.15 * r.gauss() - 4.0 * std::min(1.0, std::max(0.0, (t - 3600) / 5400.0)));
            m.pushLevels(lv, dt);
        }
        CHECK(m.state().eventsTotal == 0, "slow terminator drift gave %d events", m.state().eventsTotal);
    }
    // 3. noise on the level (0.5 dB rms), a station below the 6 dB presence limit never flags
    {
        SidMonitor m;
        TRng r(9);
        const int n = m.nStations(), iN = idx(m, "NAA"), iL = idx(m, "NLK");
        for (int k = 0; k < 3 * 720; k++) {
            std::vector<float> lv((size_t)n, -200.f);
            lv[(size_t)iN] = (float)(22.0 + 0.5 * r.gauss());
            lv[(size_t)iL] = (float)(2.0 + 3.0 * r.gauss());   // under the noise: not present
            m.pushLevels(lv, dt);
        }
        CHECK(m.state().eventsTotal == 0, "noisy levels gave %d events", m.state().eventsTotal);
    }
    // 4. the front end: carriers in complex noise, levels measured, a step in one carrier found
    {
        const double rate = 12000, dial = 22000;
        SidMonitor m;
        m.configure(rate, dial);
        m.setSampleSec(5.0);
        const double dur = 25 * 60;
        std::vector<std::vector<float>> gain(2, std::vector<float>((size_t)dur, 0.f));
        for (size_t s = 1080; s < gain[0].size(); s++) gain[0][s] = 3.0f;   // +3 dB on the first carrier at 18 min
        auto x = sidRender(rate, dial, dur, {{24000.0, 20.0}, {21400.0, 12.0}}, 1.0, gain, 3);
        for (size_t i = 0; i < x.size(); i += 4800) m.feed(x.data() + i, std::min<size_t>(4800, x.size() - i));
        const int iN = idx(m, "NAA"), iP = idx(m, "NPM"), iL = idx(m, "NLK");
        const auto& sN = m.state().stations[(size_t)iN];
        const auto& sP = m.state().stations[(size_t)iP];
        std::printf("SID front end: NAA %.1f dB over noise (carrier 20 dB / 23 after the step, in a 120 Hz band), NPM %.1f dB (12), step events %d\n", (double)sN.levelDb, (double)sP.levelDb, m.state().eventsTotal);
        // the carrier is 20 dB over the noise per sample; in a +-60 Hz band against the noise per bin it is higher: the band holds ~ 40 bins and the carrier one
        CHECK(sN.inSpan && sP.inSpan && m.state().stations[(size_t)iL].inSpan, "stations in the span");
        CHECK(sN.present && sP.present, "present flags");
        CHECK(std::fabs((sN.levelDb - sP.levelDb) - (23.0 - 12.0)) < 1.5, "level difference %.1f dB (want 11 +- 1.5)", (double)(sN.levelDb - sP.levelDb));
        CHECK(m.state().eventsTotal >= 1 && m.state().lastEventCall == "NAA", "step in the render: events %d call '%s'", m.state().eventsTotal, m.state().lastEventCall.c_str());
        CHECK(!m.state().stations[(size_t)iL].present, "NLK has no carrier but is present");
    }
    // 5. out of span stations and a rate that is too high
    {
        SidMonitor m;
        m.configure(12000, 22000);
        int in = 0;
        for (auto& s : m.state().stations) in += s.inSpan;
        CHECK(in >= 6 && in < m.nStations(), "stations in a 12 kHz span: %d", in);
        m.configure(2.4e6, 22000);
        in = 0;
        for (auto& s : m.state().stations) in += s.inSpan;
        CHECK(in == 0, "a 2.4 Msps input must be brought to a VLF rate first");
    }
    std::printf(g_fails ? "SuperSID: %d failures\n" : "SuperSID: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
