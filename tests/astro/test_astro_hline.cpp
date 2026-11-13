// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hydrogen line toolkit (astro/hline.h): synthetic sky (Gaussian components + Tsys + RFI + drift) calibration recovery, LSR corrections and
// pointing against published worked examples (Meeus, Astronomical Algorithms 2nd ed.: ex. 12.a sidereal time, 25.a Sun, 47.a Moon, 15.a Venus
// transit / rise / set at Boston; IAU standard solar apex in galactic coordinates l = 56.1, b = +22.8), Gaussian fit accuracy, the l-v map
// with a flat rotation curve, FITS round trip, session and CSV text.
#include "dect2/astro/hline.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2::astro::hl;
#include "../common/test_util.h"

static double gauss(double x, double a, double c, double s) { const double d = (x - c) / s; return a * std::exp(-0.5 * d * d); }

int main() {
    std::mt19937_64 rng(7);
    dect2::normal_distribution<double> nd(0, 1);

    // ---- Y-factor: Trx 60 K, hot 300 K, cold 77 K, gain 2 units / K
    {
        const double trx = 60, g = 2;
        const YFactor y = yFactor(g * (trx + 300), g * (trx + 77), 300, 77);
        CHECK(y.ok && std::fabs(y.trx - trx) < 1e-9 && std::fabs(y.gainPerK - g) < 1e-9, "yfactor trx %.4f gain %.4f", y.trx, y.gainPerK);
        CHECK(!yFactor(1, 2, 300, 77).ok, "inverted loads accepted");
    }
    // ---- synthetic sky: tsys 100 K, two lines, slope, RFI, calibration recovery (position switching)
    const int N = 2048;
    std::vector<double> v(N), on(N), off(N), truth(N);
    for (int i = 0; i < N; i++) {
        v[(size_t)i] = -250 + 500.0 * i / (N - 1);
        const double band = 1 + 0.3 * (i / (double)N - 0.5) + 0.05 * std::sin(i * 0.5);   // bandpass with ripple
        truth[(size_t)i] = gauss(v[(size_t)i], 6.0, -20, 8) + gauss(v[(size_t)i], 3.0, 60, 15);   // K
        const double noise = 1.0 / std::sqrt(2e5);
        off[(size_t)i] = band * (1 + noise * nd(rng));
        on[(size_t)i] = band * (1 + truth[(size_t)i] / 100.0) * (1 + noise * nd(rng));
    }
    const std::vector<double> ta = antennaTemperature(on, off, 100.0);
    {
        double worst = 0;
        for (int i = 0; i < N; i++) worst = std::max(worst, std::fabs(ta[(size_t)i] - truth[(size_t)i]));
        CHECK(worst < 1.6, "calibration recovery worst error %.3f K", worst);
        const std::vector<double> gain = perBinGain(off, 101), flat = applyGain(off, gain), sm = medianFilter(off, 101);
        double r = 0; for (int i = 0; i < N; i++) r = std::max(r, std::fabs(flat[(size_t)i] / sm[(size_t)i] - 1));
        CHECK(r < 1e-9, "gain flatten %.3g", r);
    }
    // ---- RFI: three spikes in one spectrum, a bursty channel over time
    {
        std::vector<double> s = on;
        s[300] += 0.5; s[301] += 0.3; s[1500] += 0.8;
        const auto f = flagRobust(s, RfiParams());
        CHECK(f[300] && f[1500], "spikes not flagged");
        int bad = 0; for (int i = 0; i < N; i++) if (f[(size_t)i] && i != 300 && i != 301 && i != 1500 && i != 299 && i != 302) bad++;
        CHECK(bad <= 4, "false flags %d", bad);
        const auto fixed = maskInterpolate(s, f);
        CHECK(std::fabs(fixed[300] - on[300]) < 0.1 && std::fabs(fixed[1500] - on[1500]) < 0.1, "interpolation off: %.3f vs %.3f", fixed[1500], on[1500]);
        std::vector<std::vector<double>> rows;
        std::exponential_distribution<double> ex(1.0);
        for (int t = 0; t < 200; t++) { std::vector<double> r(256); for (auto& x : r) x = ex(rng); if (t % 10 == 0) r[40] += 40; rows.push_back(r); }
        const auto fk = flagKurtosis(rows, 1, RfiParams());
        int nf = 0; for (bool b : fk) nf += b;
        CHECK(fk.size() == 256 && fk[40] && nf <= 3, "kurtosis: ch40 %d, total %d", fk.size() > 40 ? (int)fk[40] : -1, nf);
    }
    // ---- Gaussian fit: two components in noise
    {
        std::vector<double> y(N);
        for (int i = 0; i < N; i++) y[(size_t)i] = 0.2 + gauss(v[(size_t)i], 6.0, -20, 8) + gauss(v[(size_t)i], 3.0, 60, 15) + 0.3 * nd(rng);
        auto g0 = guessComponents(v, y, 4);
        CHECK(g0.size() == 2, "guess found %zu components", g0.size());
        const FitResult f = fitGaussians(v, y, g0);
        CHECK(f.ok && f.comps.size() == 2, "fit failed");
        if (f.ok && f.comps.size() == 2) {
            auto c = f.comps;
            if (c[0].centre > c[1].centre) std::swap(c[0], c[1]);
            CHECK(std::fabs(c[0].amp - 6) < 4 * c[0].dAmp + 0.01 && std::fabs(c[0].centre + 20) < 4 * c[0].dCentre && std::fabs(c[0].sigma - 8) < 4 * c[0].dSigma, "comp 1: %.3f %.3f %.3f", c[0].amp, c[0].centre, c[0].sigma);
            CHECK(std::fabs(c[1].amp - 3) < 4 * c[1].dAmp && std::fabs(c[1].centre - 60) < 4 * c[1].dCentre && std::fabs(c[1].sigma - 15) < 4 * c[1].dSigma, "comp 2: %.3f %.3f %.3f", c[1].amp, c[1].centre, c[1].sigma);
            CHECK(c[0].dCentre > 0 && c[0].dCentre < 0.5 && std::fabs(c[0].area() - 6 * 8 * 2.5066) < 4 * c[0].dArea, "area %.2f +- %.2f", c[0].area(), c[0].dArea);
            CHECK(std::fabs(f.offset - 0.2) < 4 * f.dOffset + 0.01, "offset %.3f", f.offset);
            CHECK(std::fabs(f.rms - 0.3) < 0.03, "rms %.3f", f.rms);
        }
    }
    // ---- time and sidereal time (Meeus 12.a, J2000)
    {
        Utc u; u.y = 1987; u.mo = 4; u.d = 10; u.h = 0; u.mi = 0; u.s = 0;
        const double jd = julianDate(u);
        CHECK(std::fabs(jd - 2446895.5) < 1e-9, "JD %.4f", jd);
        CHECK(std::fabs(gmstDeg(jd) - (13 + 10 / 60.0 + 46.3668 / 3600) * 15) < 1e-4, "GMST %.6f", gmstDeg(jd));
        CHECK(std::fabs(gmstDeg(2451545.0) - 280.46061837) < 1e-6, "GMST J2000");
        const Utc b = fromJulianDate(2446895.75);
        CHECK(b.y == 1987 && b.mo == 4 && b.d == 10 && b.h == 6, "back conversion %d-%d-%d %d", b.y, b.mo, b.d, b.h);
    }
    // ---- Sun (Meeus 25.a) and Moon (47.a)
    {
        const Eq s = bodyPosition(Body::Sun, 2448908.5, Eq());
        CHECK(std::fabs(s.raDeg - 198.38083) < 0.02 && std::fabs(s.decDeg + 7.78507) < 0.01, "Sun %.4f %.4f", s.raDeg, s.decDeg);
        double d = 0;
        const Eq m = bodyPosition(Body::Moon, 2448724.5, Eq(), &d);
        CHECK(std::fabs(m.raDeg - 134.688470) < 0.4 && std::fabs(m.decDeg - 13.768368) < 0.4 && std::fabs(d - 368409.7) < 6000, "Moon %.3f %.3f %.0f", m.raDeg, m.decDeg, d);
    }
    // ---- galactic <-> equatorial, solar apex
    {
        const Gal g0 = galacticFromJ2000(Eq{266.40499, -28.93617});
        CHECK(std::fabs(std::remainder(g0.lDeg, 360.0)) < 0.001 && std::fabs(g0.bDeg) < 0.001, "galactic centre %.4f %.4f", g0.lDeg, g0.bDeg);
        const Gal g = galacticFromJ2000(kSgrA);
        CHECK(std::fabs(g.lDeg - 359.944) < 0.01 && std::fabs(g.bDeg + 0.046) < 0.01, "Sgr A* l %.3f b %.3f", g.lDeg, g.bDeg);
        const Eq e = j2000FromGalactic(Gal{123.4, -33.3}), back = Eq();
        (void)back;
        const Gal g2 = galacticFromJ2000(e);
        CHECK(std::fabs(g2.lDeg - 123.4) < 1e-6 && std::fabs(g2.bDeg + 33.3) < 1e-6, "round trip");
        const Gal ap = galacticFromJ2000(solarApexJ2000());
        CHECK(std::fabs(ap.lDeg - 56.1) < 0.3 && std::fabs(ap.bDeg - 22.8) < 0.3, "IAU apex l %.2f b %.2f", ap.lDeg, ap.bDeg);
        // towards l = 0 the Sun contributes 20 cos(angle) = about 10.3 km/s
        Utc u; u.y = 2024; u.mo = 6; u.d = 1;
        const ObserverVelocity o = observerVelocity(j2000FromGalactic(Gal{0, 0}), julianDate(u), 54.7, 25.3);
        CHECK(std::fabs(o.sun - 10.3) < 0.3, "sun term %.2f", o.sun);
        const double dyn = observerVelocity(j2000FromGalactic(Gal{0, 0}), julianDate(u), 54.7, 25.3, Lsr::Dynamical).sun;
        CHECK(std::fabs(dyn - 11.1) < 0.01, "dynamical LSR towards the centre %.2f (U = 11.1)", dyn);
    }
    // ---- Earth's orbital velocity: speed at perihelion / aphelion, direction 90 deg west of the Sun at the equinox
    {
        double ev[3];
        Utc u; u.y = 2024; u.mo = 1; u.d = 3; u.h = 0;
        earthVelocity(julianDate(u), ev);
        double sp = std::sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]);
        CHECK(std::fabs(sp - 30.29) < 0.05, "perihelion speed %.3f", sp);
        u.mo = 7; u.d = 5;
        earthVelocity(julianDate(u), ev);
        sp = std::sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]);
        CHECK(std::fabs(sp - 29.29) < 0.05, "aphelion speed %.3f", sp);
        // 2024-03-20 03:06 UT equinox: apex at ecliptic longitude 270 (RA 18h, Dec -23.44): the full speed towards it, none towards RA 0h
        u.mo = 3; u.d = 20; u.h = 3; u.mi = 6;
        const double jd = julianDate(u);
        const double towards = observerVelocity(Eq{270, -23.4393}, jd, 0, 0).orbit, away = observerVelocity(Eq{90, 23.4393}, jd, 0, 0).orbit;
        CHECK(std::fabs(towards - 29.8) < 0.15 && std::fabs(away + 29.8) < 0.15, "equinox apex %.2f / %.2f", towards, away);
        // Earth rotation at the equator towards the east horizon: +0.465 (source at HA -6h, dec 0)
        const double lst = lmstDeg(jd, 0);
        const double rot = observerVelocity(Eq{lst + 90, 0}, jd, 0, 0).rotation;
        CHECK(std::fabs(rot - 0.4651) < 0.01, "rotation %.4f", rot);
    }
    // ---- transit, rise, set (Meeus 15.a, Venus at Boston 1988-03-20): transit 0.81965, rise 0.51766, set 0.12130 of the day (UT)
    {
        Utc u; u.y = 1988; u.mo = 3; u.d = 20; u.h = 0;
        const double jd0 = julianDate(u);
        const double lat = 42.3333, lon = -71.0833;
        const Transit t = events(Body::Custom, jd0, lat, lon, Eq{42.5946, 18.5}, 0);
        CHECK(t.transits && std::fabs(t.transitJd - jd0 - 0.81965) < 0.0015, "transit %.5f", t.transitJd - jd0);
        const Transit r = events(Body::Custom, jd0, lat, lon, Eq{42.2753, 18.6433}, -0.5667);
        CHECK(r.rises && std::fabs(r.riseJd - jd0 - 0.51766) < 0.004, "rise %.5f", r.riseJd - jd0);
        const Transit s = events(Body::Custom, jd0, lat, lon, Eq{41.8588, 18.4884}, -0.5667);
        CHECK(s.rises && std::fabs(s.setJd - jd0 - 0.12130) < 0.003, "set %.5f", s.setJd - jd0);
        // geometry: elevation at transit = 90 - |lat - dec|
        CHECK(std::fabs(t.transitElDeg - (90 - (lat - 18.5))) < 0.05, "transit elevation %.3f", t.transitElDeg);
        const auto vis = visibility(Body::Custom, jd0, lat, lon, Eq{42.5946, 18.5}, 144);
        double mx = -90; for (double e : vis) mx = std::max(mx, e);
        CHECK(vis.size() == 144 && std::fabs(mx - t.transitElDeg) < 0.5, "visibility peak %.2f", mx);
        // Cas A is circumpolar at Boston, Sgr A* never gets high
        const Transit c = events(Body::CasA, jd0, lat, lon, Eq(), 0);
        CHECK(c.transits && !c.rises, "Cas A circumpolar");
        CHECK(horizontal(Eq{0, 90}, jd0, 40, 10).elDeg > 39.99, "pole altitude");
    }
    // ---- l-v map: flat rotation curve 220 km/s, R0 = 8.5 kpc, first quadrant, terminal velocities and the recovered curve
    {
        LvMap m;
        const double R0 = 8.5, V0 = 220;
        for (int l = 15; l <= 80; l += 5) {
            std::vector<double> vs(400), t(400);
            for (int i = 0; i < 400; i++) vs[(size_t)i] = -200 + i;
            const double lr = l * M_PI / 180;
            for (double d = 0.05; d < 18; d += 0.05) {
                const double R = std::sqrt(R0 * R0 + d * d - 2 * R0 * d * std::cos(lr));
                if (R < 2 || R > 15) continue;
                const double vl = V0 * (R0 / R - 1) * std::sin(lr);   // flat curve: V(R) = V0
                for (int i = 0; i < 400; i++) t[(size_t)i] += 0.4 * gauss(vs[(size_t)i], 1, vl, 5);
            }
            for (auto& x : t) x += 0.2 * nd(rng);
            m.add(l, vs, t);
        }
        const auto term = terminalVelocities(m, 5, 3);
        const auto rc = rotationCurve(term, R0, V0);
        CHECK(rc.size() >= 12, "only %zu terminal velocities", rc.size());
        double worst = 0;
        for (const auto& p : rc) worst = std::max(worst, std::fabs(p.vKms - V0));
        CHECK(worst < 12, "rotation curve off by %.1f km/s", worst);
        LvMap neg; neg.add(120, {0, 1}, {0, 1});
        CHECK(!terminalVelocities(neg)[0].ok, "outer galaxy given a terminal velocity");
    }
    // ---- FITS round trip: image and table, then a whole file
    {
        std::vector<float> img(12 * 5);
        for (size_t i = 0; i < img.size(); i++) img[i] = (float)(i * 0.25 - 3);
        const std::string im = fitsImage(img, 12, 5, {{"OBJECT", "L=45 B=0"}, {"CRVAL1", "1420405751.0"}}, false);
        CHECK(im.size() % 2880 == 0, "image not block aligned");
        const std::vector<std::vector<double>> cols{{1420.0, 1420.1, 1420.2}, {-3.5, 7.25, 1e-3}};
        const std::string tb = fitsTable({"FREQ", "TA"}, {"MHz", "K"}, cols, {{"EXTNAME", "SPECTRUM"}});
        const std::string file = fitsFile({{"TELESCOP", "test"}, {"ORIGIN", "OnAir"}}, {im, tb});
        CHECK(file.compare(0, 30, "SIMPLE  =                    T") == 0, "primary header start");
        std::vector<Hdu> h;
        CHECK(fitsRead(file, h) && h.size() == 3, "read failed or %zu HDUs", h.size());
        if (h.size() == 3) {
            CHECK(h[0].cards["TELESCOP"] == "test" && h[0].image.empty(), "primary");
            CHECK(h[1].naxis.size() == 2 && h[1].naxis[0] == 12 && h[1].image.size() == img.size() && h[1].cards["OBJECT"] == "L=45 B=0", "image shape");
            bool same = true; for (size_t i = 0; i < img.size(); i++) same &= (h[1].image[i] == img[i]);
            CHECK(same, "image values");
            CHECK(h[2].isTable && h[2].colNames.size() == 2 && h[2].colNames[1] == "TA" && h[2].colUnits[0] == "MHz" && h[2].cols[0] == cols[0] && h[2].cols[1] == cols[1] && h[2].cards["EXTNAME"] == "SPECTRUM", "table");
        }
        CHECK(!fitsRead("not fits", h), "garbage accepted");
    }
    // ---- CSV, SigMF text, session
    {
        const std::string c = spectrumCsv({1420.4e6, 1420.5e6}, {0, -21}, 5.5, {1.5, 2.5}, {false, true}, "site test");
        CHECK(c.find("vel_lsr_kms") != std::string::npos && c.find("-15.5000") != std::string::npos && c.find(",1\n") != std::string::npos, "csv text");
        GaussComp g; g.amp = 5; g.centre = -20; g.sigma = 8;
        const std::string sm = sigmfMeta(1420405751, 2.4e6, "H I", {g}, "2026-10-17T00:00:00Z", 54.7, 25.3, 1000);
        CHECK(sm.find("\"core:frequency\": 1420405751.000") != std::string::npos && sm.find("onair:fwhm_kms\": 18.8") != std::string::npos, "sigmf text");
        Session s; s.info["lat"] = "54.7"; s.spectra = {{1, 2, 3.5}, {4, 5, 6}}; s.times = {10.5, 20.5};
        Session b;
        CHECK(parseSession(sessionText(s), b) && b.spectra == s.spectra && b.times == s.times && b.info["lat"] == "54.7", "session round trip");
        CHECK(!parseSession("junk", b), "junk session accepted");
    }
    return testSummary("astro hline");
}
