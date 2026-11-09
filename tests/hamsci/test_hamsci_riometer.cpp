// Riometer on simulated data: the sidereal time, the quiet-day curve built over several simulated days, an absorption event of known depth, interference
// minutes that must not enter the curve, a gain step, the export / import, and the power estimate from IQ with narrow carriers and bursts in it.
// Simulated only: a real riometer needs a stable receiver gain over days; an SDR with automatic gain would defeat it (see docs/HAM-SCIENCE-3.8.md).
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

int main() {
    // sidereal time: 2000-01-01 12:00 UT (JD 2451545.0) at longitude 0 is GMST 18.697 h
    {
        const double unix2000 = (2451545.0 - 2440587.5) * 86400.0;
        CHECK(std::fabs(Riometer::siderealHours(unix2000, 0) - 18.6974) < 0.001, "GMST at J2000 %.4f", Riometer::siderealHours(unix2000, 0));
        CHECK(std::fabs(Riometer::siderealHours(unix2000 + 86164.0905, 0) - 18.6974) < 0.01, "one sidereal day later");
        CHECK(std::fabs(Riometer::siderealHours(unix2000, 15.0) - 19.6974) < 0.001, "longitude 15 E adds one hour");
    }
    // quiet-day curve over 6 simulated days, minute by minute, noise 0.4 % (0.017 dB) on each minute, then an event
    {
        Riometer rio;
        rio.setPosition(21.0);
        const double t0 = 1.8e9;
        rio.setStartUnix(t0);
        TRng r(3);
        auto sky = [&](double t) { const double lst = Riometer::siderealHours(t, 21.0); return 1.0 * (1.0 + 0.25 * std::cos(2 * 3.14159265 * (lst - 17.7) / 24.0)); };
        double evAbs = 0;
        int evMin = -1, falseEv = 0;
        const int days = 7;
        for (int m = 0; m < days * 1440; m++) {
            const double t = t0 + 60.0 * m;
            double p = sky(t) * (1.0 + 0.004 * r.gauss());
            const bool inEvent = m >= 6 * 1440 + 300 && m < 6 * 1440 + 420;
            if (inEvent) p *= std::pow(10.0, -0.8 / 10.0);   // 0.8 dB absorption for two hours
            rio.pushMinute(p, t, false);
            const auto& s = rio.state();
            if (inEvent && s.event && evMin < 0) evMin = m;
            if (inEvent && m == 6 * 1440 + 360) evAbs = s.absorptionDb;
            if (!inEvent && m >= 2 * 1440 && s.event) falseEv++;
        }
        std::printf("riometer: 0.8 dB event read as %.2f dB, flagged at minute %d of the event, QDC bins %d\n", evAbs, evMin >= 0 ? evMin - (6 * 1440 + 300) : -1, rio.state().qdcFill);
        CHECK(rio.state().haveQdc && rio.state().qdcFill >= 280, "QDC bins %d of 288", rio.state().qdcFill);
        CHECK(std::fabs(evAbs - 0.8) < 0.15, "absorption %.3f dB (want 0.8 +- 0.15)", evAbs);
        CHECK(evMin >= 0, "event never flagged");
        CHECK(falseEv <= 3, "%d minutes flagged outside the event", falseEv);
        // export and import
        Riometer r2;
        CHECK(r2.importQdc(rio.exportQdc()) && r2.state().qdcFill == rio.state().qdcFill, "QDC import");
        CHECK(!r2.importQdc("qdc1 1 2 3") && !r2.importQdc("junk"), "bad QDC text accepted");
    }
    // interference minutes (flagged) and brief rises (+1.5 dB, not flagged) must not lift the curve
    {
        Riometer rio;
        rio.setPosition(0);
        const double t0 = 1.8e9;
        TRng r(5);
        for (int m = 0; m < 5 * 1440; m++) {
            const double t = t0 + 60.0 * m;
            const double lst = Riometer::siderealHours(t, 0);
            double p = (1.0 + 0.2 * std::cos(2 * 3.14159265 * (lst - 17.7) / 24.0)) * (1.0 + 0.004 * r.gauss());
            bool flagged = false;
            if (m % 97 == 0) { p *= 3.0; flagged = true; }          // a loud burst, flagged as interference
            else if (m % 131 == 0) p *= 1.4;                        // a 1.5 dB rise that the receiver did not flag
            rio.pushMinute(p, t, flagged);
        }
        // after day 1 the curve must follow the sky within 0.2 dB
        double worst = 0;
        Riometer probe;
        probe.importQdc(rio.exportQdc());
        const double lst = 17.7;
        (void)lst;
        for (int b = 0; b < 288; b += 7) {
            const double l = (b + 0.5) * 24.0 / 288.0;
            const double want = 1.0 + 0.2 * std::cos(2 * 3.14159265 * (l - 17.7) / 24.0);
            // read the curve through the export text
            std::istringstream is(rio.exportQdc());
            std::string tag; is >> tag;
            float v = 0; for (int i = 0; i <= b; i++) is >> v;
            worst = std::max(worst, std::fabs(10 * std::log10((double)v / want)));
        }
        std::printf("riometer: QDC follows the sky within %.2f dB with interference and rises in the data\n", worst);
        CHECK(worst < 0.8, "QDC lifted by %.2f dB", worst);
    }
    // from IQ: a power step of -0.5 dB seen, narrow carriers and bursts rejected
    {
        const double rate = 48000;
        auto x = rioRender(rate, 130, 1.0, 1.8e9, 21.0, 0.0, {{70.0, 0.5}}, 0.12, 4);
        // a strong carrier at 5 kHz (narrow): the spectrum median ignores it
        for (size_t i = 0; i < x.size(); i++) x[i] += std::complex<float>((float)(3.0 * std::cos(2 * 3.14159265 * 5000.0 * (double)i / rate)), (float)(3.0 * std::sin(2 * 3.14159265 * 5000.0 * (double)i / rate)));
        Riometer rio;
        rio.setStartUnix(1.8e9);
        rio.configure(rate);
        for (size_t i = 0; i < x.size(); i += 4800) rio.feed(x.data() + i, std::min<size_t>(4800, x.size() - i));
        const auto& h = rio.state().powerHistory;
        std::printf("riometer IQ: minute powers (dB):");
        for (float v : h) std::printf(" %.2f", (double)v);
        std::printf("\n");
        CHECK(h.size() >= 5, "minutes %zu", h.size());
        if (h.size() >= 5) {
            CHECK(std::fabs(h[0] - 0.0) < 0.25, "first minute %.2f dB (want 0 with a carrier and 12 %% bursts in the data)", (double)h[0]);
            CHECK(std::fabs(h[4] - (-0.5)) < 0.2, "fifth minute %.2f dB (want -0.5, the step at 70 s)", (double)h[4]);
        }
    }
    std::printf(g_fails ? "riometer: %d failures\n" : "riometer: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
