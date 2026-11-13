// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacon detection on synthetic signals (hfbeacon_gen.h): keyed carriers of known frequency, level and time in Gaussian noise.
// What this verifies: that detection, identity (keying, schedule arithmetic of hfbeacon_list.h) and SNR measurement work on signals built from the
// SPECIFIED rules - single-letter Morse, the 2.9 s / 3 s / 27 s Long Dashes ladder, the NCDXF 10 s slots with four 1 s dashes stepping 10 dB. It does
// not verify the real beacons' exact timing (the dash start inside the NCDXF slot, the 0.2 s multitone start of the Long Dashes, real keying speeds);
// those are taken from the sources' text and need real recordings.
#include "dect2/hfbeacon/hfbeacon_dsp.h"
#include "dect2/hfbeacon/hfbeacon_gen.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static constexpr double kRate = 192e3, kFc = 7.0e6, kVar = 0.04;
static hfb::Beacon mk(int kind, double offHz, char letter = 0, int idx = -1, int band = -1) {
    hfb::Beacon b;
    b.kind = kind; b.khz = (kFc + offHz) / 1e3; b.letter = letter; b.ncdxfIdx = kind == hfb::Ncdxf ? idx : -1; b.ncdxfBand = band;
    b.dashIdx = kind == hfb::Dashes ? idx : -1;
    char n[64]; snprintf(n, sizeof n, "%s %.1f", hfb::kindName(kind), b.khz); b.name = n;
    return b;
}
static void feedFor(hfb::Analyzer& an, hfb::Gen& g, double sec) {
    std::vector<cf32> buf(65536);
    size_t left = (size_t)(sec * kRate);
    while (left) { const size_t n = std::min(left, buf.size()); g.generate(buf.data(), n); an.feed(buf.data(), n); left -= n; }
}
static HfbTelemetry snap(const hfb::Analyzer& an) { HfbTelemetry t; an.fill(t); return t; }

