// --mode-selftest: switches modes on the receiver of a hidden run and checks the frequency after every switch (issue: the frequency stayed on 1090 MHz
// when the mode changed). The rule under test is keepsFrequencyOnSwitch(): a switch tunes the new mode's own remembered or default frequency, and
// keeps the old one only between the satellite modes that listen to an LNB output. Also loads settings polluted by the old bug (3.1 to 3.3).
// tests/app/run_mode_switch.cmake runs it from a renamed copy of the program with its own settings. Exit code = number of failures.
#include "../app.h"
#include "state_internal.h"
#include "../platform/platform.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
int fails = 0;
void check(bool ok, const std::string& what) { if (!ok) { fails++; printf("FAIL: %s\n", what.c_str()); } }
std::string label(const ModeDef& m) { return std::string(m.name) + (m.subId ? std::string(" / ") + m.subId : ""); }
int subOf(const ModeDef& m) { return m.subId ? subIndexOf(m.subId) : 0; }
int stdOf(const ModeDef& m) { return m.family + 2; }
const ModeDef* findMode(const char* name) { for (const auto& m : allModes()) if (!strcmp(m.name, name)) return &m; return nullptr; }
void forget(App& a) { for (double& f : a.famFreq) f = 0; }
void go(App& a, const ModeDef& m) { setMode(a, m.family, subOf(m)); }
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
}  // namespace

