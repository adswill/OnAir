// Satellite tracker logic: SGP4 against a published reference, pass search against brute force at mid, high and polar latitudes, peak filter vs horizon-to-horizon
// passes, conflicts by priority, AOS/LOS over a day boundary, stale TLEs, armed passes that survive new elements, and the run of a simulated pass.
#include "dect2/geo/sat_plan.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string fix(std::string l) {
    l.resize(68, ' ');
    int s = 0;
    for (char c : l) { if (c >= '0' && c <= '9') s += c - '0'; else if (c == '-') s++; }
    return l + char('0' + s % 10);
}

static double dist(const double a[3], const double b[3]) { return std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2])); }

// A sun-synchronous 850 km orbit (NOAA 19-like) and a 51.6 degree station orbit
static Tle polarTle() {
    Tle t;
    parseTle(fix("1 33591U 09005A   26305.50000000  .00000050  00000-0  50000-4 0  9990"), fix("2 33591  99.0000 300.0000 0014000  90.0000 270.0000 14.12500000000000"), t, "POLAR");
    return t;
}
static Tle issTle() {
    Tle t;
    parseTle(fix("1 25544U 98067A   26305.50000000  .00010000  00000-0  18000-3 0  9990"), fix("2 25544  51.6400 120.0000 0005000  80.0000 280.0000 15.50000000000000"), t, "ISS");
    return t;
}

// The passes found by stepping every second: what findPasses has to agree with
static std::vector<std::pair<double, double>> brute(const Sgp4& s, const Observer& o, double t0, double t1, double horizon) {
    std::vector<std::pair<double, double>> v;
    bool in = false; double a = 0;
    for (double t = t0; t < t1; t += 1) {
        Look l;
        if (!lookAt(s, o, t, l)) break;
        const bool up = l.elDeg >= horizon;
        if (up && !in) { in = true; a = t; }
        if (!up && in) { in = false; v.push_back({a, t}); }
    }
    return v;
}

class FakeHost : public PassHost {
public:
    bool run = false, tx = false, rec = false, canRec = true, failStart = false;
    int fam = 7; double hz = 100e6;
    int starts = 0, stops = 0, tunes = 0, recStarts = 0, recStops = 0, aos = 0, los = 0;
    std::vector<double> tuned;
    double dop = 2000;
    bool running() const override { return run; }
    bool transmitting() const override { return tx; }
    int family() const override { return fam; }
    double tunedHz() const override { return hz; }
    void setMode(int f) override { fam = f; }
    void tune(double h) override { hz = h; tuned.push_back(h); tunes++; }
    void start() override { starts++; run = !failStart; }
    void stop() override { stops++; run = false; }
    bool canRecord() const override { return canRec; }
    bool recording() const override { return rec; }
    bool startRecording(std::string& p, std::string&) override { p = "/x/pass.iq"; rec = true; recStarts++; return true; }
    void stopRecording() override { rec = false; recStops++; }
    double dopplerHz(const RunJob&, double t) const override { return dop - 4.0 * t; }   // falls 4 Hz/s
    void log(const std::string&) override {}
    void onAos(const RunJob&) override { aos++; }
    void onLos(const RunJob&, bool reached) override { if (reached) los++; }
};

static RunJob job(double aos, double los, int family = 59, double hz = 137.9e6) {
    RunJob j; j.norad = 57166; j.name = "Meteor-M N2-3"; j.mode = "LRPT"; j.family = family; j.hz = hz;
    j.pass.aos = aos; j.pass.los = los; j.pass.tca = (aos + los) / 2; j.pass.maxElDeg = 40; j.start = aos; j.end = los; j.takeOver = true;
    return j;
}

static SchedPass sp(int sat, int prio, double aos, double los, double el) {
    SchedPass p; p.sat = sat; p.priority = prio; p.pass.aos = aos; p.pass.los = los; p.pass.tca = (aos + los) / 2; p.pass.maxElDeg = el; return p;
}