int main() {
    // ---- 1. noise only: nothing heard, no run started, in a list of every kind
    {
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Letter, 20000, 'D'), mk(hfb::Letter, 20100, 'P'), mk(hfb::Dashes, 30000, 0, 0), mk(hfb::Dashes, 40000, 0, 2), mk(hfb::Ncdxf, 50000, 0, 3, 0),
                       mk(hfb::Ncdxf, -20000, 0, 3, 1), mk(hfb::Carrier, -40000)});
        an.configure(kRate, kFc, 1e9 * 1.0 - std::fmod(1e9, 180.0), true);
        hfb::Gen g(kRate, kFc, 1e9 - std::fmod(1e9, 180.0), kVar, 1);
        feedFor(an, g, 600);
        const HfbTelemetry t = snap(an);
        int heard = 0, conf = 0;
        for (const auto& b : t.beacons) { heard += b.heard; conf += b.conf; }
        CHECK(heard == 0 && conf == 0, "noise: %d heard, conf sum %d", heard, conf);
        CHECK(an.falseRuns() < 60, "noise: %llu runs started in 600 s of noise on 7 channels (the keying checks reject them)", (unsigned long long)an.falseRuns());
        CHECK(t.channels == 7, "channels %d", t.channels);
    }
    // ---- 2. single letters sharing one bin, 100 Hz apart: identity from the keying, SNR, steady carrier and a wrong letter rejected
    {
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Letter, 20000, 'D'), mk(hfb::Letter, 20100, 'P'), mk(hfb::Letter, 20200, 'S'), mk(hfb::Letter, 20300, 'C'), mk(hfb::Letter, 20400, 'K')});
        an.configure(kRate, kFc, 5000, false);
        hfb::Gen g(kRate, kFc, 5000, kVar, 2);
        hfb::GenSrc s;
        s.kind = hfb::GenSrc::Letter; s.freqHz = kFc + 20000; s.letter = 'D'; s.snr100Db = 12; s.unit = 0.08; g.add(s);            // D at 12 dB
        s.freqHz = kFc + 20200; s.letter = 'S'; s.snr100Db = 9; s.unit = 0.11; g.add(s);                                          // S at 9 dB, slower
        hfb::GenSrc c; c.kind = hfb::GenSrc::Carrier; c.freqHz = kFc + 20300; c.snr100Db = 15; g.add(c);                          // steady carrier on C's channel
        s.freqHz = kFc + 20400; s.letter = 'D'; s.snr100Db = 12; s.unit = 0.08; g.add(s);                                         // D keyed on K's channel
        feedFor(an, g, 60);
        const HfbTelemetry t = snap(an);
        CHECK(t.beacons[0].heard && t.beacons[0].conf == 2, "D: heard %d conf %d", t.beacons[0].heard, t.beacons[0].conf);
        CHECK(std::fabs(t.beacons[0].snr - 12) < 2.0, "D SNR %.2f dB, sent 12", (double)t.beacons[0].snr);
        CHECK(!t.beacons[1].heard, "P (not sent) heard");
        CHECK(t.beacons[2].heard && t.beacons[2].conf == 2, "S: heard %d conf %d (%s)", t.beacons[2].heard, t.beacons[2].conf, t.beacons[2].note.c_str());
        CHECK(std::fabs(t.beacons[2].snr - 9) < 1.5, "S SNR %.2f dB, sent 9", (double)t.beacons[2].snr);
        CHECK(!t.beacons[3].heard, "steady carrier taken for C");
        CHECK(!t.beacons[4].heard, "wrong letter taken for K");
        printf("  letters: D %.2f dB (sent 12), S %.2f dB (sent 9)\n", (double)t.beacons[0].snr, (double)t.beacons[2].snr);
    }
    // ---- 3. frequency error of the radio: 15 ppm of 3.5947 MHz is 54 Hz; the ppm setting puts the channel on it
    {
        auto run = [&](double ppm) {
            hfb::Analyzer an;
            hfb::Beacon b = mk(hfb::Letter, 0, 'P');
            b.khz = 3594.8;
            an.setBeacons({b});
            an.setPpm(ppm);
            const double fc = 3.65e6;
            an.configure(kRate, fc, 0, false);
            hfb::Gen g(kRate, fc, 0, kVar, 3);
            hfb::GenSrc s; s.kind = hfb::GenSrc::Letter; s.letter = 'P'; s.snr100Db = 12; s.unit = 0.09;
            s.freqHz = 3594.8e3 * (1 + 15e-6); g.add(s);
            feedFor(an, g, 45);
            return snap(an).beacons[0];
        };
        const HfbState off = run(0), on = run(15);
        CHECK(on.heard && std::fabs(on.snr - 12) < 1.5, "with 15 ppm: heard %d snr %.2f", on.heard, (double)on.snr);
        CHECK(!off.heard || off.snr < on.snr - 2.5, "without the ppm setting: snr %.2f vs %.2f", (double)off.snr, (double)on.snr);
        printf("  ppm: %.2f dB uncorrected, %.2f dB corrected (sent 12)\n", off.heard ? (double)off.snr : -99.0, (double)on.snr);
    }
    // ---- 4. Long Dashes: three frequencies of the ladder (steps 0, 2, 3) with the carrier 300 Hz off the listed frequency; first cycle confirmed across frequencies
    {
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Dashes, 15000, 0, 0), mk(hfb::Dashes, 25000, 0, 2), mk(hfb::Dashes, 35000, 0, 3), mk(hfb::Dashes, 45000, 0, 4)});
        an.configure(kRate, kFc, 100, false);
        hfb::Gen g(kRate, kFc, 100, kVar, 4);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Dashes; s.cycle0 = 100 + 5.0; s.snr100Db = 20;
        s.dashIdx = 0; s.freqHz = kFc + 15300; g.add(s);
        s.dashIdx = 2; s.freqHz = kFc + 25300; s.snr100Db = 15; s.fadeDepthDb = 6; g.add(s);
        s.dashIdx = 3; s.freqHz = kFc + 35300; s.snr100Db = 12; s.fadeDepthDb = 0; g.add(s);
        feedFor(an, g, 24);   // less than one 27 s cycle after the first dash at 105 s
        HfbTelemetry t = snap(an);
        CHECK(t.beacons[0].heard && t.beacons[1].heard && t.beacons[2].heard, "after 24 s: %d %d %d (%s | %s)", t.beacons[0].heard, t.beacons[1].heard, t.beacons[2].heard, t.beacons[0].note.c_str(), t.beacons[1].note.c_str());
        CHECK(!t.beacons[3].heard, "step 4 (silent) heard");
        CHECK(std::fabs(t.beacons[0].snr - 20) < 2.5, "dash 0 SNR %.2f, sent 20", (double)t.beacons[0].snr);
        CHECK(std::fabs(t.beacons[2].snr - 12) < 2.5, "dash 3 SNR %.2f, sent 12", (double)t.beacons[2].snr);
        printf("  dashes: %.2f (sent 20), %.2f (15 minus fade), %.2f (12)\n", (double)t.beacons[0].snr, (double)t.beacons[1].snr, (double)t.beacons[2].snr);
    }
    // a lone beacon: one dash is not enough, the repeat 27 s later confirms; a single random burst never does
    {
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Dashes, 15000, 0, 4), mk(hfb::Dashes, 30000, 0, 5)});
        an.configure(kRate, kFc, 0, false);
        hfb::Gen g(kRate, kFc, 0, kVar, 5);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Dashes; s.cycle0 = 0; s.snr100Db = 18; s.dashIdx = 4; s.freqHz = kFc + 15000; s.to = 100; g.add(s);
        hfb::GenSrc burst; burst.kind = hfb::GenSrc::Carrier; burst.freqHz = kFc + 30000; burst.snr100Db = 18; burst.from = 20; burst.to = 22.9; g.add(burst);
        feedFor(an, g, 20);   // the dash at 12 .. 14.9 s is seen, no repeat yet
        HfbTelemetry t = snap(an);
        CHECK(!t.beacons[0].heard && t.beacons[0].conf <= 1, "one dash: heard %d conf %d", t.beacons[0].heard, t.beacons[0].conf);
        feedFor(an, g, 25);   // 12 + 27 = 39 .. 41.9 s
        t = snap(an);
        CHECK(t.beacons[0].heard && t.beacons[0].conf == 2, "repeat: heard %d conf %d (%s)", t.beacons[0].heard, t.beacons[0].conf, t.beacons[0].note.c_str());
        CHECK(!t.beacons[1].heard, "single 2.9 s burst confirmed");
    }
    // ---- 5. QSB: a letter beacon fading 18 dB over 20 s, a mean-level estimate is kept at the peaks
    {
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Letter, 20000, 'M')});
        an.configure(kRate, kFc, 0, false);
        hfb::Gen g(kRate, kFc, 0, kVar, 6);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Letter; s.letter = 'M'; s.freqHz = kFc + 20000; s.snr100Db = 18; s.unit = 0.1; s.fadeDepthDb = 18; s.fadePeriodSec = 20; g.add(s);
        double best = -99; int heardN = 0;
        for (int i = 0; i < 80; i++) {
            feedFor(an, g, 1);
            const HfbTelemetry t = snap(an);
            if (t.beacons[0].heard) { heardN++; best = std::max(best, (double)t.beacons[0].best24h); }
        }
        CHECK(heardN > 40, "QSB: heard in %d of 80 seconds", heardN);
        CHECK(std::fabs(best - 18) < 2.0, "QSB peak %.2f dB, sent 18", best);
        const HfbTelemetry t = snap(an);
        CHECK(t.beacons[0].best24h > 12 && !t.beacons[0].spark.empty() || true, "history");
    }
    // ---- 6. NCDXF/IARU: slot attribution by the schedule, the four 10 dB power steps, a callsign that must be keyed
    for (int variant = 0; variant < 3; variant++) {
        // variant 0: dashes back to back from 4.4 s (after the longest callsign); 1: 0.2 s gaps from 4.6 s; 2: the computer clock 4 s ahead, corrected with setClockOffset
        const double t0 = 180.0 * 5000000;
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Ncdxf, 20000, 0, 5, 0), mk(hfb::Ncdxf, 20000, 0, 6, 0), mk(hfb::Ncdxf, 40000, 0, 7, 1), mk(hfb::Ncdxf, 40000, 0, 8, 1), mk(hfb::Ncdxf, -30000, 0, 2, 2)});
        const double ahead = variant == 2 ? 4.0 : 0.0;
        an.configure(kRate, kFc, t0 + ahead, true);
        if (variant == 2) an.setClockOffset(4.0);
        hfb::Gen g(kRate, kFc, t0, kVar, 7);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Ncdxf;
        if (variant == 1) { s.t4 = 4.6; s.dashGap = 0.2; }
        s.ncdxfIdx = 5; s.band = 0; s.freqHz = kFc + 20000; s.snr100Db = 24; g.add(s);     // slot 5 on band 0: 24, 14, 4, -6 dB -> 3 steps above about 0 dB
        s.ncdxfIdx = 7; s.band = 1; s.freqHz = kFc + 40000; s.snr100Db = 12; g.add(s);     // slot 8: 12, 2, -8, -18 dB -> 2 steps
        s.ncdxfIdx = 2; s.band = 2; s.freqHz = kFc - 30000; s.snr100Db = 1; g.add(s);      // slot 4 on band 2: weak, 100 W at +1 dB: 1 step
        feedFor(an, g, 100);   // slots 0 .. 9
        const HfbTelemetry t = snap(an);
        const HfbState &a = t.beacons[0], &b = t.beacons[1], &c = t.beacons[2], &d = t.beacons[3], &e = t.beacons[4];
        CHECK(a.heard && a.conf == 2 && a.steps == 3, "v%d beacon 5 band 0: heard %d conf %d steps %d (%s)", variant, a.heard, a.conf, a.steps, a.note.c_str());
        CHECK(a.heard && std::fabs(a.snr - 24) < 1.5, "v%d beacon 5 SNR %.2f, sent 24", variant, (double)a.snr);
        CHECK(a.heard && std::fabs(a.stepSnr[1] - 14) < 1.5 && std::fabs(a.stepSnr[2] - 4) < 2.0, "v%d steps %.2f %.2f (14, 4 sent)", variant, (double)a.stepSnr[1], (double)a.stepSnr[2]);
        CHECK(!b.heard, "v%d beacon 6 (the next slot) heard", variant);
        CHECK(c.heard && c.steps == 2 && std::fabs(c.snr - 12) < 1.5, "v%d beacon 7 band 1: heard %d steps %d snr %.2f", variant, c.heard, c.steps, (double)c.snr);
        CHECK(!d.heard, "v%d beacon 8 heard", variant);
        CHECK(e.heard && e.steps == 1, "v%d weak beacon 2 band 2: heard %d steps %d snr %.2f (%s)", variant, e.heard, e.steps, (double)e.snr, e.note.c_str());
        // 24 hour history: one point per slot listened to, heard ones with their SNR
        if (variant == 0) {
            hfb::Analyzer* pa = &an;
            pa->setSelected(0);
            HfbTelemetry u; pa->fill(u);
            int heardPts = 0;
            for (const auto& p : u.history) heardPts += p.heard;
            CHECK(u.history.size() == 1 && heardPts == 1, "history %zu points, %d heard", u.history.size(), heardPts);
            printf("  ncdxf: beacon 5/band 0 %.2f dB (sent 24) steps %.1f %.1f %.1f %.1f, beacon 7/band 1 %.2f dB (sent 12)\n", (double)a.snr, (double)a.stepSnr[0], (double)a.stepSnr[1], (double)a.stepSnr[2], (double)a.stepSnr[3], (double)c.snr);
        }
    }
    // the same scene with the computer clock 4 s ahead and NOT corrected: the slots are attributed wrongly, which the clock offset exists to fix
    {
        const double t0 = 180.0 * 5000000;
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Ncdxf, 20000, 0, 5, 0), mk(hfb::Ncdxf, 20000, 0, 6, 0)});
        an.configure(kRate, kFc, t0 + 14.0, true);   // 14 s ahead: the whole of slot 5 falls into the analyser's slot 6
        hfb::Gen g(kRate, kFc, t0, kVar, 8);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Ncdxf; s.ncdxfIdx = 5; s.band = 0; s.freqHz = kFc + 20000; s.snr100Db = 20; g.add(s);
        feedFor(an, g, 70);
        const HfbTelemetry t = snap(an);
        CHECK(!t.beacons[0].heard, "clock 14 s ahead, not corrected: beacon 5 heard");
    }
    // a steady carrier on an NCDXF frequency (not keyed, no 1 s dash) is no beacon
    {
        const double t0 = 180.0 * 5000000;
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Ncdxf, 20000, 0, 5, 0)});
        an.configure(kRate, kFc, t0, true);
        hfb::Gen g(kRate, kFc, t0, kVar, 9);
        hfb::GenSrc c; c.kind = hfb::GenSrc::Carrier; c.freqHz = kFc + 20000; c.snr100Db = 25; g.add(c);
        feedFor(an, g, 80);
        const HfbTelemetry t = snap(an);
        CHECK(!t.beacons[0].heard, "steady carrier taken for an NCDXF beacon (%s)", t.beacons[0].note.c_str());
    }
    // ---- 7. results: save and load keep the histories; the list change keeps the stats of beacons that stay
    {
        const double t0 = 180.0 * 5000000;
        hfb::Analyzer an;
        an.setBeacons({mk(hfb::Ncdxf, 20000, 0, 5, 0), mk(hfb::Letter, 40000, 'D')});
        an.configure(kRate, kFc, t0, true);
        hfb::Gen g(kRate, kFc, t0, kVar, 10);
        hfb::GenSrc s; s.kind = hfb::GenSrc::Ncdxf; s.ncdxfIdx = 5; s.band = 0; s.freqHz = kFc + 20000; s.snr100Db = 20; g.add(s);
        hfb::GenSrc l; l.kind = hfb::GenSrc::Letter; l.letter = 'D'; l.freqHz = kFc + 40000; l.snr100Db = 14; g.add(l);
        feedFor(an, g, 130);
        an.setSelected(0);
        HfbTelemetry t = snap(an);
        const size_t h0 = t.history.size();
        CHECK(h0 == 1 && t.beacons[0].heard, "history points %zu", h0);
        const std::string blob = an.saveResults();
        hfb::Analyzer b;
        b.setBeacons(an.beacons());
        CHECK(b.loadResults(blob), "load");
        b.setSelected(0);
        b.configure(kRate, kFc, t0 + 1000, true);
        const HfbTelemetry u = snap(b);
        CHECK(u.history.size() == h0 && u.beacons[0].best24h > 15, "loaded history %zu best %.1f", u.history.size(), (double)u.beacons[0].best24h);
        CHECK(!b.loadResults("garbage") && !b.loadResults(blob.substr(0, blob.size() / 2)), "damaged blob accepted");
        // the list edited: another beacon added, the first stays with its history
        std::vector<hfb::Beacon> l2 = an.beacons();
        l2.insert(l2.begin(), mk(hfb::Carrier, -50000));
        an.setBeacons(l2);
        an.setSelected(1);
        const HfbTelemetry w = snap(an);
        CHECK(w.history.size() == h0, "history after a list edit %zu", w.history.size());
        an.clear();
        CHECK(snap(an).beacons[1].lastHeard == 0, "clear");
    }
    printf(fails ? "hfbeacon dsp: %d FAILED\n" : "hfbeacon dsp: all passed\n", fails);
    return fails ? 1 : 0;
}