int modeSelfTest(App& a) {
    fails = 0;
    check(!a.engine.running(), "the self test runs without a radio");
    const ModeDef* adsb = findMode("ADS-B");
    const ModeDef* air = findMode("Airband");
    const ModeDef* am = findMode("AM broadcast");
    const ModeDef* fm = findMode("FM");
    const ModeDef* dvbs = findMode("DVB-S/S2");
    const ModeDef* gen = findMode("General receiver");
    check(adsb && air && am && fm && dvbs && gen, "the modes the test needs are in this build");
    if (!(adsb && air && am && fm && dvbs && gen)) return fails;

    {   int n = 0; subModes(n);   // every protocol entry needs a slot of its own (AM broadcast shared the general receiver's frequency while there were more entries than slots)
        check(n <= kNumSubModes, "kNumSubModes (" + std::to_string(kNumSubModes) + ") covers the " + std::to_string(n) + " protocol entries"); }

    // 1. the reported sequence, from a clean receiver
    forget(a);
    go(a, *adsb);
    check(near(a.freqMhz, 1090.0), "ADS-B opens on 1090 MHz, got " + std::to_string(a.freqMhz));
    go(a, *air);
    const double airDef = a.freqMhz;
    check(airDef >= 100 && airDef < 1000, "Airband after ADS-B is on its own band, got " + std::to_string(airDef));
    go(a, *adsb);
    check(near(a.freqMhz, 1090.0), "ADS-B again: 1090 MHz, got " + std::to_string(a.freqMhz));
    go(a, *am);
    check(a.freqMhz < 1000, "AM broadcast after ADS-B is not 1090, got " + std::to_string(a.freqMhz));
    go(a, *air);
    check(near(a.freqMhz, airDef), "Airband comes back on " + std::to_string(airDef) + ", got " + std::to_string(a.freqMhz));
    a.freqMhz = 127.5;   // the user tunes Airband, leaves for ADS-B and comes back
    go(a, *adsb);
    go(a, *air);
    check(near(a.freqMhz, 127.5), "Airband remembers the user's 127.5 MHz, got " + std::to_string(a.freqMhz));

    // 2. every mode, after ADS-B on 1090 MHz and after DVB-S on 1500 / 1897 MHz: the same frequency as when entered from FM, unless the rule keeps it
    int swept = 0, noTable = 0;
    for (const double from : {1090.0, 1500.0, 1897.344}) {
        const ModeDef& src = from == 1090.0 ? *adsb : *dvbs;
        for (const auto& m : allModes()) {
            if (m.family < 0 || m.family >= kNumFamilies || &m == &src || m.family == src.family) continue;
            if (!modeTuning(stdOf(m))) { noTable++; continue; }   // a work-in-progress mode without a tuning table has no band of its own: it keeps the frequency
            forget(a); go(a, *fm); a.freqMhz = 100.0;
            go(a, m);
            const double base = a.freqMhz;
            forget(a); go(a, src); a.freqMhz = from;
            go(a, m);
            const bool keep = keepsFrequencyOnSwitch(stdOf(src), stdOf(m), from);
            check(keep ? near(a.freqMhz, from) : near(a.freqMhz, base),
                  std::string(src.name) + " " + std::to_string(from) + " -> " + label(m) + ": " + std::to_string(a.freqMhz) + " MHz, expected " + std::to_string(keep ? from : base));
            swept++;
        }
    }
    printf("swept %d mode switches (%d skipped: no tuning table)\n", swept, noTable);

    // 3. the keep rule itself
    check(keepsFrequencyOnSwitch(8, 146, 1500) && keepsFrequencyOnSwitch(8, 156, 1500) && keepsFrequencyOnSwitch(8, 49, 1500), "DVB-S 1500: kept for the sat beacon, ISDB-S and MAC");
    check(keepsFrequencyOnSwitch(8, 55, 1090) && !keepsFrequencyOnSwitch(8, 55, 1500), "DVB-S to QO-100: kept only inside QO-100's IF band");
    check(!keepsFrequencyOnSwitch(8, 146, 900) && !keepsFrequencyOnSwitch(8, 146, 2500), "outside the LNB output nothing is kept");
    check(!keepsFrequencyOnSwitch(13, 8, 1090) && !keepsFrequencyOnSwitch(8, 13, 1090) && !keepsFrequencyOnSwitch(28, 57, 1090), "ADS-B, Airband and the general receiver never keep");
    check(!keepsFrequencyOnSwitch(8, 8, 1500), "the same mode is no switch");
    for (const auto& m : allModes()) {   // a mode that is not a satellite mode never keeps anything
        if (m.family < 0 || m.family >= kNumFamilies) continue;
        const int s = stdOf(m);
        if (s == 8 || s == 49 || s == 55 || s == 146 || s == 156) continue;
        check(!keepsFrequencyOnSwitch(8, s, 1500) && !keepsFrequencyOnSwitch(s, 8, 1500), std::string("no keep between DVB-S and ") + label(m));
    }

    // 4. settings written by the old bug: Airband and the general receiver remembered 1090 / 1500 MHz, the current frequency too
    {
        plat::Prefs& p = plat::prefs();
        forget(a);
        p.setI("family", air->family); p.setS("subMode", ""); p.setD("freqMhz", 1090.0);
        p.setD(("fFreq" + slotKey(air->family)).c_str(), 1090.0);
        p.setD(("fFreq" + slotKey(gen->family)).c_str(), 1500.0);
        p.setD(("fFreq" + slotKey(adsb->family)).c_str(), 1090.0);   // ADS-B on 1090 is right
        p.setD(("fFreq" + slotKey(dvbs->family)).c_str(), 1500.0);   // so is DVB-S on 1500
        p.setI("fFreqRule", 1);
        loadPrefs(a);
        check(a.family == air->family && a.freqMhz >= 100 && a.freqMhz < 1000, "polluted settings: Airband opens on its own band, got " + std::to_string(a.freqMhz));
        check(a.famFreq[adsb->family] == 1090.0 && a.famFreq[dvbs->family] == 1500.0, "polluted settings: the right ones stay");
        go(a, *adsb);
        check(near(a.freqMhz, 1090.0), "polluted settings: ADS-B on 1090");
        go(a, *am);
        check(a.freqMhz < 1000, "polluted settings: AM broadcast not on 1090, got " + std::to_string(a.freqMhz));
        // the new rule version: a frequency the user chose outside a mode's usual band is kept
        p.setI("family", air->family); p.setD("freqMhz", 1200.0);
        p.setD(("fFreq" + slotKey(air->family)).c_str(), 1200.0); p.setI("fFreqRule", 2);
        loadPrefs(a);
        check(near(a.freqMhz, 1200.0), "rule 2: Airband at 1200 MHz is the user's choice, got " + std::to_string(a.freqMhz));
        go(a, *adsb); go(a, *air);
        check(near(a.freqMhz, 1200.0), "rule 2: Airband remembers 1200 MHz, got " + std::to_string(a.freqMhz));
    }
    printf(fails ? "mode selftest: %d failed\n" : "mode selftest: all passed\n", fails);
    return fails;
}