int main() {
    {   // a link that exists only during events is never the default, but is chosen when it is the only one
        SatLink always; always.hz = 145.825e6; always.family = 24; SatLink event; event.hz = 145.800e6; event.family = 100025; event.eventOnly = true;
        RadioCaps none; bool h = false;
        if (bestLink({always, event}, none, BandAny, &h) != 0 || !h) { printf("FAIL: event-only link taken by default\n"); return 1; }
        if (bestLink({event}, none, BandAny, &h) != 0 || !h) { printf("FAIL: event-only link alone\n"); return 1; }
    }

    {   // SGP4 against the published verification values (Hoots & Roehrich, Spacetrack Report #3, as republished by Vallado et al. 2006: satellite 00005, 360 minutes)
        Tle t;
        CHECK(parseTle("1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753", "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", t, "VANGUARD 1"), "parse 00005");
        Sgp4 s; CHECK(s.init(t), "init 00005");
        double r[3], v[3];
        s.propagate(360.0, r, v);
        const double r1[3] = {-7154.03120202, -3783.17682504, -3536.19412294}, v1[3] = {4.741887409, -4.151817765, -2.093935425};
        CHECK(dist(r, r1) < 0.01 && dist(v, v1) < 1e-5, "00005 t=360 off by %.6f km", dist(r, r1));
    }

    const Tle polar = polarTle(), iss = issTle();
    Sgp4 sp_polar, sp_iss;
    CHECK(sp_polar.init(polar), "polar init");
    CHECK(sp_iss.init(iss), "iss init");
    const double t0 = jdToUnix(polar.epochJd);

    // The pass search against one-second brute force: every pass at Vilnius, in the high Arctic and near the pole. The coarse step far below the horizon must not lose a pass.
    struct { const char* where; double lat, lon; } places[] = {{"Vilnius", 54.69, 25.28}, {"Tromso", 69.65, 18.96}, {"Svalbard", 78.22, 15.65}, {"near the pole", 89.5, 0.0}, {"McMurdo", -77.85, 166.67}, {"equator", 0.0, 30.0}};
    int totalPolar = 0;
    for (const auto& pl : places) {
        Observer o; o.latDeg = pl.lat; o.lonDeg = pl.lon;
        const auto ref = brute(sp_polar, o, t0, t0 + 2 * 86400, 0);
        const auto got = findPasses(sp_polar, o, t0, t0 + 2 * 86400, 0);
        std::vector<Pass> usable;
        for (const Pass& p : got) if (p.aos > t0 + 1) usable.push_back(p);
        std::vector<std::pair<double, double>> refUsable;
        for (const auto& x : ref) if (x.first > t0 + 1) refUsable.push_back(x);
        // brute force at one second also finds the few-second grazes the 20 s search skips by design: every pass of 40 s or more must be found
        size_t n = 0;
        for (const auto& x : refUsable) {
            if (x.second - x.first < 40) continue;
            n++;
            bool found = false;
            for (const Pass& p : usable) if (std::fabs(p.aos - x.first) < 2 && std::fabs(p.los - x.second) < 2) found = true;
            CHECK(found, "%s: pass %.0f..%.0f (%.0f s) not found", pl.where, x.first - t0, x.second - t0, x.second - x.first);
        }
        if (pl.lat > 60 || pl.lat < -60) totalPolar += (int)n;
        CHECK(n > 0 || std::fabs(pl.lat) < 60, "%s: no pass of a polar orbit in two days", pl.where);
    }
    CHECK(totalPolar > 20, "high latitudes see a polar orbit often: %d", totalPolar);

    {   // the near-pole observer: a polar orbiter climbs high on most passes, and a pass is never negative or absurdly long
        Observer o; o.latDeg = 89.5;
        for (const Pass& p : findPasses(sp_polar, o, t0, t0 + 86400, 0)) {
            CHECK(p.los > p.aos && p.los - p.aos < 1500, "pole pass length %.0f", p.los - p.aos);
            CHECK(p.maxElDeg >= 0 && p.maxElDeg <= 90.01, "pole peak %.1f", p.maxElDeg);
        }
    }

    {   // a minimum elevation filters on the peak: AOS and LOS stay at the horizon, so nothing of the pass is cut
        Observer o; o.latDeg = 54.69; o.lonDeg = 25.28;
        const auto all = findPassesPeak(sp_iss, o, t0, t0 + 3 * 86400, 0, 0);
        const auto high = findPassesPeak(sp_iss, o, t0, t0 + 3 * 86400, 0, 20);
        CHECK(!high.empty() && high.size() < all.size(), "peak filter %zu of %zu", high.size(), all.size());
        for (const Pass& h : high) {
            CHECK(h.maxElDeg >= 20, "low peak kept %.1f", h.maxElDeg);
            bool same = false;
            for (const Pass& a : all) if (std::fabs(a.aos - h.aos) < 1 && std::fabs(a.los - h.los) < 1) same = true;
            CHECK(same, "the kept pass is horizon to horizon");
            Look l; lookAt(sp_iss, o, h.aos + 30, l);
            CHECK(l.elDeg < 20, "AOS is at the horizon, not at the filter");
        }
        // the old way (a pass cut at the threshold) is shorter
        const auto cut = findPasses(sp_iss, o, t0, t0 + 3 * 86400, 20);
        CHECK(!cut.empty() && cut[0].los - cut[0].aos < high[0].los - high[0].aos, "threshold passes are shorter");
    }

    {   // filters
        SatLink vhf; vhf.hz = 137.9e6; vhf.family = 59;
        SatLink hrpt; hrpt.hz = 1700e6; hrpt.family = 264; hrpt.needRateHz = 3e6;
        SatLink uhfNoDecoder; uhfNoDecoder.hz = 435e6; uhfNoDecoder.decoder = false;
        RadioCaps rtl; rtl.minHz = 24e6; rtl.maxHz = 1766e6; rtl.maxRateHz = 2.4e6;
        RadioCaps hack; hack.minHz = 1e6; hack.maxHz = 6e9; hack.maxRateHz = 20e6;
        CHECK(linkHearable(vhf, rtl, BandAny), "RTL hears 137 MHz");
        CHECK(!linkHearable(hrpt, rtl, BandAny), "RTL at 2.4 Msps does not reach an HRPT rate");
        CHECK(linkHearable(hrpt, hack, BandAny), "HackRF reaches HRPT");
        CHECK(!linkHearable(hrpt, hack, BandVhf), "a VHF antenna does not hear 1.7 GHz");
        CHECK(linkHearable(vhf, RadioCaps(), BandVhf), "unknown radio is not held against a link");
        bool h = false;
        CHECK(bestLink({hrpt, vhf}, hack, BandAny, &h) == 1 && h, "the easiest band first");
        CHECK(bestLink({hrpt, vhf}, hack, BandL, &h) == 0 && h, "the L-band antenna picks HRPT");
        CHECK(bestLink({uhfNoDecoder, vhf}, hack, BandAny, &h) == 1, "a link with a decoder first");
        CHECK(bestLink({uhfNoDecoder}, rtl, BandL, &h) == 0 && !h, "not hearable, still reported");
        Pass p; p.maxElDeg = 30;
        PassFilter f; f.onlyHearable = true; f.radio = rtl; f.antennaMask = BandL; f.minPeakDeg = 10;
        CHECK(!passVisible(TagWeather, false, {vhf}, p, f), "antenna filter hides it");
        f.antennaMask = BandVhf;
        CHECK(passVisible(TagWeather, false, {vhf}, p, f), "shown on VHF");
        f.tagMask = TagAmateur;
        CHECK(!passVisible(TagWeather, false, {vhf}, p, f), "kind filter");
        CHECK(passVisible(TagWeather | TagAmateur, false, {vhf}, p, f), "several kinds");
        f.tagMask = TagAll; f.minPeakDeg = 40;
        CHECK(!passVisible(TagWeather, false, {vhf}, p, f), "peak filter");
        f.minPeakDeg = 0;
        CHECK(!passVisible(TagWeather, true, {vhf}, p, f), "retired hidden");
        f.showRetired = true;
        CHECK(passVisible(TagWeather, true, {vhf}, p, f), "retired on request");
    }

    {   // stale TLEs
        CHECK(tleFreshness(1) == TleFresh::Fresh && tleFreshness(10) == TleFresh::Ageing && tleFreshness(20) == TleFresh::Stale && tleFreshness(40) == TleFresh::Expired, "age classes");
        CHECK(tleFreshness(-10) == TleFresh::Expired, "a future epoch is not trusted");
        CHECK(tleTimingErrorSec(30) > tleTimingErrorSec(3), "the error grows with age");
        Tle a = iss; a.epochJd -= 40;
        CHECK(tleFreshness(tleAgeDays(a, t0)) == TleFresh::Expired, "age from the epoch %.1f", tleAgeDays(a, t0));
        Observer o; o.latDeg = 54.69; o.lonDeg = 25.28;
        Sgp4 old; CHECK(old.init(a), "an old TLE still initialises");
        CHECK(!findPasses(old, o, t0, t0 + 86400).empty(), "an old TLE still predicts (never blocked, only flagged)");
        // the merge: a stale download never replaces a newer set, a newer one replaces it
        std::map<int, Tle> m;
        Tle n = iss; n.epochJd = 100; mergeNewestTle(m, {n});
        Tle o2 = iss; o2.epochJd = 50; mergeNewestTle(m, {o2});
        CHECK(m[25544].epochJd == 100, "older TLE ignored");
        Tle n2 = iss; n2.epochJd = 120; mergeNewestTle(m, {n2});
        CHECK(m[25544].epochJd == 120, "newer TLE wins");
    }

    {   // an armed pass survives new elements (a shift of seconds), and is not confused with the next orbit
        Pass armed; armed.tca = 10000; armed.aos = 9700; armed.los = 10300;
        Pass shifted = armed; shifted.tca += 8; Pass nextOrbit = armed; nextOrbit.tca += 6000;
        CHECK(matchPass(armed, {nextOrbit, shifted}) == 1, "matched by culmination");
        CHECK(matchPass(armed, {nextOrbit}) == -1, "not matched to the next orbit");
        CHECK(samePass(armed, shifted), "same"); CHECK(!samePass(armed, nextOrbit), "different");
    }

    {   // conflicts: an armed pass beats a higher-priority group; the loser is trimmed when enough is left
        SchedParams prm; prm.leadSec = 20; prm.minUsefulSec = 120;
        const SchedPlan pl = planPasses({sp(1, queuePriority(3, false), 1000, 1600, 80), sp(2, queuePriority(0, true), 1200, 1500, 15)}, {}, 0, prm);
        bool armedWhole = false, otherTrimmed = false;
        for (const SchedSlot& s : pl.slots) { if (s.sp.sat == 2 && !s.trimmed) armedWhole = true; if (s.sp.sat == 1 && s.trimmed) otherTrimmed = true; }
        CHECK(armedWhole, "the armed pass stays whole"); CHECK(otherTrimmed, "the other is trimmed around it");
        const SchedPlan p2 = planPasses({sp(1, 1, 1000, 1500, 50), sp(2, 5, 1000, 1500, 20)}, {}, 0, prm);
        CHECK(p2.slots.size() == 1 && p2.slots[0].sp.sat == 2 && p2.dropped.size() == 1 && p2.dropped[0].why == DropReason::Conflict, "the lower priority is skipped");
    }

    {   // AOS and LOS over a day boundary, and a reservation across midnight
        const double midnight = 86400.0 * 20000;   // 00:00 UTC of some day
        const auto busy = dailyBusy(23 * 60, 2 * 60, midnight - 86400, midnight + 86400, 0);
        bool covers = false;
        for (const BusyWindow& b : busy) if (b.start <= midnight - 600 && b.end >= midnight + 3600) covers = true;
        CHECK(covers, "a reservation that wraps past midnight");
        SchedParams prm; prm.leadSec = 20; prm.minUsefulSec = 120;
        const SchedPlan pl = planPasses({sp(1, 1, midnight - 200, midnight + 300, 40)}, {}, midnight - 3600, prm);
        CHECK(pl.slots.size() == 1 && pl.slots[0].start == midnight - 200 && pl.slots[0].end == midnight + 300, "a pass over midnight is one slot");
        FakeHost h; PassRunner r; std::vector<PassResult> done;
        RunJob j = job(midnight - 200, midnight + 300);
        for (double t = midnight - 400; t <= midnight + 500; t += 1) { bool due = t >= j.start - 20 && t < j.end; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(done.size() == 1 && done[0].outcome == PassOutcome::Completed, "completed over midnight (%zu)", done.size());
        CHECK(!r.active() && h.starts == 1, "one start");
    }

    {   // a simulated pass, receiver idle: warm-up, tune, mode, Doppler with a dead band, record, stop, restore
        FakeHost h; h.fam = 7; h.hz = 100e6;
        PassRunner r; std::vector<PassResult> done;
        const double aos = 5000, los = 5600;
        RunJob j = job(aos, los);
        double sampleAt = 0;
        for (double t = aos - 60; t <= los + 30; t += 0.5) {
            const bool due = t >= j.start - 20 && t < j.end;
            if (!due && r.active()) sampleAt = t;
            r.tick(t, due ? &j : nullptr, h, &done);
            if (t < aos - 20) CHECK(!r.active() && h.starts == 0, "nothing before the warm-up");
            if (t == aos - 19) CHECK(r.active() && h.run && h.fam == 59 && std::fabs(h.hz - (137.9e6 + 2000 - 4.0 * (aos - 20))) < 5, "warm-up: family %d tuned %.1f", h.fam, h.hz);
            if (t == aos - 10) CHECK(r.phase() == PassRunner::Phase::Warmup && h.recStarts == 0 && h.aos == 0, "not recording during the warm-up");
            if (t == aos + 1) CHECK(r.phase() == PassRunner::Phase::Receiving && h.recStarts == 1 && h.aos == 1, "AOS: receiving, recording");
        }
        CHECK(done.size() == 1, "one result %zu", done.size());
        CHECK(done[0].outcome == PassOutcome::Completed && done[0].iqPath == "/x/pass.iq", "result");
        CHECK(h.stops == 1 && !h.run && h.fam == 7 && h.recStops == 1 && h.los == 1, "stopped, mode restored, recording stopped");
        CHECK(std::fabs(h.hz - 100e6) < 1, "frequency restored %.1f", h.hz);
        const int retunes = r.retunes();
        // 4 Hz/s over 620 s = 2480 Hz: about 16 retunes with a 150 Hz dead band, not one a second
        CHECK(retunes >= 15 && retunes <= 22, "dead band: %d retunes", retunes);
        (void)sampleAt;
    }

    {   // a receiver in another mode: taken over and put back when the user armed the pass; left alone when the pass was scheduled by itself
        FakeHost h; h.run = true; h.fam = 7; h.hz = 95e6;
        PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        for (double t = 4990; t < 5620; t += 1) { bool due = t >= j.start - 20 && t < j.end; r.tick(t, due ? &j : nullptr, h, &done); if (t == 5100) CHECK(h.fam == 59, "switched"); }
        CHECK(h.fam == 7 && h.run && std::fabs(h.hz - 95e6) < 1 && h.starts == 2 && done[0].outcome == PassOutcome::Completed, "restored and restarted: fam %d run %d starts %d", h.fam, h.run, h.starts);
    }
    {
        FakeHost h; h.run = true; h.fam = 7; h.hz = 95e6;
        PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600); j.takeOver = false;
        for (double t = 4990; t < 5620; t += 1) { bool due = t >= j.start - 20 && t < j.end; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(h.tunes == 0 && h.fam == 7 && h.stops == 0 && h.starts == 0 && std::fabs(h.hz - 95e6) < 1, "a receiver in use is not touched (tunes %d)", h.tunes);
        CHECK(done.size() == 1 && done[0].outcome == PassOutcome::SkippedBusy, "reported as skipped");
    }
    {   // never interrupts a transmission
        FakeHost h; h.tx = true; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        for (double t = 4990; t < 5100; t += 1) { bool due = t >= j.start - 20; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(h.starts == 0 && h.tunes == 0 && done.size() == 1 && done[0].outcome == PassOutcome::SkippedTx, "transmitting: untouched");
    }
    {   // the receiver already on this mode: tuned and recorded, left running, back on the nominal frequency
        FakeHost h; h.run = true; h.fam = 59; h.hz = 137.5e6;
        PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        for (double t = 4990; t < 5620; t += 1) { bool due = t >= j.start - 20 && t < j.end; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(h.run && h.stops == 0 && std::fabs(h.hz - j.hz) < 1, "left running on the nominal frequency %.1f", h.hz);
    }
    {   // the user changes the mode during the pass: the run lets go and does not put anything back
        FakeHost h; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        for (double t = 4990; t < 5620; t += 1) {
            if (t == 5200) h.fam = 12;
            bool due = t >= j.start - 20 && t < j.end;
            r.tick(t, due ? &j : nullptr, h, &done);
            if (t == 5205) CHECK(!r.following() && h.recStops == 1, "let go");
        }
        CHECK(h.fam == 12 && h.stops == 0, "the user's receiver is left as it is");
    }
    {   // LoRa: the decoder follows the Doppler shift, the radio is tuned once
        FakeHost h; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600); j.radioFollowsDoppler = false;
        for (double t = 4990; t < 5620; t += 1) { bool due = t >= j.start - 20 && t < j.end; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(h.tuned.size() >= 1 && std::fabs(h.tuned[0] - j.hz) < 1, "tuned to the nominal frequency");
        CHECK(h.tuned.size() <= 2, "no retunes during the pass (%zu)", h.tuned.size());
    }
    {   // a pass is not run twice while it is still listed, and the next one runs
        FakeHost h; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        for (double t = 4990; t < 5620; t += 1) { bool due = t >= j.start - 20 && t < j.end + 15; r.tick(t, due ? &j : nullptr, h, &done); }
        CHECK(done.size() == 1 && h.starts == 1, "run once (%zu)", done.size());
        RunJob next = job(9000, 9600);
        r.tick(8990, &next, h, &done);
        CHECK(r.active(), "the next pass starts");
    }
    {   // cancelled during the pass, and an arm that joins a pass already under way
        FakeHost h; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        r.tick(5100, &j, h, &done);
        CHECK(r.active() && h.starts == 1, "joins a pass under way");
        r.tick(5101, &j, h, &done);
        r.cancel(5102, h, &done);
        CHECK(done.size() == 1 && done[0].outcome == PassOutcome::Cancelled && !h.run && h.recStops == 1, "cancelled");
    }
    {   // the receiver does not start
        FakeHost h; h.failStart = true; PassRunner r; std::vector<PassResult> done; RunJob j = job(5000, 5600);
        r.tick(4990, &j, h, &done);
        CHECK(!r.active() && done.size() == 1 && done[0].outcome == PassOutcome::Failed, "failure reported");
    }

    if (!fails) printf("sat_plan: all passed\n");
    return fails ? 1 : 0;
}
