// Mode Browser data derived from the mode table: tags (from the group, then a few by name), what a mode needs, its minimum radio rate, and
// whether the selected radio fits it. Nothing is parsed from the blurbs; the rows stay as they are (mode_picker_logic.h has the rules).
#include "../app.h"
#include "dect2/engine/mode_tuning.h"
#include "mode_picker_logic.h"
#include <cstdlib>
#include <cstring>

bool (*gModeSignalHint)(const ModeDef&, float* conf) = nullptr;

namespace {
// release tag: modes introduced in the current version show a NEW badge. Edit this list once per release.
const char* const kNewModes[] = {nullptr};
// modes that need a dish or an LNA at the feed, a cable feed or an active antenna (a note only, never blocking)
bool inList(const char* n, std::initializer_list<const char*> l) { for (const char* s : l) if (!strcmp(n, s)) return true; return false; }
}  // namespace

void modeInfoFill() {
    static int done = 0;
    if (done == kNumModes) return;
    for (int i = 0; i < kNumModes; i++) {
        ModeDef& m = kModes[i];
        if (m.tags) continue;
        m.tags = modepick::tagsOf(m.group, m.name);
        if (const dect2::ModeTuning* t = dect2::modeTuning(m.family + 2)) {
            m.minRateMsps = (float)(t->minSampleRate / 1e6);
            m.bandMinMhz = m.bandMaxMhz = t->defMhz;
        }
        if (inList(m.name, {"GOES HRIT", "Elektro-L", "DVB-S/S2"})) m.needs |= modepick::NDish;
        if (inList(m.name, {"GNSS", "Inmarsat-C", "Inmarsat Aero", "Iridium"})) m.needs |= modepick::NActiveAnt;
        if (inList(m.name, {"DVB-C"})) m.needs |= modepick::NCable;
        if (m.minRateMsps >= 8) m.needs |= modepick::NWideRadio;
        if (m.group == 14) m.needs |= modepick::NPlugin;
        for (const char* const* n = kNewModes; *n; n++) if (!strcmp(*n, m.name)) m.isNew = true;
        if (!m.tags) m.tags = 0x80000000u;   // "no tag": still a valid row (the General and custom groups)
    }
    done = kNumModes;
}

static modepick::Radio radioOf(const App& a) {
    modepick::Radio r;
    static const char* fake = getenv("DECT2_FAKE_RADIO");   // dev/test only: "rtl" = an RTL-SDR profile (24 - 1766 MHz, 3.2 Msps) for --shot runs
    if (fake && !strcmp(fake, "rtl")) { r.known = true; r.minHz = 24e6; r.maxHz = 1766e6; r.maxRateHz = 3.2e6; return r; }
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return r;
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!d.isRadio()) return r;   // test signal, file, sound card: everything is fine
    r.known = true; r.minHz = d.minFreqHz; r.maxHz = d.maxFreqHz; r.maxRateHz = d.maxRateHz;
    return r;
}

ModeFit modeFit(const App& a, const ModeDef& m) {
    modepick::ModeNeed n;
    n.minRateHz = m.minRateMsps * 1e6;
    n.defMhz = m.bandMinMhz;
    const modepick::Fit f = modepick::fit(radioOf(a), n);
    return {f.ok, f.reason};
}

bool modeCodecMissing(const ModeDef&) { return false; }   // wired to the codec check (docs/CODECS.md) later
