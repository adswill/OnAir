// The ham science receivers through the engine's front end on their own test signals (hamsci_gen.h): the audio modes at 600 ksps, SuperSID at 48 ksps (a flare
// 17 minutes in), the riometer power trace, the pulsar chain at 2.4 Msps for a few seconds. Simulated only.
#include "dect2/hamsci/hamsci_gen.h"
#include "dect2/hamsci/hamsci_rx.h"
#include "hamsci_testutil.h"
#include <chrono>
using namespace dect2;

static HamsciTelemetry run(int kind, double rate, double seconds, int opt0 = 0, double snr = 20, double cfo = 0, double centre = 0, double offset = 0) {
    SynthConfig sc;
    sc.snrDb = snr; sc.cfoHz = cfo; sc.modeOpt[0] = opt0;
    auto syn = makeHamsciSynth(sc, rate, kind);
    HamsciReceiver rx(kind);
    rx.configure(rate);
    if (centre > 0) rx.setCenterHz(centre);
    rx.setSignalOffset(offset);
    HamsciTelemetry t, last;
    uint64_t seq = 0;
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(seconds * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t m = std::min(buf.size(), total - done);
        syn->generate(buf.data(), m);
        rx.feed(buf.data(), m);
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // tuning table
    for (int k = 0; k < kHamsciKinds; k++) {
        const ModeTuning t = hamsciTuning(k);
        CHECK(t.stdMode == hamsciFamily(k) + 2 && hamsciKindOfFamily(hamsciFamily(k)) == k && hamsciKindOfStd(t.stdMode) == k, "numbering of kind %d", k);
    }
    const double R = 600000;
    {
        auto t = run(kHsFsk441, R, 26, 0, 25);
        std::printf("FSK441: %zu lines, first '%s'\n", t.lines.size(), t.lines.empty() ? "" : t.lines.back().text.c_str());
        CHECK(!t.lines.empty() && t.lines.front().text.find("K1JT") != std::string::npos, "FSK441 through the receiver");
    }
    {
        auto t = run(kHsIscat, R, 34, 0, 20, 25.0);
        std::printf("ISCAT: %zu lines, '%s' (%s)\n", t.lines.size(), t.lines.empty() ? "" : t.lines[0].text.c_str(), t.found.c_str());
        CHECK(!t.lines.empty() && t.lines[0].text == "CQ DX K1JT FN20", "ISCAT-A through the receiver");
        auto b = run(kHsIscat, R, 34, 1, 20, -30.0);
        CHECK(!b.lines.empty() && b.lines[0].text == "CQ DX K1JT FN20", "ISCAT-B through the receiver: %zu lines", b.lines.size());
    }
    {
        auto t = run(kHsPi4, R, 40, 0, 20);
        std::printf("PI4: '%s' (%s)\n", t.lines.empty() ? "" : t.lines[0].text.c_str(), t.found.c_str());
        CHECK(!t.lines.empty() && t.lines[0].text == "PI4ABC", "PI4 through the receiver");
    }
    {
        auto t = run(kHsThrob, R, 36, 1, 20);
        std::printf("Throb: '%s' (%s)\n", t.text.c_str(), t.found.c_str());
        CHECK(t.text.find("CQCQ") != std::string::npos || t.text.find("CQ CQ") != std::string::npos, "Throb2 through the receiver: '%s'", t.text.c_str());
        CHECK(t.variant == 1, "Throb variant %d", t.variant);
    }
    {
        auto t = run(kHsQrss, R, 130, 0, 25);
        std::printf("QRSS: '%s' (%s)\n", t.text.c_str(), t.found.c_str());
        CHECK(t.text.find("CQ") != std::string::npos, "QRSS through the receiver: '%s'", t.text.c_str());
        auto d = run(kHsDfcw, R, 140, 0, 25);
        std::printf("DFCW: '%s' (%s)\n", d.text.c_str(), d.found.c_str());
        CHECK(d.text.find("CQ") != std::string::npos, "DFCW through the receiver: '%s'", d.text.c_str());
    }
    {   // SuperSID: 48 ksps, centre 22 kHz, the flare option
        auto t = run(kHsSid, 48000, 22 * 60, 1, 99, 0, 22000);
        int heard = 0;
        for (auto& s : t.sid.stations) heard += s.present;
        std::printf("SuperSID: %d stations heard, %d events, last %s %+.1f dB\n", heard, t.sid.eventsTotal, t.sid.lastEventCall.c_str(), t.sid.lastEventRiseDb);
        CHECK(heard == 4, "stations heard %d (want 4)", heard);
        CHECK(t.sid.eventsTotal == 1 && t.sid.lastEventCall == "NAA", "flare: events %d call '%s'", t.sid.eventsTotal, t.sid.lastEventCall.c_str());
        auto q = run(kHsSid, 48000, 20 * 60, 0, 99, 0, 22000);
        CHECK(q.sid.eventsTotal == 0, "quiet run gave %d events", q.sid.eventsTotal);
    }
    {   // riometer: a few minutes
        auto t = run(kHsRiometer, 48000, 130, 0, 99);
        std::printf("Riometer: %zu minute values\n", t.rio.powerHistory.size());
        CHECK(t.rio.powerHistory.size() >= 2 && !t.rio.haveQdc, "riometer minutes %zu, no curve yet", t.rio.powerHistory.size());
    }
    {   // pulsar chain: 2.4 Msps for 4 s, the folder collects rows
        auto t = run(kHsPulsar, 2.4e6, 4, 0, 99, 0, 408e6);
        std::printf("Pulsar chain: observed %.1f s\n", t.pulsarObservedSec);
        CHECK(std::fabs(t.pulsarObservedSec - 4.0) < 0.1, "observed %.2f s", t.pulsarObservedSec);
    }
    std::printf(g_fails ? "receivers: %d failures\n" : "receivers: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
