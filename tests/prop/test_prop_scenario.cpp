// Propagation scenario layer: presets, power units, radio noise figures, input checks, the Simple-mode verdict, the link-budget table,
// sweeps and CSV.
#include "dect2/prop/prop_scenario.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::prop;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.4f, want %.4f\n", __LINE__, #a, _a, _b); fails++; } } while (0)

static bool hasText(const std::vector<Issue>& v, const char* t, int level) {
    for (const Issue& i : v) if (i.level == level && i.text.find(t) != std::string::npos) return true;
    return false;
}
static int presetIndex(const char* name) {
    for (size_t i = 0; i < presets().size(); i++) if (std::string(presets()[i].name) == name) return (int)i;
    return -1;
}

int main() {
    // presets: every one gives a valid scenario and a range > 0; the last is Custom and keeps the numbers
    CHECK(std::string(presets().back().name) == "Custom");
    for (int i = 0; i + 1 < (int)presets().size(); i++) {
        Scenario s;
        applyPreset(s, i);
        CHECK(s.preset == i && s.fMHz == presets()[i].fMHz);
        const auto issues = validate(s);
        CHECK(!hasErrors(issues));
        s.month = 6; s.day = 21; s.hour = 12; s.useNow = false;
        const Range r = findRange(toParams(s));
        if (!r.any) printf("  preset %s: not heard anywhere\n", presets()[i].name);
        CHECK(r.any);
    }
    {
        Scenario s; s.fMHz = 1234; s.bandwidthHz = 777;
        applyPreset(s, (int)presets().size() - 1);
        CHECK(s.fMHz == 1234 && s.bandwidthHz == 777);
    }
    // sensible ranges for the everyday presets (no terrain): broadcast FM tens of km and more, PMR446 a few km, ADS-B hundreds
    auto rangeOf = [](const char* name) {
        Scenario s; applyPreset(s, presetIndex(name)); s.useNow = false; s.month = 6; s.day = 21; s.hour = 12;
        return findRange(toParams(s)).maxKm;
    };
    const double fm = rangeOf("FM broadcast"), pmr = rangeOf("PMR446"), ads = rangeOf("ADS-B (1090 MHz)"), two = rangeOf("Ham 2 m FM");
    printf("  ranges: FM %.0f km, 2 m %.0f km, PMR446 %.1f km, ADS-B %.0f km, LoRa %.1f km, DVB-T %.0f km, CB %.1f km\n", fm, two, pmr, ads,
           rangeOf("LoRa / Meshtastic"), rangeOf("DVB-T/T2 TV"), rangeOf("CB radio"));
    CHECK(fm > 60 && fm < 400);
    CHECK(pmr > 0.5 && pmr < 15);
    CHECK(ads > 250 && ads < 600);
    CHECK(two > 20 && two < 150);
    // power units
    NEAR(powerDbw(100, PowerUnit::W), 20, 1e-9);
    NEAR(powerDbw(1, PowerUnit::kW), 30, 1e-9);
    NEAR(powerDbw(13, PowerUnit::dBW), 13, 1e-9);
    NEAR(powerDbw(30, PowerUnit::dBm), 0, 1e-9);
    {
        Scenario s; s.power = 50; s.powerUnit = PowerUnit::W; s.txGainDbi = 6; s.txFeederDb = 1.5;
        const Params p = toParams(s);
        NEAR(eirpDbw(p), 10 * std::log10(50.0) + 6 - 1.5, 1e-9);
    }
    // radios: the gain setting costs noise figure, HF has its own figure
    {
        Scenario s; s.radio = 1; s.radioGainPct = 100;
        NEAR(radioNfDb(s, 100), radios()[1].nfDb, 1e-9);
        NEAR(radioNfDb(s, 7), radios()[1].nfHfDb, 1e-9);
        s.radioGainPct = 0;
        NEAR(radioNfDb(s, 100), radios()[1].nfDb + 16, 1e-9);
        s.radio = 0; s.rxNfDb = 2.5;
        NEAR(radioNfDb(s, 100), 2.5, 1e-9);
        NEAR(toParams(s).rxNfDb, 2.5, 1e-9);
    }
    // input checks with helpful messages
    {
        Scenario s; s.fMHz = 0;
        CHECK(hasErrors(validate(s)) && hasText(validate(s), "Frequency", 2));
        s = Scenario(); s.bandwidthHz = 0;
        CHECK(hasText(validate(s), "Bandwidth", 2));
        s = Scenario(); s.power = -1;
        CHECK(hasText(validate(s), "Power", 2));
        s = Scenario(); s.rxHeightM = -3;
        CHECK(hasText(validate(s), "heights", 2));
        s = Scenario(); s.txFeederDb = -1;
        CHECK(hasText(validate(s), "Feeder", 2));
        s = Scenario(); s.fMHz = 7.1; s.kIndex = 12;
        CHECK(hasText(validate(s), "K index", 2));
        s = Scenario(); s.fMHz = 7.1; s.radio = 3;   // Airspy R2 has no HF
        CHECK(hasText(validate(s), "does not tune", 1));
        s = Scenario(); s.fMHz = 10500;
        CHECK(hasText(validate(s), "rain", 0));
        s = Scenario(); applyPreset(s, presetIndex("Ham 2 m FM")); s.fMHz = 150;
        CHECK(hasText(validate(s), "outside the usual", 0) && !hasErrors(validate(s)));
        s = Scenario(); s.useNow = false; s.month = 13;
        CHECK(hasText(validate(s), "Date", 2));
        CHECK(!hasErrors(validate(Scenario())));
    }
    // verdict: plain language, a rating and reasons
    CHECK(rating(15) == 2 && rating(3) == 1 && rating(-1) == 0);
    CHECK(std::string(ratingName(2)) == "good");
    CHECK(kmText(0.4) == "400 m" && kmText(4.23) == "4.2 km" && kmText(42.4) == "42 km" && kmText(423) == "425 km" && kmText(4230) == "4250 km");
    {
        Scenario s; applyPreset(s, presetIndex("Ham 2 m FM"));
        const Params p = toParams(s);
        const Range r = findRange(p);
        const Point near = evaluate(p, 5, 0), far = evaluate(p, r.maxKm * 2, 0);
        const Verdict a = verdict(p, r, &near, "your receiver");
        CHECK(a.headline.find("You should hear it up to about") == 0);
        CHECK(a.rating == 2 && a.atRx.find("good") != std::string::npos && !a.reasons.empty());
        const Verdict b = verdict(p, r, &far, "the probe point");
        CHECK(b.rating == 0 && b.atRx.find("short") != std::string::npos);
        bool horizon = false;
        for (const auto& t : b.reasons) horizon |= t.find("horizon") != std::string::npos;
        CHECK(horizon || far.model == Model::Troposcatter || far.model == Model::Ducting);
        Params dead = p; dead.txPowerDbw = -150;
        const Verdict c = verdict(dead, findRange(dead), nullptr, nullptr);
        CHECK(c.headline.find("not be heard") != std::string::npos);
    }
    {   // HF skip zone in words
        Scenario s; applyPreset(s, presetIndex("Ham 20 m SSB"));
        s.useNow = false; s.year = 2026; s.month = 6; s.day = 21; s.hour = 10; s.env = Env::Rural;
        const Params p = toParams(s);
        const Range r = findRange(p, 90);
        CHECK(r.spans.size() >= 2);
        const Verdict v = verdict(p, r, nullptr, nullptr);
        CHECK(v.headline.find("skip zone") != std::string::npos);
        const Point mid = evaluate(p, (r.spans[0].second + r.spans[1].first) / 2, 90);
        const Verdict w = verdict(p, r, &mid, "the probe point");
        bool skip = false;
        for (const auto& t : w.reasons) skip |= t.find("skip zone") != std::string::npos;
        CHECK(skip);
        Params closed = p; closed.fMHz = 28.5; closed.sfi = 65; closed.utcHour = 23; closed.month = 12;
        const Point q = evaluate(closed, 1500, 90);
        const Verdict z = verdict(closed, findRange(closed, 90), &q, "the probe point");
        bool muf = false;
        for (const auto& t : z.reasons) muf |= t.find("MUF") != std::string::npos;
        CHECK(muf);
    }
    // link-budget table: the rows add up
    {
        Scenario s; applyPreset(s, presetIndex("Ham 70 cm FM")); s.txFeederDb = 2; s.rxClutter = Clutter::Suburban;
        const Params p = toParams(s);
        const Point q = evaluate(p, 15, 0);
        const auto rows = budget(p, q);
        auto row = [&](const char* label) { for (const auto& r : rows) if (r.label.find(label) == 0) return r.value; printf("  no row %s\n", label); fails++; return 0.0; };
        NEAR(row("EIRP"), row("Transmitter power") + row("TX feeder loss") + row("TX antenna gain"), 1e-9);
        NEAR(row("Total path loss"), row("Free-space loss") + row("Excess loss") + row("Receiver clutter") + row("Polarisation"), 1e-9);
        NEAR(row("Received power"), row("EIRP") + row("Total path loss") + row("RX antenna gain"), 1e-9);
        NEAR(row("SNR"), row("Received power") - row("Noise floor"), 1e-9);
        NEAR(row("Margin"), row("SNR") - row("Required SNR"), 1e-9);
        NEAR(row("Sensitivity"), row("Noise floor") + row("Required SNR"), 1e-9);
        for (const auto& r : rows) CHECK(!r.help.empty());
        const std::string csv = budgetCsv(rows, &rows);
        CHECK(csv.find("term,A,B,unit") == 0 && csv.find("\"Margin\"") != std::string::npos);
        CHECK(budgetCsv(rows, nullptr).find("term,value,unit") == 0);
    }
    // sweeps
    {
        Scenario s; applyPreset(s, presetIndex("Ham 2 m FM"));
        const Params p = toParams(s);
        const Series a = sweepSignalVsDistance(p, 100, 30, 0);
        CHECK(a.x.size() == 30 && a.y.front() > a.y.back());
        const Series b = sweepRangeVsPower(p, 9, 0);
        CHECK(b.x.size() == 9);
        for (size_t i = 1; i < b.y.size(); i++) CHECK(b.y[i] >= b.y[i - 1]);
        NEAR(b.x[4], std::pow(10.0, p.txPowerDbw / 10), 1e-6);
        const Series c = sweepRangeVsFrequency(p, 50, 1000, 8, 0);
        CHECK(c.y.front() > c.y.back());   // the same antennas reach farther on the lower frequency
        CHECK(seriesCsv({a, b}).find("distance (km)") != std::string::npos);
        Scenario h; applyPreset(h, presetIndex("Ham 40 m SSB"));
        h.useNow = false; h.month = 6; h.day = 21;
        const HfDay d = hfDay(toParams(h), 1000, 90);
        CHECK(d.hour.size() == 49 && d.hour.back() == 24);
        double mx = 0, mn = 1e9;
        for (size_t i = 0; i < d.muf.size(); i++) { mx = std::max(mx, d.muf[i]); mn = std::min(mn, d.muf[i]); CHECK(d.fot[i] < d.muf[i]); if (d.luf[i] > 0) CHECK(d.luf[i] <= d.muf[i]); }
        CHECK(mx > mn * 1.5);   // the MUF has a day and a night
        CHECK(hfDayCsv(d).find("utc_hour,muf_mhz") == 0);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_scenario: ok\n");
    return 0;
}
